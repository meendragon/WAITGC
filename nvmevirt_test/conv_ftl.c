// SPDX-License-Identifier: GPL-2.0-only
 
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/sched/clock.h>
#include <linux/string.h>
#include "nvmev.h"
#include "conv_ftl.h"
#include <linux/highmem.h>
#include <linux/io.h>

#define CONV_STRINGIFY_INNER(value) #value
#define CONV_STRINGIFY(value) CONV_STRINGIFY_INNER(value)

/* Read back by build.sh to prove which compile-time configuration is in .ko. */
MODULE_INFO(waitgc_policy, CONV_STRINGIFY(CONV_GC_POLICY));
MODULE_INFO(waitgc_cb_profile, CONV_STRINGIFY(CONV_CB_AGE_PROFILE));
MODULE_INFO(waitgc_hot_cold_relocation, "0");
MODULE_INFO(waitgc_iso_log_samples, CONV_STRINGIFY(CONV_ISO_LOG_SAMPLES));
MODULE_INFO(waitgc_iso_horizon_ns,
	    CONV_STRINGIFY(CONV_ISO_SAMPLE_HORIZON_NS));

#undef CONV_STRINGIFY
#undef CONV_STRINGIFY_INNER

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
			.last_modified_time = 0,
			.iso_sample_active = false,
			.iso_sample_ipc = 0,
			.iso_sample_age_ns = 0,
			.iso_sample_start_ns = 0,
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
	if (io_type == USER_IO)
		return &ftl->wp;
	if (io_type == GC_IO)
		return &ftl->gc_wp;

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

static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd,
			  uint32_t part_id)
{
	/*copy convparams*/
	conv_ftl->cp = *cpp;
 
	conv_ftl->ssd = ssd;
	conv_ftl->part_id = part_id;
	conv_ftl->cur_nsecs = 0;
	conv_ftl->dsm_invalidated_pages = 0;

	/* WAF 측정 카운터 */
	conv_ftl->gc_cnt            = 0;
	conv_ftl->gc_valid_copied   = 0;
	conv_ftl->host_write_pages  = 0;
	conv_ftl->gc_victim_vpc_sum = 0;
	conv_ftl->gc_diff_from_greedy = 0;
	conv_ftl->gc_selected_age_ns_sum = 0;
	conv_ftl->gc_selected_age_value_sum = 0;
	conv_ftl->gc_selected_score_sum = 0;
	conv_ftl->gc_candidates_available_sum = 0;
	conv_ftl->gc_candidate_eval_count = 0;
	conv_ftl->gc_candidate_age_ns_sum = 0;
	conv_ftl->gc_candidate_age_ns_min = ~0ULL;
	conv_ftl->gc_candidate_age_ns_max = 0;
	memset(conv_ftl->gc_candidate_level_hist, 0,
	       sizeof(conv_ftl->gc_candidate_level_hist));
	memset(conv_ftl->gc_selected_level_hist, 0,
	       sizeof(conv_ftl->gc_selected_level_hist));
	conv_ftl->gc_selected_age_ns_min = ~0ULL;
	conv_ftl->gc_selected_age_ns_max = 0;
	conv_ftl->gc_greedy_ref_age_ns_sum = 0;
	conv_ftl->gc_greedy_ref_vpc_sum = 0;
	conv_ftl->gc_extra_vpc_vs_greedy_sum = 0;
	conv_ftl->gc_forced_cnt = 0;
	conv_ftl->gc_zero_vpc_victim_cnt = 0;
	conv_ftl->gc_victim_vpc_min = ~0ULL;
	conv_ftl->gc_victim_vpc_max = 0;
	conv_ftl->iso_samples_completed = 0;
	conv_ftl->iso_samples_censored = 0;
 
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
	remove_lines(conv_ftl);
	remove_rmap(conv_ftl);
	remove_maptbl(conv_ftl);
}
 
static void conv_init_params(struct convparams *cpp)
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	cpp->gc_thres_lines = 2; /* host and GC open lines */
	cpp->gc_thres_lines_high = 2;
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
		conv_init_ftl(&conv_ftls[i], &cpp, ssd, i);
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
	line->last_modified_time = conv_ftl->cur_nsecs;
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
 
	mark_page_valid(conv_ftl, &new_ppa);
	conv_ftl->gc_valid_copied++;

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
 
	return 0;
}
 
/* Age is elapsed time since the line's latest invalidation. */
static inline uint64_t line_age_ns(struct conv_ftl *conv_ftl, struct line *ln)
{
	uint64_t now = conv_ftl->cur_nsecs;
	return (now > ln->last_modified_time) ? (now - ln->last_modified_time) : 0;
}

static void evidence_record_selected_age(struct conv_ftl *conv_ftl,
					 uint64_t age_ns, bool has_level,
					 uint32_t level)
{
	conv_ftl->gc_selected_age_ns_sum += age_ns;
	if (age_ns < conv_ftl->gc_selected_age_ns_min)
		conv_ftl->gc_selected_age_ns_min = age_ns;
	if (age_ns > conv_ftl->gc_selected_age_ns_max)
		conv_ftl->gc_selected_age_ns_max = age_ns;
	if (has_level && level < CONV_AGE_LEVEL_MAX)
		conv_ftl->gc_selected_level_hist[level]++;
}

#if CONV_ISO_LOG_SAMPLES
static void isotonic_sample_candidates(struct conv_ftl *conv_ftl)
{
	pqueue_t *pq = conv_ftl->lm.victim_line_pq;
	uint64_t now = conv_ftl->cur_nsecs;
	size_t i;

	for (i = 1; i <= pq->size; i++) {
		struct line *line = (struct line *)pq->d[i];
		uint64_t elapsed;
		uint64_t future_invalid;

		if (!line)
			continue;
		if (!line->iso_sample_active) {
			line->iso_sample_active = true;
			line->iso_sample_ipc = line->ipc;
			line->iso_sample_age_ns = line_age_ns(conv_ftl, line);
			line->iso_sample_start_ns = now;
			continue;
		}

		elapsed = now > line->iso_sample_start_ns ?
			now - line->iso_sample_start_ns : 0;
		if (elapsed < CONV_ISO_SAMPLE_HORIZON_NS)
			continue;

		future_invalid = line->ipc >= line->iso_sample_ipc ?
			(uint64_t)(line->ipc - line->iso_sample_ipc) : 0;
		conv_ftl->iso_samples_completed++;
		NVMEV_INFO("[ISO-SAMPLE] part=%u line=%d age_ns=%llu "
			   "future_invalid_pages=%llu horizon_ns=%llu "
			   "start_ipc=%d end_ipc=%d\n",
			   conv_ftl->part_id, line->id, line->iso_sample_age_ns,
			   future_invalid, elapsed, line->iso_sample_ipc, line->ipc);

		/* Rearm on the next GC decision to avoid overlapping observations. */
		line->iso_sample_active = false;
	}
}

static void isotonic_censor_selected(struct conv_ftl *conv_ftl, struct line *line)
{
	if (!line->iso_sample_active)
		return;
	conv_ftl->iso_samples_censored++;
	line->iso_sample_active = false;
}
#else
static inline void isotonic_sample_candidates(struct conv_ftl *conv_ftl)
{
	(void)conv_ftl;
}

static inline void isotonic_censor_selected(struct conv_ftl *conv_ftl, struct line *line)
{
	(void)conv_ftl;
	(void)line;
}
#endif

static const char *gc_policy_name(void)
{
#if CONV_GC_POLICY == GC_POLICY_GREEDY
	return "GREEDY";
#elif CONV_GC_POLICY == GC_POLICY_AGE_CB
	return "CB_AGE";
#else
	return "ISOTONIC";
#endif
}

static const char *gc_relocation_name(void)
{
	return "SINGLE";
}

static const char *gc_age_profile_name(void)
{
#if CONV_GC_POLICY == GC_POLICY_GREEDY
	return "NA";
#elif CONV_GC_POLICY == GC_POLICY_ISOTONIC
	return "ISOTONIC_FIT";
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_RAW_LINEAR
	return "RAW_LINEAR";
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_FLAT
	return "FLAT";
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_CAT7
	return "CAT7";
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_COARSE3
	return "COARSE3";
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_EARLY_SAT
	return "EARLY_SAT";
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_LATE_RAMP
	return "LATE_RAMP";
#else
	return "AGGRESSIVE7";
#endif
}

static int gc_age_profile_id(void)
{
#if CONV_GC_POLICY == GC_POLICY_GREEDY
	return -1;
#elif CONV_GC_POLICY == GC_POLICY_ISOTONIC
	return -2;
#else
	return CONV_CB_AGE_PROFILE;
#endif
}

static int gc_age_step_count(void)
{
#if CONV_GC_POLICY == GC_POLICY_GREEDY
	return 0;
#elif CONV_GC_POLICY == GC_POLICY_ISOTONIC
	return CONV_ISO_STEP_COUNT;
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_RAW_LINEAR
	return 0;
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_FLAT
	return 1;
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_COARSE3
	return 3;
#else
	return 7;
#endif
}

#if CONV_GC_POLICY != GC_POLICY_GREEDY
#if CONV_GC_POLICY == GC_POLICY_AGE_CB
struct conv_age_step {
	uint64_t upper_age_ns;
	uint64_t value;
};

static uint64_t age_step_lookup(uint64_t age_ns, const struct conv_age_step *steps,
				uint32_t nr_steps, uint32_t *level)
{
	uint32_t i;

	NVMEV_ASSERT(nr_steps > 0);
	for (i = 0; i < nr_steps; i++) {
		if (age_ns < steps[i].upper_age_ns) {
			*level = i;
			return steps[i].value;
		}
	}

	/* The final entry must use ~0ULL as its upper bound. */
	*level = nr_steps - 1;
	return steps[nr_steps - 1].value;
}
/* Compile exactly one profile into each mode-2 experimental binary. */
static uint64_t cb_age_value(uint64_t age_ns, uint32_t *level)
{
#if CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_RAW_LINEAR
	/* Original unnormalized linear Age. */
	*level = 0;
	return age_ns ? age_ns : 1;
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_FLAT
	/* Negative control: remove Age from the ranking. */
	*level = 0;
	return 1;
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_CAT7
	static const struct conv_age_step steps[] = {
		{  10000000000ULL, 1 },
		{  20000000000ULL, 2 },
		{  45000000000ULL, 3 },
		{  90000000000ULL, 4 },
		{ 180000000000ULL, 5 },
		{ 360000000000ULL, 6 },
		{            ~0ULL, 7 },
	};
	return age_step_lookup(age_ns, steps, ARRAY_SIZE(steps), level);
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_COARSE3
	static const struct conv_age_step steps[] = {
		{  20000000000ULL, 1 },
		{ 180000000000ULL, 2 },
		{            ~0ULL, 3 },
	};
	return age_step_lookup(age_ns, steps, ARRAY_SIZE(steps), level);
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_EARLY_SAT
	/* Deliberately poor candidate: most realistic ages quickly become level 7. */
	static const struct conv_age_step steps[] = {
		{  1000000ULL, 1 },
		{  2000000ULL, 2 },
		{  4000000ULL, 3 },
		{  8000000ULL, 4 },
		{ 16000000ULL, 5 },
		{ 32000000ULL, 6 },
		{       ~0ULL, 7 },
	};
	return age_step_lookup(age_ns, steps, ARRAY_SIZE(steps), level);
#elif CONV_CB_AGE_PROFILE == CB_AGE_PROFILE_LATE_RAMP
	/* Deliberately conservative candidate: Age stays flat for a long time. */
	static const struct conv_age_step steps[] = {
		{   60000000000ULL, 1 },
		{  300000000000ULL, 2 },
		{  900000000000ULL, 3 },
		{ 1800000000000ULL, 4 },
		{ 3600000000000ULL, 5 },
		{ 7200000000000ULL, 6 },
		{             ~0ULL, 7 },
	};
	return age_step_lookup(age_ns, steps, ARRAY_SIZE(steps), level);
#else
	/* Same CAT7 boundaries, but a deliberately age-dominant value range. */
	static const struct conv_age_step steps[] = {
		{  10000000000ULL,  1 },
		{  20000000000ULL,  2 },
		{  45000000000ULL,  4 },
		{  90000000000ULL,  8 },
		{ 180000000000ULL, 16 },
		{ 360000000000ULL, 32 },
		{            ~0ULL, 64 },
	};
	return age_step_lookup(age_ns, steps, ARRAY_SIZE(steps), level);
#endif
}
#else
/*
 * Mode 3 consumes the step function exported by reduced isotonic regression.
 * Fitting stays offline; the kernel uses only K, boundaries, and relative
 * values, so victim selection remains integer-only and deterministic.
 */
static uint64_t isotonic_age_value(uint64_t age_ns, uint32_t *level)
{
	static const uint64_t boundaries[] = {
		CONV_ISO_AGE_T1_NS, CONV_ISO_AGE_T2_NS, CONV_ISO_AGE_T3_NS,
		CONV_ISO_AGE_T4_NS, CONV_ISO_AGE_T5_NS, CONV_ISO_AGE_T6_NS,
	};
	static const uint64_t values[] = {
		CONV_ISO_AGE_V1, CONV_ISO_AGE_V2, CONV_ISO_AGE_V3,
		CONV_ISO_AGE_V4, CONV_ISO_AGE_V5, CONV_ISO_AGE_V6,
		CONV_ISO_AGE_V7,
	};
	uint32_t i;

	for (i = 0; i + 1 < CONV_ISO_STEP_COUNT; i++) {
		if (age_ns < boundaries[i]) {
			*level = i;
			return values[i];
		}
	}
	*level = CONV_ISO_STEP_COUNT - 1;
	return values[CONV_ISO_STEP_COUNT - 1];
}
#endif

static uint64_t selected_age_value(uint64_t age_ns, uint32_t *level)
{
#if CONV_GC_POLICY == GC_POLICY_AGE_CB
	return cb_age_value(age_ns, level);
#else
	return isotonic_age_value(age_ns, level);
#endif
}

static void evidence_record_candidate(struct conv_ftl *conv_ftl,
				      uint64_t age_ns, uint32_t level)
{
	conv_ftl->gc_candidate_eval_count++;
	conv_ftl->gc_candidate_age_ns_sum += age_ns;
	if (age_ns < conv_ftl->gc_candidate_age_ns_min)
		conv_ftl->gc_candidate_age_ns_min = age_ns;
	if (age_ns > conv_ftl->gc_candidate_age_ns_max)
		conv_ftl->gc_candidate_age_ns_max = age_ns;
	if (level < CONV_AGE_LEVEL_MAX)
		conv_ftl->gc_candidate_level_hist[level]++;
}

/* Exact AgeValue * IPC / (VPC + 1), with saturating overflow handling. */
static uint64_t cost_benefit_score(struct line *ln, uint64_t age_value)
{
	const uint64_t max_u64 = ~0ULL;
	uint64_t ipc = (uint64_t)ln->ipc;
	uint64_t divisor = (uint64_t)ln->vpc + 1;
	uint64_t quotient, remainder, score, tail;

	if (!age_value || !ipc)
		return 0;

	quotient = age_value / divisor;
	remainder = age_value % divisor;
	if (quotient > max_u64 / ipc)
		return max_u64;
	score = quotient * ipc;

	if (remainder > max_u64 / ipc)
		tail = max_u64 / divisor;
	else
		tail = (remainder * ipc) / divisor;
	if (score > max_u64 - tail)
		return max_u64;

	return score + tail;
}
#endif

#if CONV_GC_POLICY == GC_POLICY_GREEDY
/* Greedy remains the pqueue top: the line with minimum VPC. */
static struct line *select_victim_greedy(struct conv_ftl *conv_ftl, bool force)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *victim_line = NULL;
	uint64_t selected_age_ns;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line)
		return NULL;

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8)))
		return NULL;
	isotonic_sample_candidates(conv_ftl);
	selected_age_ns = line_age_ns(conv_ftl, victim_line);
	conv_ftl->gc_candidates_available_sum += lm->victim_line_pq->size;
	conv_ftl->gc_greedy_ref_vpc_sum += victim_line->vpc;
	conv_ftl->gc_greedy_ref_age_ns_sum += selected_age_ns;
	evidence_record_selected_age(conv_ftl, selected_age_ns, false, 0);

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;

	return victim_line;
}
#endif

#if CONV_GC_POLICY != GC_POLICY_GREEDY
/* CB/isotonic: scan every candidate and maximize AgeValue*IPC/(VPC+1). */
static struct line *select_victim_cost_benefit(struct conv_ftl *conv_ftl, bool force)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	pqueue_t *pq = lm->victim_line_pq;
	struct line *best = NULL;
	struct line *greedy;
	uint64_t best_score = 0;
	uint64_t best_age_ns = 0;
	uint64_t best_age_value = 0;
	uint64_t greedy_age_ns;
	uint32_t best_age_level = 0;
	size_t i;

	if (pq->size == 0)
		return NULL;

	greedy = pqueue_peek(pq);
	if (!greedy)
		return NULL;
	if (!force && (greedy->vpc > (spp->pgs_per_line / 8)))
		return NULL;
	isotonic_sample_candidates(conv_ftl);
	greedy_age_ns = line_age_ns(conv_ftl, greedy);
	conv_ftl->gc_candidates_available_sum += pq->size;

	for (i = 1; i <= pq->size; i++) {
		struct line *ln = (struct line *)pq->d[i];
		uint64_t age_ns;
		uint64_t age_value;
		uint64_t score;
		uint32_t age_level;

		if (!ln)
			continue;
		age_ns = line_age_ns(conv_ftl, ln);
		age_value = selected_age_value(age_ns, &age_level);
		evidence_record_candidate(conv_ftl, age_ns, age_level);
		score = cost_benefit_score(ln, age_value);
#if CONV_GC_LOG_CANDIDATES
		NVMEV_INFO("[GC-CAND] policy=%s profile=%s part=%u gc_seq=%llu "
			   "line=%d age_ns=%llu age_level=%u age_value=%llu "
			   "ipc=%d vpc=%d score=%llu\n",
			   gc_policy_name(), gc_age_profile_name(),
			   conv_ftl->part_id, conv_ftl->gc_cnt + 1, ln->id,
			   age_ns, age_level, age_value, ln->ipc, ln->vpc, score);
#endif
		if (!best || score > best_score ||
		    (score == best_score && ln->vpc < best->vpc) ||
		    (score == best_score && ln->vpc == best->vpc && ln->id < best->id)) {
			best_score = score;
			best_age_ns = age_ns;
			best_age_value = age_value;
			best_age_level = age_level;
			best = ln;
		}
	}
 
	if (!best)
		return NULL;

#if CONV_GC_LOG_SELECTIONS
	NVMEV_INFO("[GC-SELECT] policy=%s profile=%s part=%u gc_seq=%llu line=%d "
		   "age_ns=%llu age_level=%u age_value=%llu ipc=%d vpc=%d score=%llu "
		   "greedy_line=%d greedy_vpc=%d greedy_age_ns=%llu\n",
		   gc_policy_name(), gc_age_profile_name(),
		   conv_ftl->part_id, conv_ftl->gc_cnt + 1, best->id,
		   best_age_ns, best_age_level, best_age_value, best->ipc, best->vpc,
		   best_score, greedy->id, greedy->vpc, greedy_age_ns);
#endif

	if (greedy != best)
		conv_ftl->gc_diff_from_greedy++;
	conv_ftl->gc_greedy_ref_vpc_sum += greedy->vpc;
	conv_ftl->gc_greedy_ref_age_ns_sum += greedy_age_ns;
	if (best->vpc > greedy->vpc)
		conv_ftl->gc_extra_vpc_vs_greedy_sum += best->vpc - greedy->vpc;
	evidence_record_selected_age(conv_ftl, best_age_ns, true, best_age_level);
	conv_ftl->gc_selected_age_value_sum += best_age_value;
	conv_ftl->gc_selected_score_sum += best_score;

	pqueue_remove(pq, best);
	best->pos = 0;
	lm->victim_line_cnt--;

	return best;
}

#endif

static struct line *select_victim_line(struct conv_ftl *conv_ftl, bool force)
{
#if CONV_GC_POLICY == GC_POLICY_GREEDY
	return select_victim_greedy(conv_ftl, force);
#else
	return select_victim_cost_benefit(conv_ftl, force);
#endif
}
 
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
	line->last_modified_time = 0;
	line->iso_sample_active = false;
	line->iso_sample_ipc = 0;
	line->iso_sample_age_ns = 0;
	line->iso_sample_start_ns = 0;
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
}
 
static int do_gc(struct conv_ftl *conv_ftl, bool force)
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa = { .ppa = 0 };
	int flashpg;
 
	victim_line = select_victim_line(conv_ftl, force);
	if (!victim_line)
		return -1;
	isotonic_censor_selected(conv_ftl, victim_line);
 
	/* WAF measurement counters. */
	conv_ftl->gc_cnt++;
	conv_ftl->gc_victim_vpc_sum += victim_line->vpc;
	if (force)
		conv_ftl->gc_forced_cnt++;
	if (victim_line->vpc == 0)
		conv_ftl->gc_zero_vpc_victim_cnt++;
	if ((uint64_t)victim_line->vpc < conv_ftl->gc_victim_vpc_min)
		conv_ftl->gc_victim_vpc_min = victim_line->vpc;
	if ((uint64_t)victim_line->vpc > conv_ftl->gc_victim_vpc_max)
		conv_ftl->gc_victim_vpc_max = victim_line->vpc;
 
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
		conv_ftl->cur_nsecs = nsecs_latest;
		local_lpn = lpn / nr_parts;
		ppa = get_maptbl_ent(
			conv_ftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(conv_ftl, &ppa);
			set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
			NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(conv_ftl, &ppa));
		}

		/* new write */
		ppa = get_new_page(conv_ftl, USER_IO);
		/* update maptbl */
		set_maptbl_ent(conv_ftl, local_lpn, &ppa);
		NVMEV_DEBUG("%s: got new ppa %lld, ", __func__, ppa2pgidx(conv_ftl, &ppa));
		/* update rmap */
		set_rmap_ent(conv_ftl, local_lpn, &ppa);
 
		mark_page_valid(conv_ftl, &ppa);
		conv_ftl->host_write_pages++;
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
static struct nvme_dsm_range __dsm_ranges[256];
 
static void conv_discard(struct nvmev_ns *ns, struct nvmev_request *req,
			 struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct ssdparams *spp = &conv_ftls[0].ssd->sp;
	struct nvme_command *cmd = req->cmd;
	uint32_t nr_parts = ns->nr_parts;
 
	void *vaddr = NULL;
	bool is_memremap = false;
	u64 paddr;
	uint32_t nr_ranges, i;
	uint64_t total = 0;
 
	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = req->nsecs_start;
 
	/* AD(Deallocate) 비트가 없으면 단순 힌트이므로 무시 */
	if (!(le32_to_cpu(cmd->dsm.attributes) & NVME_DSMGMT_AD))
		return;
 
	nr_ranges = (le32_to_cpu(cmd->dsm.nr) & 0xFF) + 1;   /* 0-based, 최대 256 */
 
	/* ---- range 목록 복사 (최대 256*16 = 4096B = 1 page) ---- */
	paddr = cmd->dsm.prp1;
	if (pfn_valid(paddr >> PAGE_SHIFT)) {
		vaddr = kmap_atomic_pfn(PRP_PFN(paddr));
	} else {
		vaddr = memremap(paddr, PAGE_SIZE, MEMREMAP_WT);
		is_memremap = true;
	}
	if (!vaddr) {
		NVMEV_ERROR("%s: cannot map DSM range list\n", __func__);
		ret->status = NVME_SC_INTERNAL;
		return;
	}
 
	memcpy(__dsm_ranges, (char *)vaddr + (paddr & PAGE_OFFSET_MASK),
	       nr_ranges * sizeof(struct nvme_dsm_range));
 
	if (is_memremap)
		memunmap(vaddr);
	else
		kunmap_atomic(vaddr);
	/* ---- 여기부터는 매핑 없이 처리 (preemption 정상) ---- */
 
	for (i = 0; i < nr_ranges; i++) {
		uint64_t slba = le64_to_cpu(__dsm_ranges[i].slba);
		uint32_t nlb  = le32_to_cpu(__dsm_ranges[i].nlb);
		uint64_t start_lpn, end_lpn, lpn;
 
		if (nlb == 0)
			continue;
 
		start_lpn = slba / spp->secs_per_pg;
		end_lpn   = (slba + nlb - 1) / spp->secs_per_pg;
 
		if ((end_lpn / nr_parts) >= spp->tt_pgs) {
			NVMEV_ERROR("%s: range %u OOB (end_lpn=%llu)\n",
				    __func__, i, end_lpn);
			continue;
		}
 
		for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
			struct conv_ftl *ftl = &conv_ftls[lpn % nr_parts];
			uint64_t local_lpn = lpn / nr_parts;
			struct ppa ppa;
 
			ppa = get_maptbl_ent(ftl, local_lpn);
			if (!mapped_ppa(&ppa) || !valid_ppa(ftl, &ppa))
				continue;
 
			/* age 기준 시각 (conv_write와 동일 축) */
			ftl->cur_nsecs = req->nsecs_start;
 
			mark_page_invalid(ftl, &ppa);
			set_rmap_ent(ftl, INVALID_LPN, &ppa);
			ftl->maptbl[local_lpn].ppa = UNMAPPED_PPA;
 
			ftl->dsm_invalidated_pages++;
			total++;
		}
 
		/* 대량 trim이 dispatcher를 오래 붙잡지 않도록 양보 */
		cond_resched();
	}
 
	NVMEV_DEBUG("%s: %u ranges, %llu pages invalidated\n", __func__, nr_ranges, total);
}
 
/* nvme flush 시점에 WAF 요약을 dmesg로 덤프. */
static void conv_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	uint64_t start, latest;
	uint64_t gc_cnt = 0, gc_valid = 0, host = 0, victim_vpc_sum = 0;
	uint64_t dsm_invalidated = 0;
	uint64_t diff_greedy = 0, selected_age_sum = 0;
	uint64_t selected_age_value_sum = 0, selected_score_sum = 0;
	uint64_t candidates_available_sum = 0, candidate_eval_count = 0;
	uint64_t candidate_age_sum = 0, candidate_age_min = ~0ULL;
	uint64_t candidate_age_max = 0, selected_age_min = ~0ULL;
	uint64_t selected_age_max = 0, greedy_ref_age_sum = 0;
	uint64_t greedy_ref_vpc_sum = 0, extra_vpc_sum = 0;
	uint64_t forced_gc = 0, zero_vpc_victims = 0;
	uint64_t victim_vpc_min = ~0ULL, victim_vpc_max = 0;
	uint64_t candidate_level_hist[CONV_AGE_LEVEL_MAX] = { 0 };
	uint64_t selected_level_hist[CONV_AGE_LEVEL_MAX] = { 0 };
	uint64_t iso_samples = 0, iso_censored = 0;
	uint64_t waf_x1000, avg_vpc_x1000, diff_pct_x1000 = 0;
	uint64_t avg_age_ns = 0, avg_age_value_x1000 = 0, avg_score = 0;
	uint64_t avg_candidates_x1000 = 0, avg_candidate_age_ns = 0;
	uint64_t avg_greedy_ref_age_ns = 0, avg_greedy_ref_vpc_x1000 = 0;
	uint64_t avg_extra_vpc_x1000 = 0;
	uint32_t i, j;

	(void)req;

	for (i = 0; i < ns->nr_parts; i++) {
		struct conv_ftl *ftl = &conv_ftls[i];

		gc_cnt += ftl->gc_cnt;
		gc_valid += ftl->gc_valid_copied;
		host += ftl->host_write_pages;
		victim_vpc_sum += ftl->gc_victim_vpc_sum;
		dsm_invalidated += ftl->dsm_invalidated_pages;
		diff_greedy += ftl->gc_diff_from_greedy;
		selected_age_sum += ftl->gc_selected_age_ns_sum;
		selected_age_value_sum += ftl->gc_selected_age_value_sum;
		selected_score_sum += ftl->gc_selected_score_sum;
		candidates_available_sum += ftl->gc_candidates_available_sum;
		candidate_eval_count += ftl->gc_candidate_eval_count;
		candidate_age_sum += ftl->gc_candidate_age_ns_sum;
		greedy_ref_age_sum += ftl->gc_greedy_ref_age_ns_sum;
		greedy_ref_vpc_sum += ftl->gc_greedy_ref_vpc_sum;
		extra_vpc_sum += ftl->gc_extra_vpc_vs_greedy_sum;
		forced_gc += ftl->gc_forced_cnt;
		zero_vpc_victims += ftl->gc_zero_vpc_victim_cnt;
		if (ftl->gc_candidate_eval_count) {
			candidate_age_min = min(candidate_age_min,
						ftl->gc_candidate_age_ns_min);
			candidate_age_max = max(candidate_age_max,
						ftl->gc_candidate_age_ns_max);
		}
		if (ftl->gc_cnt) {
			selected_age_min = min(selected_age_min,
					       ftl->gc_selected_age_ns_min);
			selected_age_max = max(selected_age_max,
					       ftl->gc_selected_age_ns_max);
			victim_vpc_min = min(victim_vpc_min,
					     ftl->gc_victim_vpc_min);
			victim_vpc_max = max(victim_vpc_max,
					     ftl->gc_victim_vpc_max);
		}
		for (j = 0; j < CONV_AGE_LEVEL_MAX; j++) {
			candidate_level_hist[j] += ftl->gc_candidate_level_hist[j];
			selected_level_hist[j] += ftl->gc_selected_level_hist[j];
		}
		iso_samples += ftl->iso_samples_completed;
		iso_censored += ftl->iso_samples_censored;
	}

	/* The kernel has no floating point; printable ratios use x1000. */
	waf_x1000 = host ? ((host + gc_valid) * 1000 / host) : 1000;
	avg_vpc_x1000 = gc_cnt ? (victim_vpc_sum * 1000 / gc_cnt) : 0;
	if (gc_cnt) {
		diff_pct_x1000 = diff_greedy * 100000 / gc_cnt;
		avg_age_ns = selected_age_sum / gc_cnt;
		avg_age_value_x1000 = selected_age_value_sum * 1000 / gc_cnt;
		avg_score = selected_score_sum / gc_cnt;
		avg_candidates_x1000 = candidates_available_sum * 1000 / gc_cnt;
		avg_greedy_ref_age_ns = greedy_ref_age_sum / gc_cnt;
		avg_greedy_ref_vpc_x1000 = greedy_ref_vpc_sum * 1000 / gc_cnt;
		avg_extra_vpc_x1000 = extra_vpc_sum * 1000 / gc_cnt;
	} else {
		selected_age_min = 0;
		victim_vpc_min = 0;
	}
	if (candidate_eval_count)
		avg_candidate_age_ns = candidate_age_sum / candidate_eval_count;
	else
		candidate_age_min = 0;

	NVMEV_INFO("========== [FLUSH] GC SUMMARY mode=%d policy=%s age_profile=%s "
		   "relocation=%s "
		   "profile_id=%d steps=%d ==========\n",
		   CONV_GC_POLICY, gc_policy_name(), gc_age_profile_name(),
		   gc_relocation_name(), gc_age_profile_id(), gc_age_step_count());
	NVMEV_INFO("[FLUSH]  WAF              : %llu.%03llu (x1000=%llu host=%llu gc_copied=%llu)\n",
		   waf_x1000 / 1000, waf_x1000 % 1000, waf_x1000, host, gc_valid);
	NVMEV_INFO("[FLUSH]  gc_cnt           : %llu\n", gc_cnt);
	NVMEV_INFO("[FLUSH]  gc_relocation    : single=%llu total=%llu\n",
		   gc_valid, gc_valid);
	NVMEV_INFO("[FLUSH]  avg_victim_vpc   : %llu.%03llu\n",
		   avg_vpc_x1000 / 1000, avg_vpc_x1000 % 1000);
	NVMEV_INFO("[FLUSH]  victim_vpc_range : min=%llu max=%llu zero=%llu forced_gc=%llu\n",
		   victim_vpc_min, victim_vpc_max, zero_vpc_victims, forced_gc);
	NVMEV_INFO("[FLUSH]  candidates       : total=%llu avg=%llu.%03llu evaluated=%llu\n",
		   candidates_available_sum, avg_candidates_x1000 / 1000,
		   avg_candidates_x1000 % 1000, candidate_eval_count);
	NVMEV_INFO("[FLUSH]  selected_age     : avg=%llu min=%llu max=%llu ns\n",
		   avg_age_ns, selected_age_min, selected_age_max);
	NVMEV_INFO("[FLUSH]  greedy_reference : avg_age=%llu ns avg_vpc=%llu.%03llu "
		   "extra_vpc_avg=%llu.%03llu\n",
		   avg_greedy_ref_age_ns, avg_greedy_ref_vpc_x1000 / 1000,
		   avg_greedy_ref_vpc_x1000 % 1000, avg_extra_vpc_x1000 / 1000,
		   avg_extra_vpc_x1000 % 1000);
	NVMEV_INFO("[FLUSH]  dsm_invalidated  : %llu\n", dsm_invalidated);

#if CONV_GC_POLICY != GC_POLICY_GREEDY
	NVMEV_INFO("[FLUSH]  diff_from_greedy : %llu / %llu (%llu.%03llu%%)\n",
		   diff_greedy, gc_cnt, diff_pct_x1000 / 1000, diff_pct_x1000 % 1000);
	NVMEV_INFO("[FLUSH]  candidate_age    : avg=%llu min=%llu max=%llu ns\n",
		   avg_candidate_age_ns, candidate_age_min, candidate_age_max);
	NVMEV_INFO("[FLUSH]  avg_age_value    : %llu.%03llu\n",
		   avg_age_value_x1000 / 1000, avg_age_value_x1000 % 1000);
	NVMEV_INFO("[FLUSH]  avg_cb_score     : %llu\n", avg_score);
#endif

#if CONV_ISO_LOG_SAMPLES
	NVMEV_INFO("[FLUSH]  isotonic_samples : completed=%llu censored=%llu horizon_ns=%llu\n",
		   iso_samples, iso_censored, (uint64_t)CONV_ISO_SAMPLE_HORIZON_NS);
#endif

	/* One stable, grep-friendly result record for experiment collection. */
	NVMEV_INFO("[FLUSH-RESULT] mode=%d policy=%s age_profile=%s relocation=%s "
		   "profile_id=%d steps=%d "
		   "waf_x1000=%llu host_pages=%llu gc_copied=%llu gc_cnt=%llu "
		   "avg_vpc_x1000=%llu diff_greedy=%llu diff_pct_x1000=%llu "
		   "avg_age_ns=%llu avg_age_value_x1000=%llu avg_score=%llu "
		   "single_copied=%llu hot_copied=%llu cold_copied=%llu "
		   "dsm_invalidated=%llu "
		   "iso_samples=%llu iso_censored=%llu\n",
		   CONV_GC_POLICY, gc_policy_name(), gc_age_profile_name(),
		   gc_relocation_name(), gc_age_profile_id(), gc_age_step_count(),
		   waf_x1000, host, gc_valid, gc_cnt, avg_vpc_x1000,
		   diff_greedy, diff_pct_x1000, avg_age_ns, avg_age_value_x1000,
		   avg_score, gc_valid, 0ULL, 0ULL, dsm_invalidated,
		   iso_samples, iso_censored);

	NVMEV_INFO("[FLUSH-EVIDENCE] gc_cnt=%llu candidates_sum=%llu "
		   "avg_candidates_x1000=%llu candidate_evals=%llu "
		   "candidate_age_sum_ns=%llu selected_age_sum_ns=%llu "
		   "greedy_ref_age_sum_ns=%llu victim_vpc_sum=%llu "
		   "greedy_ref_vpc_sum=%llu "
		   "candidate_age_avg_ns=%llu candidate_age_min_ns=%llu "
		   "candidate_age_max_ns=%llu selected_age_avg_ns=%llu "
		   "selected_age_min_ns=%llu selected_age_max_ns=%llu "
		   "greedy_ref_age_avg_ns=%llu victim_vpc_min=%llu "
		   "victim_vpc_max=%llu zero_vpc_victims=%llu forced_gc=%llu "
		   "greedy_ref_vpc_x1000=%llu selected_vpc_x1000=%llu "
		   "extra_vpc_sum=%llu extra_vpc_avg_x1000=%llu\n",
		   gc_cnt, candidates_available_sum, avg_candidates_x1000,
		   candidate_eval_count, candidate_age_sum, selected_age_sum,
		   greedy_ref_age_sum, victim_vpc_sum, greedy_ref_vpc_sum,
		   avg_candidate_age_ns, candidate_age_min,
		   candidate_age_max, avg_age_ns, selected_age_min, selected_age_max,
		   avg_greedy_ref_age_ns, victim_vpc_min, victim_vpc_max,
		   zero_vpc_victims, forced_gc, avg_greedy_ref_vpc_x1000,
		   avg_vpc_x1000, extra_vpc_sum, avg_extra_vpc_x1000);

	NVMEV_INFO("[FLUSH-LEVEL-HIST] steps=%d "
		   "candidate=%llu,%llu,%llu,%llu,%llu,%llu,%llu "
		   "selected=%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
		   gc_age_step_count(), candidate_level_hist[0], candidate_level_hist[1],
		   candidate_level_hist[2], candidate_level_hist[3],
		   candidate_level_hist[4], candidate_level_hist[5],
		   candidate_level_hist[6], selected_level_hist[0],
		   selected_level_hist[1], selected_level_hist[2],
		   selected_level_hist[3], selected_level_hist[4],
		   selected_level_hist[5], selected_level_hist[6]);

	NVMEV_INFO("========== [FLUSH] END (policy=%d) ==========\n", CONV_GC_POLICY);

	/* Preserve the existing flush latency model. */
	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++)
		latest = max(latest, ssd_next_idle_time(conv_ftls[i].ssd));

	NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
}
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
	case nvme_cmd_dsm:
		conv_discard(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
				nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}
 
	return true;
}
