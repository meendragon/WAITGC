// SPDX-License-Identifier: GPL-2.0-only

#include <linux/vmalloc.h>
#include <linux/slab.h>     /* kzalloc, kfree */
#include <linux/ktime.h>
#include <linux/sched/clock.h>
#include <linux/limits.h>   /* ULLONG_MAX */
#include <linux/string.h>   /* memset */
#include "nvmev.h"
#include "conv_ftl.h"

extern uint gc_policy;  

static inline bool last_pg_in_wordline(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static bool should_gc(struct conv_ftl *conv_ftl)
{
	return (conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines);
}

static inline bool should_gc_high(struct conv_ftl *conv_ftl)
{
	return conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines_high;
}

static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	return conv_ftl->maptbl[lpn];
}

static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	NVMEV_ASSERT(lpn < conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint64_t pgidx;

	NVMEV_DEBUG_VERBOSE("%s: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", __func__,
			ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

	pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
		ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

	NVMEV_ASSERT(pgidx < spp->tt_pgs);

	return pgidx;
}

static inline uint64_t get_rmap_ent(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	return conv_ftl->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	conv_ftl->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
	return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct line *)a)->pos = pos;
}

static inline void consume_write_credit(struct conv_ftl *conv_ftl)
{
	conv_ftl->wfc.write_credits--;
}

static void foreground_gc(struct conv_ftl *conv_ftl);

static inline void check_and_refill_write_credit(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	if (wfc->write_credits <= 0) {
		foreground_gc(conv_ftl);

		wfc->write_credits += wfc->credits_to_refill;
	}
}

static void init_lines(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line;
	int i;

	lm->tt_lines = spp->blks_per_pl;
	NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
					 victim_line_set_pri, victim_line_get_pos,
					 victim_line_set_pos);

	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct line){
			.id = i,
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
			/*MN Start*/
			.last_modified_time = 0,
			.poi = 0,
			/*MN End*/
		};

		/* initialize all the lines as free lines */
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static void remove_lines(struct conv_ftl *conv_ftl)
{
	pqueue_free(conv_ftl->lm.victim_line_pq);
	vfree(conv_ftl->lm.lines);
}

static void init_write_flow_control(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
}

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct conv_ftl *conv_ftl)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

	if (!curline) {
		NVMEV_ERROR("No free line left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	NVMEV_DEBUG("%s: free_line_cnt %d\n", __func__, lm->free_line_cnt);
	return curline;
}

static struct write_pointer *__get_wp(struct conv_ftl *ftl, uint32_t io_type)
{
	if (io_type == USER_IO) {
		return &ftl->wp;
	} else if (io_type == GC_IO) {
		return &ftl->gc_wp;
	}

	NVMEV_ASSERT(0);
	return NULL;
}

static void prepare_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct write_pointer *wp = __get_wp(conv_ftl, io_type);
	struct line *curline = get_next_free_line(conv_ftl);

	NVMEV_ASSERT(wp);
	NVMEV_ASSERT(curline);

	/* wp->curline is always our next-to-write super-block */
	*wp = (struct write_pointer){
		.curline = curline,
		.ch = 0,
		.lun = 0,
		.pg = 0,
		.blk = curline->id,
		.pl = 0,
	};
}

static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct write_pointer *wpp = __get_wp(conv_ftl, io_type);

	NVMEV_DEBUG_VERBOSE("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0)
		goto out;

	wpp->pg -= spp->pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->nchs);
	wpp->ch++;
	if (wpp->ch != spp->nchs)
		goto out;

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch)
		goto out;

	wpp->lun = 0;
	/* go to next wordline in the block */
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk)
		goto out;

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == spp->pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG_VERBOSE("wpp: move line to full_line_list\n");
	} else {
		NVMEV_DEBUG_VERBOSE("wpp: line is moved to victim list\n");
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
	wpp->curline = get_next_free_line(conv_ftl);
	NVMEV_DEBUG_VERBOSE("wpp: got new clean line %d\n", wpp->curline->id);

	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG_VERBOSE("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct ppa ppa;
	struct write_pointer *wp = __get_wp(conv_ftl, io_type);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

static void init_maptbl(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	conv_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
}

static void remove_maptbl(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->maptbl);
}

static void init_rmap(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	conv_ftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->rmap[i] = INVALID_LPN;
	}
}

static void remove_rmap(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->rmap);
}

static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd)
{
	/*copy convparams*/
	conv_ftl->cp = *cpp;

	conv_ftl->ssd = ssd;
	/*MN Start*/
	conv_ftl->gc_policy = gc_policy;   /* insmod 파라미터 주입 */
	conv_ftl->cur_nsecs = 0;

	/* [A] WAF/부하 */
	conv_ftl->gc_cnt            = 0;
	conv_ftl->gc_valid_copied   = 0;
	conv_ftl->host_write_pages  = 0;
	conv_ftl->gc_victim_vpc_sum = 0;
	/* [B] 정책 차이 */
	conv_ftl->sel_diff_g_vs_cb  = 0;
	/* [C] age 신호 */
	conv_ftl->cb_age_sum        = 0;
	/* [D] age 분포 */
	memset(conv_ftl->cand_age_hist,   0, sizeof(conv_ftl->cand_age_hist));
	memset(conv_ftl->victim_age_hist, 0, sizeof(conv_ftl->victim_age_hist));
	memset(conv_ftl->victim_vpc_sum,  0, sizeof(conv_ftl->victim_vpc_sum));
	memset(conv_ftl->cand_vpc_sum,    0, sizeof(conv_ftl->cand_vpc_sum));
	/* [E] RIPEN Phase 0 */
	ripen_init(&conv_ftl->ripen, ssd->sp.tt_pgs);
	/*MN End*/
	/* initialize maptbl */
	init_maptbl(conv_ftl); // mapping table

	/* initialize rmap */
	init_rmap(conv_ftl); // reverse mapping table (?)

	/* initialize all the lines */
	init_lines(conv_ftl);

	/* initialize write pointer, this is how we allocate new pages for writes */
	prepare_write_pointer(conv_ftl, USER_IO);
	prepare_write_pointer(conv_ftl, GC_IO);

	init_write_flow_control(conv_ftl);

	NVMEV_INFO("Init FTL instance with %d channels (%ld pages)\n", conv_ftl->ssd->sp.nchs,
		   conv_ftl->ssd->sp.tt_pgs);

	return;
}

static void conv_remove_ftl(struct conv_ftl *conv_ftl)
{
	ripen_free(&conv_ftl->ripen);   /*MN RIPEN Phase 0 */
	remove_lines(conv_ftl);
	remove_rmap(conv_ftl);
	remove_maptbl(conv_ftl);
}

static void conv_init_params(struct convparams *cpp)
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
	cpp->enable_gc_delay = 1;
	cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100);
}

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct convparams cpp;
	struct conv_ftl *conv_ftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	ssd_init_params(&spp, size, nr_parts);
	conv_init_params(&cpp);

	conv_ftls = kmalloc(sizeof(struct conv_ftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
		ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
		ssd_init(ssd, &spp, cpu_nr_dispatcher);
		conv_init_ftl(&conv_ftls[i], &cpp, ssd);
	}

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		kfree(conv_ftls[i].ssd->pcie->perf_model);
		kfree(conv_ftls[i].ssd->pcie);
		kfree(conv_ftls[i].ssd->write_buffer);

		conv_ftls[i].ssd->pcie = conv_ftls[0].ssd->pcie;
		conv_ftls[i].ssd->write_buffer = conv_ftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)conv_ftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = conv_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
		   size, ns->size, cpp.pba_pcent);

	return;
}

void conv_remove_namespace(struct nvmev_ns *ns)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	const uint32_t nr_parts = SSD_PARTITIONS;
	uint32_t i;

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		/*
		 * These were freed from conv_init_namespace() already.
		 * Mark these NULL so that ssd_remove() skips it.
		 */
		conv_ftls[i].ssd->pcie = NULL;
		conv_ftls[i].ssd->write_buffer = NULL;
	}

	for (i = 0; i < nr_parts; i++) {
		conv_remove_ftl(&conv_ftls[i]);
		ssd_remove(conv_ftls[i].ssd);
		kfree(conv_ftls[i].ssd);
	}

	kfree(conv_ftls);
	ns->ftls = NULL;
}

static inline bool valid_ppa(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	//int sec = ppa->g.sec;

	if (ch < 0 || ch >= spp->nchs)
		return false;
	if (lun < 0 || lun >= spp->luns_per_ch)
		return false;
	if (pl < 0 || pl >= spp->pls_per_lun)
		return false;
	if (blk < 0 || blk >= spp->blks_per_pl)
		return false;
	if (pg < 0 || pg >= spp->pgs_per_blk)
		return false;

	return true;
}

static inline bool valid_lpn(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	return (lpn < conv_ftl->ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct line *get_line(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	return &(conv_ftl->lm.lines[ppa->g.blk]);
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct line *line;

	/* update corresponding page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	line->last_modified_time = conv_ftl->cur_nsecs;  /*MN 마지막 invalidation 시각 */
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
	if (line->vpc == spp->pgs_per_line) {
		NVMEV_ASSERT(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
		/* Note that line->vpc will be updated by this call */
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		line->vpc--;
	}

	if (was_full_line) {
		/* move line: "full" -> "victim" */
		list_del_init(&line->entry);
		lm->full_line_cnt--;
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}
}

static void mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	/* update page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_FREE);
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
	blk->vpc++;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
	line->vpc++;
}

static void mark_block_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = get_blk(conv_ftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	for (i = 0; i < spp->pgs_per_blk; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	/* reset block status */
	NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

static void gc_read_page(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	/* advance conv_ftl status, we don't care about how long it takes */
	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz,
			.interleave_pci_dma = false,
			.ppa = ppa,
		};
		ssd_advance_nand(conv_ftl->ssd, &gcr);
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);

	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));
	new_ppa = get_new_page(conv_ftl, GC_IO);
	/* update maptbl */
	set_maptbl_ent(conv_ftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(conv_ftl, lpn, &new_ppa);

	/*MN Start*/
	mark_page_valid(conv_ftl, &new_ppa);
    conv_ftl->gc_valid_copied++;
	/*MN End*/
	/* need to advance the write pointer here */
	advance_write_pointer(conv_ftl, GC_IO);

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &new_ppa,
		};
		if (last_pg_in_wordline(conv_ftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(conv_ftl->ssd, &gcw);
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(conv_ftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(conv_ftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

	return 0;
}

/* ============================================================
 * MN: age 진단 헬퍼 (정책 무관)
 * ============================================================
 * age 정의는 line->last_modified_time (마지막 invalidation 시각) 그대로 유지.
 * flush 로그에서 "age가 살아있나 / 0에 몰렸나"를 보고 cost-benefit이
 * greedy로 붕괴하는지 판정하기 위한 계측만 추가한다. */

static inline uint64_t line_age_ns(struct conv_ftl *conv_ftl, struct line *ln)
{
	uint64_t now = conv_ftl->cur_nsecs;
	return (now > ln->last_modified_time) ? (now - ln->last_modified_time) : 0;
}

/* floor(log2(age_ns)) 버킷. fls64(x)-1 = floor(log2(x)). age=0 → 버킷 0.
 * 버킷 b ≈ age ∈ [2^b, 2^(b+1)) ns. b=20≈1ms, b=30≈1s, b=33≈8s. */
static inline int age_to_hist_bucket(uint64_t age_ns)
{
	int b;
	if (age_ns == 0)
		return 0;
	b = fls64(age_ns) - 1;
	if (b < 0)
		b = 0;
	if (b >= AGE_HIST_BUCKETS)
		b = AGE_HIST_BUCKETS - 1;
	return b;
}

/* GC 시점 victim_pq 후보 전체의 age 분포 기록.
 * 정책 무관하게 do_gc 진입 시 호출 → greedy로 돌려도 중립적 후보 분포를 얻는다.
 * step 경계는 이 분포의 골(valley)에 놓아야 hot/cold가 갈린다. */
static void record_candidate_age_hist(struct conv_ftl *conv_ftl)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	pqueue_t *pq = lm->victim_line_pq;
	size_t i;
	for (i = 1; i <= pq->size; i++) {
		struct line *ln = (struct line *)pq->d[i];
		int b;
		if (!ln)
			continue;
		b = age_to_hist_bucket(line_age_ns(conv_ftl, ln));
		conv_ftl->cand_age_hist[b]++;
		conv_ftl->cand_vpc_sum[b] += ln->vpc;
	}
}

/* ---- age → 이산 가중치 (계단 함수) ----
 * raw age(ns)를 그대로 나누면 dynamic range(1ms~수십초 = 1e4배 이상)가
 * u/(1-u) 항(고작 수십~수백배)을 압도해 정렬이 사실상 1/age 로 붕괴한다.
 * age를 소수의 이산 weight로 눌러 utilization 항과 스케일을 맞춘다.
 * 경계/가중치는 flush 로그의 AGEHIST(cand_age_hist)를 보고 조정할 것. */
#define MS_TO_NS(x)  ((uint64_t)(x) * 1000000ULL)
#define SEC_TO_NS(x) ((uint64_t)(x) * 1000000000ULL)

#define CB_TH_VERY_HOT  MS_TO_NS(4)      /* 방금 씀: locality로 곧 재기록 → 손대지 마 */
#define CB_TH_HOT       MS_TO_NS(64)    /* 아직 활성 */
#define CB_TH_WARM      SEC_TO_NS(2)   /* 식어가는 중 */

static inline uint64_t cb_age_weight(uint64_t age_ns)
{
	if (age_ns < CB_TH_VERY_HOT) return 1;    /* 건드리지 말 것 */
	if (age_ns < CB_TH_HOT)      return 5;
	if (age_ns < CB_TH_WARM)     return 20;
	return 100;                                /* cold/static: GC 1순위 */
}
static inline uint64_t cb_score(struct conv_ftl *conv_ftl, struct line *ln)
{
	uint64_t w = cb_age_weight(line_age_ns(conv_ftl, ln));
	//
	return (w * (uint64_t)ln->ipc) / ((uint64_t)ln->vpc + 1);
}

static inline uint64_t cb_score_raw(struct conv_ftl *conv_ftl, struct line *ln)
{
	uint64_t age = line_age_ns(conv_ftl, ln);
	return (age * (uint64_t)ln->ipc) / ((uint64_t)ln->vpc + 1);
}

static inline uint64_t cb_score_raw_norm(struct conv_ftl *conv_ftl, struct line *ln)
{
	uint64_t age = line_age_ns(conv_ftl, ln);

	/* age를 ms 규모로 축소: ns >> 20 ≈ /1,048,576 (약 1ms 단위) */
	uint64_t age_scaled = age >> 20;

	const uint64_t SCALE = 100;          /* step function 최대 weight와 정합 */
	uint64_t w = age_scaled;
	if (w > SCALE) w = SCALE;            /* 상한 clamp → 유계 */
	if (w == 0) w = 1;                   /* 0 방지 */

	return (w * (uint64_t)ln->ipc) / ((uint64_t)ln->vpc + 1);
}

/* ============================================================
 * MN: "만약 반대 정책이었다면 뭘 골랐을까"를 활성 정책과 무관하게 계산.
 * pq를 훑어 greedy 선택(vpc 최소)과 cost-benefit 선택(score 최대)을
 * 둘 다 찾아 포인터로 돌려준다. do_gc가 실제 선택 후 이걸 호출해
 * 두 결과가 달랐는지(sel_diff_g_vs_cb) 집계 → 어떤 정책으로 돌려도
 * "두 정책이 실제로 다른 victim을 고르는가"를 flush 로그에서 확인 가능.
 * ============================================================ */
static void peek_both_policies(struct conv_ftl *conv_ftl,
			       struct line **greedy_pick,
			       struct line **cb_pick)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	pqueue_t *pq = conv_ftl->lm.victim_line_pq;
	struct line *g = NULL, *c = NULL;
	int gmin = spp->pgs_per_line + 1;
	uint64_t cbest = 0;
	size_t i;

	for (i = 1; i <= pq->size; i++) {
		struct line *ln = (struct line *)pq->d[i];
		uint64_t s;
		if (!ln)
			continue;
		if (ln->vpc < gmin) { gmin = ln->vpc; g = ln; }
		s = cb_score(conv_ftl, ln);
		if (c == NULL || s > cbest) { cbest = s; c = ln; }
	}
	*greedy_pick = g;
	*cb_pick     = c;
}

/*MN Start*/
static struct line *select_victim_greedy(struct conv_ftl *conv_ftl, bool force);
static struct line *select_victim_cost_benefit_linear(struct conv_ftl *conv_ftl, bool force);
static struct line *select_victim_cost_benefit_step(struct conv_ftl *conv_ftl, bool force);
static struct line *select_victim_ip_step(struct conv_ftl *conv_ftl, bool force);

static struct line *select_victim_line(struct conv_ftl *conv_ftl, bool force)
{
	switch (conv_ftl->gc_policy) {
	case GC_COST_BENEFIT_LINEAR:  return select_victim_cost_benefit_linear(conv_ftl, force);
	case GC_COST_BENEFIT_STEP:        return select_victim_cost_benefit_step(conv_ftl, force);
	case GC_IP_STEP:	return select_victim_ip_step(conv_ftl, force);
	case GC_GREEDY:
	default:               return select_victim_greedy(conv_ftl, force);
	}
}

/* ---- Greedy: pqueue top = vpc 최소 ---- */
static struct line *select_victim_greedy(struct conv_ftl *conv_ftl, bool force)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *victim_line = NULL;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line)
		return NULL;

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8)))
		return NULL;

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;

	return victim_line;
}

/* ---- Cost-Benefit: score = (age_weight * ipc) / (vpc + 1), 클수록 좋은 victim ----
 * greedy의 vpc 상한 컷은 적용하지 않는다(greedy로 수렴 방지). */
static struct line *select_victim_cost_benefit_linear(struct conv_ftl *conv_ftl, bool force)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	pqueue_t *pq = lm->victim_line_pq;
	struct line *best = NULL;
	uint64_t best_score = 0;
	size_t i;

	if (pq->size == 0)
		return NULL;

	for (i = 1; i <= pq->size; i++) {
		struct line *ln = (struct line *)pq->d[i];
		uint64_t score;
		if (!ln)
			continue;
		score = cb_score_raw_norm(conv_ftl, ln);
		if (best == NULL || score > best_score) {
			best_score = score;
			best = ln;
		}
	}

	if (!best)
		return NULL;

	pqueue_remove(pq, best);
	best->pos = 0;
	lm->victim_line_cnt--;

	return best;
}

static struct line *select_victim_cost_benefit_step(struct conv_ftl *conv_ftl, bool force)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	pqueue_t *pq = lm->victim_line_pq;
	struct line *best = NULL;
	uint64_t best_score = 0;
	size_t i;

	if (pq->size == 0)
		return NULL;

	for (i = 1; i <= pq->size; i++) {
		struct line *ln = (struct line *)pq->d[i];
		uint64_t score;
		if (!ln)
			continue;
		score = cb_score(conv_ftl, ln);
		if (best == NULL || score > best_score) {
			best_score = score;
			best = ln;
		}
	}

	if (!best)
		return NULL;

	pqueue_remove(pq, best);
	best->pos = 0;
	lm->victim_line_cnt--;

	return best;
}

/* ---- IP-STEP: point(age) = SCALE*(1-P[bucket]) 기반 victim 선택 ----
 * Phase A(현재): P[b] 관측 단계. point 미반영 → greedy 위임으로
 *   실행 안전성만 확보한다. flush 로그의 P_inv_x1k 컬럼이
 *   "P[b]가 bucket에 따라 벌어지는가"를 먼저 답한다.
 * Phase B(예정): offline 관측 P[b]를 P_lut[40] 상수 테이블로 baking하고
 *   score = point(age)*ipc/(vpc+1) 최대 선택으로 이 함수를 교체. */
static struct line *select_victim_ip_step(struct conv_ftl *conv_ftl, bool force)
{
	return select_victim_greedy(conv_ftl, force);
}
/*MN End*/


/* here ppa identifies the block we want to clean */
static void clean_one_block(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;
	int pg;

	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
		ppa->g.pg = pg;
		pg_iter = get_pg(conv_ftl->ssd, ppa);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID) {
			gc_read_page(conv_ftl, ppa);
			/* delay the maptbl update until "write" happens */
			gc_write_page(conv_ftl, ppa);
			cnt++;
		}
	}

	NVMEV_ASSERT(get_blk(conv_ftl->ssd, ppa)->vpc == cnt);
}

/* here ppa identifies the block we want to clean */
static void clean_one_flashpg(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0, i = 0;
	uint64_t completed_time = 0;
	struct ppa ppa_copy = *ppa;

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID)
			cnt++;

		ppa_copy.g.pg++;
	}

	ppa_copy = *ppa;

	if (cnt <= 0)
		return;

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz * cnt,
			.interleave_pci_dma = false,
			.ppa = &ppa_copy,
		};
		completed_time = ssd_advance_nand(conv_ftl->ssd, &gcr);
	}

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
			gc_write_page(conv_ftl, &ppa_copy);
		}

		ppa_copy.g.pg++;
	}
}

static void mark_line_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line = get_line(conv_ftl, ppa);
	line->ipc = 0;
	line->vpc = 0;
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
}
/*MN Start: RIPEN Phase 0 — 후보 line 의 valid page 그룹 표본 추출.
 * line 의 페이지를 훑어 rmap 으로 lpn 을 얻고, 서로 다른 그룹을
 * 최대 RIPEN_SAMPLES_PER_GC 개까지 ring 에 넣는다. */
static void ripen_sample_line(struct conv_ftl *conv_ftl, struct line *ln)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t seen[RIPEN_SAMPLES_PER_GC];
	int nseen = 0;
	struct ppa ppa;
	int flashpg, ch, lun, i, k;

	if (!ln)
		return;

	ppa.ppa = 0;
	ppa.g.blk = ln->id;
	ppa.g.pl = 0;

	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				ppa.g.ch = ch;
				ppa.g.lun = lun;
				for (i = 0; i < spp->pgs_per_flashpg; i++) {
					struct nand_page *pg;
					uint64_t lpn;
					uint32_t grp;
					bool dup = false;

					ppa.g.pg = flashpg * spp->pgs_per_flashpg + i;
					pg = get_pg(conv_ftl->ssd, &ppa);
					if (pg->status != PG_VALID)
						continue;
					lpn = get_rmap_ent(conv_ftl, &ppa);
					if (lpn == INVALID_LPN)
						continue;
					grp = (uint32_t)(lpn / RIPEN_GROUP_LPNS);
					for (k = 0; k < nseen; k++) {
						if (seen[k] == grp) {
							dup = true;
							break;
						}
					}
					if (dup)
						continue;
					ripen_sample_group(&conv_ftl->ripen, lpn,
							   conv_ftl->cur_nsecs);
					seen[nseen++] = grp;
					if (nseen >= RIPEN_SAMPLES_PER_GC)
						return;
				}
			}
		}
	}
}
/*MN End*/
/* MN: GC 1회분 진단 집계 — victim 선택 "직전"에 호출.
 * 이 시점의 victim_pq를 기준으로:
 *   - cand age 분포 기록(정책 무관 후보 분포)
 *   - greedy 선택 vs cost-benefit 선택을 둘 다 계산해 diff 집계
 *   - "이번에 실제 뽑힐 victim"의 age 누적/히스토그램
 *     (실제 선택은 활성 정책이 하므로, 활성 정책의 pick을 여기서 미리 계산해 쓴다) */
static void gc_account_before_select(struct conv_ftl *conv_ftl)
{
	struct line *greedy_pick = NULL, *cb_pick = NULL, *active_pick = NULL;

	/* [D] 후보 age 분포 */
	record_candidate_age_hist(conv_ftl);

	/* [B] 두 정책이 각각 뭘 고를지 (pq에서 빼기 전이라 비교 가능) */
	peek_both_policies(conv_ftl, &greedy_pick, &cb_pick);
	if (greedy_pick && cb_pick && greedy_pick != cb_pick)
		conv_ftl->sel_diff_g_vs_cb++;
	/*MN RIPEN Phase 0: 만기 표본 라벨 확정 + 새 표본 추출.
	 * greedy_pick(대표 victim 후보) + pq 회전 후보 1개 → 선택편향 완화 */
	ripen_resolve(&conv_ftl->ripen, conv_ftl->cur_nsecs);
	ripen_sample_line(conv_ftl, greedy_pick);
	{
		pqueue_t *pq = conv_ftl->lm.victim_line_pq;
		if (pq->size > 1) {
			size_t ridx = 1 + (size_t)(conv_ftl->gc_cnt % pq->size);
			struct line *r = (struct line *)pq->d[ridx];
			if (r && r != greedy_pick)
				ripen_sample_line(conv_ftl, r);
		}
	}
	/* [C] 활성 정책이 이번에 뽑을 victim의 age 누적/분포.
	 * CB 계열이면 cb_pick, 그 외(greedy/ip_step Phase A)는 greedy_pick으로 근사. */
	active_pick = (conv_ftl->gc_policy == GC_COST_BENEFIT_LINEAR ||
		       conv_ftl->gc_policy == GC_COST_BENEFIT_STEP)
			      ? cb_pick : greedy_pick;
	if (active_pick) {
		uint64_t age = line_age_ns(conv_ftl, active_pick);
		conv_ftl->cb_age_sum += age;
		conv_ftl->victim_age_hist[age_to_hist_bucket(age)]++;
		conv_ftl->victim_vpc_sum[age_to_hist_bucket(age)] += active_pick->vpc;
	}
}

static int do_gc(struct conv_ftl *conv_ftl, bool force)
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	int flashpg;

	/* MN: 선택 직전에 진단 집계(후보 분포 + 대칭 diff + victim age) */
	gc_account_before_select(conv_ftl);

	victim_line = select_victim_line(conv_ftl, force);
	if (!victim_line) {
		return -1;
	}
	/*MN Start*/
	conv_ftl->gc_cnt++;
	conv_ftl->gc_victim_vpc_sum += victim_line->vpc;
	conv_ftl->wfc.credits_to_refill = victim_line->ipc;
	/*MN End*/

	ppa.g.blk = victim_line->id;
	NVMEV_DEBUG_VERBOSE("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
		    victim_line->ipc, victim_line->vpc, conv_ftl->lm.victim_line_cnt,
		    conv_ftl->lm.full_line_cnt, conv_ftl->lm.free_line_cnt);

	conv_ftl->wfc.credits_to_refill = victim_line->ipc;

	/* copy back valid data */
	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		int ch, lun;

		ppa.g.pg = flashpg * spp->pgs_per_flashpg;
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;

				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;
				lunp = get_lun(conv_ftl->ssd, &ppa);
				clean_one_flashpg(conv_ftl, &ppa);

				if (flashpg == (spp->flashpgs_per_blk - 1)) {
					struct convparams *cpp = &conv_ftl->cp;

					mark_block_free(conv_ftl, &ppa);

					if (cpp->enable_gc_delay) {
						struct nand_cmd gce = {
							.type = GC_IO,
							.cmd = NAND_ERASE,
							.stime = 0,
							.interleave_pci_dma = false,
							.ppa = &ppa,
						};
						ssd_advance_nand(conv_ftl->ssd, &gce);
					}

					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}

	/* update line status */
	mark_line_free(conv_ftl, &ppa);

	return 0;
}

static void foreground_gc(struct conv_ftl *conv_ftl)
{
	if (should_gc_high(conv_ftl)) {
		NVMEV_DEBUG_VERBOSE("should_gc_high passed");
		/* perform GC here until !should_gc(conv_ftl) */
		do_gc(conv_ftl, true);
	}
}

static bool is_same_flash_page(struct conv_ftl *conv_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

static bool conv_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];
	/* spp are shared by all instances*/
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint32_t xfer_size, i;
	uint32_t nr_parts = ns->nr_parts;

	struct ppa prev_ppa;
	struct nand_cmd srd = {
		.type = USER_IO,
		.cmd = NAND_READ,
		.stime = nsecs_start,
		.interleave_pci_dma = true,
	};

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
			    start_lpn, spp->tt_pgs);
		return false;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
		srd.stime += spp->fw_4kb_rd_lat;
	} else {
		srd.stime += spp->fw_rd_lat;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
		conv_ftl = &conv_ftls[start_lpn % nr_parts];
		xfer_size = 0;
		prev_ppa = get_maptbl_ent(conv_ftl, start_lpn / nr_parts);

		/* normal IO read path */
		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
			cur_ppa = get_maptbl_ent(conv_ftl, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ftl, &cur_ppa)) {
				NVMEV_DEBUG_VERBOSE("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
				NVMEV_DEBUG_VERBOSE("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
					    cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk,
					    cur_ppa.g.pl, cur_ppa.g.pg);
				continue;
			}

			// aggregate read io in same flash page
			if (mapped_ppa(&prev_ppa) &&
			    is_same_flash_page(conv_ftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue;
			}

			if (xfer_size > 0) {
				srd.xfer_size = xfer_size;
				srd.ppa = &prev_ppa;
				nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
				nsecs_latest = max(nsecs_completed, nsecs_latest);
			}

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
		}

		// issue remaining io
		if (xfer_size > 0) {
			srd.xfer_size = xfer_size;
			srd.ppa = &prev_ppa;
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
			nsecs_latest = max(nsecs_completed, nsecs_latest);
		}
	}

	ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}

static bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct buffer *wbuf = conv_ftl->ssd->write_buffer;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;

	struct nand_cmd swr = {
		.type = USER_IO,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,
	};

	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n",
				__func__, start_lpn, spp->tt_pgs);
		return false;
	}

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
		return false;

	nsecs_latest =
		ssd_advance_write_buffer(conv_ftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		uint64_t local_lpn;
		uint64_t nsecs_completed = 0;
		struct ppa ppa;

		conv_ftl = &conv_ftls[lpn % nr_parts];
		conv_ftl->cur_nsecs = nsecs_latest;  /*MN 파티션별 invalidation 시각 갱신 */
		local_lpn = lpn / nr_parts;
		ppa = get_maptbl_ent(
			conv_ftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(conv_ftl, &ppa);
			set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
			NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(conv_ftl, &ppa));
			/*MN RIPEN Phase 0: overwrite = 그룹 invalidation 이벤트 */
			ripen_on_host_write(&conv_ftl->ripen, local_lpn, nsecs_latest, true);
		} else {
			/*MN RIPEN Phase 0: 최초 매핑 — 밀도만 갱신 */
			ripen_on_host_write(&conv_ftl->ripen, local_lpn, nsecs_latest, false);
		}

		/* new write */
		ppa = get_new_page(conv_ftl, USER_IO);
		/* update maptbl */
		set_maptbl_ent(conv_ftl, local_lpn, &ppa);
		NVMEV_DEBUG("%s: got new ppa %lld, ", __func__, ppa2pgidx(conv_ftl, &ppa));
		/* update rmap */
		set_rmap_ent(conv_ftl, local_lpn, &ppa);

		mark_page_valid(conv_ftl, &ppa);
		conv_ftl->host_write_pages++;	//MN: host_write debug
		/* need to advance the write pointer here */
		advance_write_pointer(conv_ftl, USER_IO);

		/* Aggregate write io in flash page */
		if (last_pg_in_wordline(conv_ftl, &ppa)) {
			swr.ppa = &ppa;

			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
						    spp->pgs_per_oneshotpg * spp->pgsz);
		}

		consume_write_credit(conv_ftl);
		check_and_refill_write_credit(conv_ftl);
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		/* Wait all flash operations */
		ret->nsecs_target = nsecs_latest;
	} else {
		/* Early completion */
		ret->nsecs_target = nsecs_xfer_completed;
	}
	ret->status = NVME_SC_SUCCESS;

	return true;
}
/*MN Start*/
/* nvme flush 시점에 GC 통계를 dmesg로 덤프.
 * Phase A: IP-STEP의 P(invalidate|age) 관측이 목적.
 *
 * 읽는 법:
 *  - waf_x1000   : WAF*1000. 정책 성능의 최종 지표(낮을수록 좋음).
 *  - P_TABLE     : bucket b별 후보/victim 분포와 proxy P[b].
 *                  P[b] = 1 - victim_vpc/cand_vpc (×1000 고정소수점).
 *                  b가 커질수록(=age 큼) P가 작아지면(monotone decreasing)
 *                  age가 사망을 예측 → IP-STEP point 반영 정당. */
static void conv_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	uint64_t start, latest;
	uint32_t i;

	/* ---- 파티션 전체 합산 ----
	 * bucket 배열 4개(각 320B)를 스택에 두면 frame이 1KB를 넘어
	 * -Wframe-larger-than 경고. flush는 성능 경로가 아니므로 힙에 올린다. */
	struct flush_acc {
		uint64_t cand_sum[AGE_HIST_BUCKETS];
		uint64_t vict_sum[AGE_HIST_BUCKETS];
		uint64_t cvpc_sum[AGE_HIST_BUCKETS];
		uint64_t vvpc_sum[AGE_HIST_BUCKETS];
	} *acc;
	uint64_t g_cnt = 0, g_valid = 0, g_host = 0, g_vpcsum = 0;
	uint64_t waf_x1000, avg_vpc_x1000;
	int policy = conv_ftls[0].gc_policy;
	int b;

	acc = kzalloc(sizeof(*acc), GFP_KERNEL);
	if (!acc) {
		ret->status = NVME_SC_SUCCESS;
		ret->nsecs_target = local_clock();
		return;
	}

	for (i = 0; i < ns->nr_parts; i++) {
		g_cnt    += conv_ftls[i].gc_cnt;
		g_valid  += conv_ftls[i].gc_valid_copied;
		g_host   += conv_ftls[i].host_write_pages;
		g_vpcsum += conv_ftls[i].gc_victim_vpc_sum;
		for (b = 0; b < AGE_HIST_BUCKETS; b++) {
			acc->cand_sum[b] += conv_ftls[i].cand_age_hist[b];
			acc->vict_sum[b] += conv_ftls[i].victim_age_hist[b];
			acc->cvpc_sum[b] += conv_ftls[i].cand_vpc_sum[b];
			acc->vvpc_sum[b] += conv_ftls[i].victim_vpc_sum[b];
		}
	}

	/* 커널엔 float 없음 → *1000 고정소수점 */
	waf_x1000     = g_host ? ((g_host + g_valid) * 1000 / g_host) : 1000;
	avg_vpc_x1000 = g_cnt  ? (g_vpcsum * 1000 / g_cnt) : 0;

	/* ---- dmesg 블록 출력 ---- */
	NVMEV_INFO("========== [FLUSH] GC SUMMARY (policy=%d %s) ==========\n",
		   policy,
		   policy == GC_GREEDY               ? "GREEDY" :
		   policy == GC_COST_BENEFIT_LINEAR  ? "CB_LINEAR" :
		   policy == GC_COST_BENEFIT_STEP    ? "CB_STEP" :
		   policy == GC_IP_STEP              ? "IP_STEP" : "UNKNOWN");
	NVMEV_INFO("[FLUSH]  WAF x1000        : %llu   (host=%llu gc_copied=%llu)\n",
		   waf_x1000, g_host, g_valid);
	NVMEV_INFO("[FLUSH]  gc_cnt           : %llu\n", g_cnt);
	NVMEV_INFO("[FLUSH]  avg_victim_vpc   : %llu.%03llu\n",
		   avg_vpc_x1000 / 1000, avg_vpc_x1000 % 1000);

	/* ---- P_TABLE: bucket별 후보/victim 분포 + proxy P[b] ---- */
	NVMEV_INFO("[FLUSH]  --- P_TABLE: b = floor(log2(age_ns)); "
		   "2^b ns (b20~1ms b30~1s b33~8s); P_inv_x1k=1000*(1-vvpc/cvpc) ---\n");
	NVMEV_INFO("[FLUSH]  %5s %14s %12s %12s %12s\n",
		   "b", "approx_ns", "cand", "victim", "P_inv_x1k");
	for (b = 0; b < AGE_HIST_BUCKETS; b++) {
		uint64_t p_x1k, surv_x1k;
		if (acc->cand_sum[b] == 0 && acc->vict_sum[b] == 0)
			continue;
		/* P[b] = 1 - victim_vpc/cand_vpc. cand_vpc==0이면 미정 → 0.
		 * vvpc>cvpc(이론상 없음) 대비 언더플로우 방어 클램프. */
		surv_x1k = acc->cvpc_sum[b] ?
			   (acc->vvpc_sum[b] * 1000 / acc->cvpc_sum[b]) : 1000;
		if (surv_x1k > 1000) surv_x1k = 1000;
		p_x1k = acc->cvpc_sum[b] ? (1000 - surv_x1k) : 0;
		NVMEV_INFO("[FLUSH]  %5d %14llu %12llu %12llu %12llu\n",
			   b, (1ULL << b), acc->cand_sum[b], acc->vict_sum[b], p_x1k);
	}
	NVMEV_INFO("========== [FLUSH] END (policy=%d) ==========\n", policy);

	/*MN RIPEN Phase 0: 만기 표본 정리만 수행(덤프 출력은 Phase A에서 제거) */
	for (i = 0; i < ns->nr_parts; i++)
		ripen_resolve(&conv_ftls[i].ripen, conv_ftls[i].cur_nsecs);

	/* ---- flush latency 모델 ---- */
	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++)
		latest = max(latest, ssd_next_idle_time(conv_ftls[i].ssd));

	NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);

	kfree(acc);
	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
}
/*MN End*/
bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		if (!conv_write(ns, req, ret))
			return false;
		break;
	case nvme_cmd_read:
		if (!conv_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		conv_flush(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
				nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}