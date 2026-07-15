// SPDX-License-Identifier: GPL-2.0-only
/*
 * MN: RIPEN-GC Phase 0 — feature 계측 구현. 상세 설명은 ripen.h 참조.
 * float 금지 → 전부 정수/shift. 예측 모델은 여기 없다(Phase 1에서 추가).
 */

#include <linux/vmalloc.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/bitops.h>

#include "nvmev.h"
#include "ripen.h"

/* ---------------- 버킷 헬퍼 ---------------- */

/* floor(log2(ns)). AGEHIST 와 동일 스케일. ~0ULL(이력 없음)은 최상위 버킷. */
static inline int rip_log_bucket(uint64_t v)
{
	int b;
	if (v == 0)
		return 0;
	if (v == ~0ULL)
		return RIPEN_LOG_BUCKETS - 1;
	b = fls64(v) - 1;
	if (b >= RIPEN_LOG_BUCKETS)
		b = RIPEN_LOG_BUCKETS - 1;
	return b;
}

static inline int rip_freq_bucket(uint16_t c)
{
	int b = fls(c); /* 0→0, 1→1, 2~3→2, 4~7→3 ... */
	if (b >= RIPEN_FREQ_BUCKETS)
		b = RIPEN_FREQ_BUCKETS - 1;
	return b;
}

static inline int rip_dens_bucket(uint16_t pct)
{
	int b = pct / 10;
	if (b >= RIPEN_DENS_BUCKETS)
		b = RIPEN_DENS_BUCKETS - 1;
	return b;
}
/* log2 버킷(0~39)을 8구간 coarse로 압축. ~0ULL(이력없음)은 log_bucket에서
 * 이미 최상위(39)로 오므로 자동으로 c7에 들어간다. */
static inline int rip_coarse_bucket(uint64_t v)
{
	int lb = rip_log_bucket(v);
	int c = (lb - RIPEN_2D_BASE) / RIPEN_2D_STEP;
	if (c < 0)
		c = 0;
	if (c >= RIPEN_2D_BINS)
		c = RIPEN_2D_BINS - 1;
	return c;
}
/* ---------------- init / free ---------------- */

int ripen_init(struct ripen *rp, unsigned long tt_pgs)
{
	memset(rp, 0, sizeof(*rp));

	rp->ngroups = (uint32_t)((tt_pgs + RIPEN_GROUP_LPNS - 1) / RIPEN_GROUP_LPNS);

	rp->g = vzalloc(sizeof(struct ripen_group) * rp->ngroups);
	if (!rp->g)
		return -ENOMEM;

	rp->ring = vzalloc(sizeof(struct ripen_sample) * RIPEN_RING_SZ);
	if (!rp->ring) {
		vfree(rp->g);
		rp->g = NULL;
		return -ENOMEM;
	}

	NVMEV_INFO("[RIPEN0] init: ngroups=%u (group=%d LPNs) ring=%d tau_ns=%llu\n",
		   rp->ngroups, RIPEN_GROUP_LPNS, RIPEN_RING_SZ,
		   (unsigned long long)RIPEN_TAU_NS);
	return 0;
}

void ripen_free(struct ripen *rp)
{
	if (rp->g)
		vfree(rp->g);
	if (rp->ring)
		vfree(rp->ring);
	rp->g = NULL;
	rp->ring = NULL;
}

/* ---------------- write 훅 ---------------- */

/* freq 감쇠: 1s 마다 전 그룹 반감. ngroups 수천 수준이라 부담 없음.
 * (CAT 교훈: 빈도 단독은 위험 — 감쇠로 recency 성분을 섞는다) */
static void rip_maybe_decay(struct ripen *rp, uint64_t now_ns)
{
	uint32_t i;

	if (rp->last_decay_ns == 0) {
		rp->last_decay_ns = now_ns;
		return;
	}
	if (now_ns - rp->last_decay_ns < RIPEN_DECAY_NS)
		return;

	for (i = 0; i < rp->ngroups; i++)
		rp->g[i].freq >>= 1;
	rp->last_decay_ns = now_ns;
}

void ripen_on_host_write(struct ripen *rp, uint64_t local_lpn, uint64_t now_ns,
			 bool overwrite)
{
	uint32_t grp = (uint32_t)(local_lpn / RIPEN_GROUP_LPNS);
	struct ripen_group *g;

	if (!rp->g || grp >= rp->ngroups)
		return;
	g = &rp->g[grp];

	rip_maybe_decay(rp, now_ns);

	if (!overwrite) {
		/* 최초 매핑 — invalidation 아님. 밀도만 갱신 */
		if (g->mapped < RIPEN_GROUP_LPNS)
			g->mapped++;
		return;
	}

	/* overwrite = 이 그룹에서 invalidation 발생 */
	if (g->last_inval_ns && now_ns > g->last_inval_ns) {
		uint64_t gap = now_ns - g->last_inval_ns;
		/* EWMA(1/8): ewma += (gap - ewma) / 8 */
		if (g->ewma_gap_ns == 0)
			g->ewma_gap_ns = gap;
		else
			g->ewma_gap_ns += (gap >> 3) - (g->ewma_gap_ns >> 3);
	}
	g->last_inval_ns = now_ns;
	g->wseq++;
	if (g->freq < 0xFFFF)
		g->freq++;
}

/* ---------------- 표본 추출 ---------------- */

void ripen_sample_group(struct ripen *rp, uint64_t local_lpn, uint64_t now_ns)
{
	uint32_t grp = (uint32_t)(local_lpn / RIPEN_GROUP_LPNS);
	uint32_t next;
	struct ripen_group *g;
	struct ripen_sample *s;

	if (!rp->g || !rp->ring || grp >= rp->ngroups)
		return;

	next = (rp->tail + 1) & (RIPEN_RING_SZ - 1);
	if (next == rp->head) {		/* ring 가득 — 표본 버림 */
		rp->dropped++;
		return;
	}

	g = &rp->g[grp];
	s = &rp->ring[rp->tail];

	s->t0       = now_ns;
	s->grp      = grp;
	s->seq0     = g->wseq;
	s->freq     = g->freq;
	s->age_ns   = g->last_inval_ns ? (now_ns - g->last_inval_ns) : ~0ULL;
	s->gap_ns   = g->ewma_gap_ns ? g->ewma_gap_ns : ~0ULL;
	s->dens_pct = (uint16_t)((uint32_t)g->mapped * 100 / RIPEN_GROUP_LPNS);

	rp->tail = next;
}

/* ---------------- 라벨 확정 ---------------- */

void ripen_resolve(struct ripen *rp, uint64_t now_ns)
{
	if (!rp->ring)
		return;

	while (rp->head != rp->tail) {
		struct ripen_sample *s = &rp->ring[rp->head];
		int y;

		if (s->t0 + RIPEN_TAU_NS > now_ns)
			break;	/* 아직 τ 안 지남 — ring 은 시간순이라 여기서 중단 */

		/* τ 동안 그룹에 overwrite 가 있었나 */
		y = (rp->g[s->grp].wseq != s->seq0) ? 1 : 0;

		rp->n_label[y]++;
		rp->h_freq[y][rip_freq_bucket(s->freq)]++;
		rp->h_age [y][rip_log_bucket(s->age_ns)]++;
		rp->h_gap [y][rip_log_bucket(s->gap_ns)]++;
		rp->h_dens[y][rip_dens_bucket(s->dens_pct)]++;
		/* [2D] gap × age 결합 */
		rp->h_ga[y][rip_coarse_bucket(s->gap_ns)]
			  [rip_coarse_bucket(s->age_ns)]++;
		rp->head = (rp->head + 1) & (RIPEN_RING_SZ - 1);
	}
}

/* ---------------- flush 덤프 ---------------- */

static void rip_print_hist2(const char *name, const uint64_t (*h)[2],
			    int nbuckets, int is_log)
{
	int b;

	NVMEV_INFO("[RIPEN0]  --- %s: bucket | y0(계속 삶) | y1(τ내 재기록) ---\n", name);
	for (b = 0; b < nbuckets; b++) {
		uint64_t y0 = h[b][0], y1 = h[b][1];
		if (y0 == 0 && y1 == 0)
			continue;
		if (is_log)
			NVMEV_INFO("[RIPEN0]  %5d %14llu %12llu %12llu\n",
				   b, (b == nbuckets - 1) ? 0ULL : (1ULL << b),
				   y0, y1);
		else
			NVMEV_INFO("[RIPEN0]  %5d %14s %12llu %12llu\n",
				   b, "", y0, y1);
	}
}

void ripen_dump(struct ripen **rps, int n)
{
	/* 파티션 합산. hist 는 [y][bucket] 이라 [bucket][2] 로 뒤집어 모은다 */
	static uint64_t f[RIPEN_FREQ_BUCKETS][2];
	static uint64_t a[RIPEN_LOG_BUCKETS][2];
	static uint64_t gp[RIPEN_LOG_BUCKETS][2];
	static uint64_t d[RIPEN_DENS_BUCKETS][2];
	static uint64_t ga[RIPEN_2D_BINS][RIPEN_2D_BINS][2];  /* [gap_c][age_c][y] */
	uint64_t ny[2] = { 0, 0 }, dropped = 0, pending = 0;
	int i, y, b;

	memset(f, 0, sizeof(f));
	memset(a, 0, sizeof(a));
	memset(gp, 0, sizeof(gp));
	memset(d, 0, sizeof(d));
	memset(ga, 0, sizeof(ga));

	for (i = 0; i < n; i++) {
		struct ripen *rp = rps[i];
		if (!rp || !rp->g)
			continue;
		for (y = 0; y < 2; y++) {
			ny[y] += rp->n_label[y];
			for (b = 0; b < RIPEN_FREQ_BUCKETS; b++)
				f[b][y] += rp->h_freq[y][b];
			for (b = 0; b < RIPEN_LOG_BUCKETS; b++) {
				a[b][y]  += rp->h_age[y][b];
				gp[b][y] += rp->h_gap[y][b];
			}
			for (b = 0; b < RIPEN_DENS_BUCKETS; b++)
				d[b][y] += rp->h_dens[y][b];
			{
				int gc, ac;
				for (gc = 0; gc < RIPEN_2D_BINS; gc++)
					for (ac = 0; ac < RIPEN_2D_BINS; ac++)
						ga[gc][ac][y] += rp->h_ga[y][gc][ac];
			}
		}
		dropped += rp->dropped;
		pending += (rp->tail - rp->head) & (RIPEN_RING_SZ - 1);
	}

	NVMEV_INFO("========== [RIPEN0] FEATURE vs LABEL (tau=%llums, group=%d LPN) ==========\n",
		   (unsigned long long)(RIPEN_TAU_NS / 1000000), RIPEN_GROUP_LPNS);
	NVMEV_INFO("[RIPEN0]  samples: y1=%llu y0=%llu (y1 rate x1000 = %llu)  dropped=%llu pending=%llu\n",
		   ny[1], ny[0],
		   (ny[0] + ny[1]) ? ny[1] * 1000 / (ny[0] + ny[1]) : 0,
		   dropped, pending);
	NVMEV_INFO("[RIPEN0]  읽는법: 각 표에서 y0 열과 y1 열의 질량이 다른 버킷에 몰려 있으면\n");
	NVMEV_INFO("[RIPEN0]          그 feature 는 재기록 예측 신호가 있다. 겹치면 신호 없음.\n");
	NVMEV_INFO("[RIPEN0]          y1 rate 가 0 또는 1000 에 붙어 있으면 τ 재조정 필요.\n");

	rip_print_hist2("FREQ  x1 (fls of decayed count)", f, RIPEN_FREQ_BUCKETS, 0);
	rip_print_hist2("AGE   x2 (log2 ns; last=no-history)", a, RIPEN_LOG_BUCKETS, 1);
	rip_print_hist2("GAP   x3 (EWMA rewrite interval, log2 ns; last=no-history)", gp, RIPEN_LOG_BUCKETS, 1);
	rip_print_hist2("DENS  x4 (mapped %% / 10)", d, RIPEN_DENS_BUCKETS, 0);
/* ---- 2D: gap × age. 각 칸에 y0/y1과 p(x1000) 출력 ----
	 * 읽는법: gap이 같은 행에서 age(열)에 따라 p가 크게 갈리면
	 *         → age가 gap의 애매함을 분리 = 결합 이득 있음.
	 *         한 행 안에서 p가 평평하면 그 gap 구간은 age 무용. */
	NVMEV_INFO("[RIPEN0]  --- 2D gap(row) x age(col): 각 칸 y1rate_x1000 (n=y0+y1) ---\n");
	NVMEV_INFO("[RIPEN0]  coarse c: 0:<256us 1:~2ms 2:~16ms 3:~128ms 4:~1s 5:~8s 6:~64s 7:>=64s\n");
	{
		int gc, ac;
		/* 헤더: age coarse 열 */
		NVMEV_INFO("[RIPEN0]  gap\\age    c0     c1     c2     c3     c4     c5     c6     c7\n");
		for (gc = 0; gc < RIPEN_2D_BINS; gc++) {
			char line[256];
			int pos = 0;
			uint64_t rown = 0;
			for (ac = 0; ac < RIPEN_2D_BINS; ac++)
				rown += ga[gc][ac][0] + ga[gc][ac][1];
			if (rown == 0)
				continue;
			pos += scnprintf(line + pos, sizeof(line) - pos,
					 "[RIPEN0]  c%d  ", gc);
			for (ac = 0; ac < RIPEN_2D_BINS; ac++) {
				uint64_t y0 = ga[gc][ac][0];
				uint64_t y1 = ga[gc][ac][1];
				uint64_t nn = y0 + y1;
				uint64_t p  = nn ? (y1 * 1000 / nn) : 0;
				if (nn == 0)
					pos += scnprintf(line + pos, sizeof(line) - pos,
							 "    -  ");
				else
					pos += scnprintf(line + pos, sizeof(line) - pos,
							 " %4llu  ", p);
			}
			NVMEV_INFO("%s\n", line);
		}
		/* 칸별 표본수(n)도 따로 — p만 보면 노이즈 칸 속으니 */
		NVMEV_INFO("[RIPEN0]  --- 2D n (표본수; 작은 칸의 p는 노이즈) ---\n");
		NVMEV_INFO("[RIPEN0]  gap\\age    c0     c1     c2     c3     c4     c5     c6     c7\n");
		for (gc = 0; gc < RIPEN_2D_BINS; gc++) {
			char line[256];
			int pos = 0;
			uint64_t rown = 0;
			for (ac = 0; ac < RIPEN_2D_BINS; ac++)
				rown += ga[gc][ac][0] + ga[gc][ac][1];
			if (rown == 0)
				continue;
			pos += scnprintf(line + pos, sizeof(line) - pos,
					 "[RIPEN0]  c%d  ", gc);
			for (ac = 0; ac < RIPEN_2D_BINS; ac++) {
				uint64_t nn = ga[gc][ac][0] + ga[gc][ac][1];
				pos += scnprintf(line + pos, sizeof(line) - pos,
						 " %5llu ", nn);
			}
			NVMEV_INFO("%s\n", line);
		}
	}
	NVMEV_INFO("========== [RIPEN0] END ==========\n");
}