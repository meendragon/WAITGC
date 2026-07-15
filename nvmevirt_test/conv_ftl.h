// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_CONV_FTL_H
#define _NVMEVIRT_CONV_FTL_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"
/*MN Start*/
#include "ripen.h"
/*MN End*/

/*MN Start*/
enum gc_policy {
    GC_GREEDY = 0,
    GC_COST_BENEFIT_LINEAR = 1,
	GC_COST_BENEFIT_STEP = 2,
    GC_IP_STEP = 3,
};
/*MN End*/
struct convparams {
	uint32_t gc_thres_lines;
	uint32_t gc_thres_lines_high;
	bool enable_gc_delay;

	double op_area_pcent;
	int pba_pcent; /* (physical space / logical space) * 100*/
};

struct line {
	int id; /* line id, the same as corresponding block id */
	int ipc; /* invalid page count in this line */
	int vpc; /* valid page count in this line */
	struct list_head entry;
	/* position in the priority queue for victim lines */
	size_t pos;
	/*MN Start*/
	uint64_t last_modified_time;  /* cost-benefit age용 */
    int poi;                   /* WAITGC: 곧 죽을 valid 기대 개수 */
	/*MN End*/
};

/* wp: record next write addr */
struct write_pointer {
	struct line *curline;
	uint32_t ch;
	uint32_t lun;
	uint32_t pg;
	uint32_t blk;
	uint32_t pl;
};

struct line_mgmt {
	struct line *lines;

	/* free line list, we only need to maintain a list of blk numbers */
	struct list_head free_line_list;
	pqueue_t *victim_line_pq;
	struct list_head full_line_list;

	uint32_t tt_lines;
	uint32_t free_line_cnt;
	uint32_t victim_line_cnt;
	uint32_t full_line_cnt;
};

struct write_flow_control {
	uint32_t write_credits;
	uint32_t credits_to_refill;
};

struct conv_ftl {
	struct ssd *ssd;

	struct convparams cp;
	struct ppa *maptbl; /* page level mapping table */
	uint64_t *rmap; /* reverse mapptbl, assume it's stored in OOB */
	struct write_pointer wp;
	struct write_pointer gc_wp;
	struct line_mgmt lm;
	struct write_flow_control wfc;
	int gc_policy;          /* MN: insmod 파라미터에서 주입 */
	uint64_t cur_nsecs;     /* 현재 처리 중인 요청의 nsecs — invalidation 시각 소스 */
	uint64_t victim_vpc_sum[40];
	uint64_t cand_vpc_sum[40];   /* MN: bucket별 후보 vpc 누적 (bias 없는 P(inv|age) 프록시) */
	/* ================================================================
	 * MN: GC 통계 — nvme flush 시점에 dmesg로 덤프.
	 * greedy vs cost-benefit 차이가 "나는지 / 안 나는지 / 왜 안 나는지"를
	 * flush 로그만 보고 판정할 수 있도록 설계한 카운터 묶음.
	 * 모든 갱신은 conv_ftl.c의 gc_account_*() 한 곳에서만 이뤄진다.
	 * ================================================================ */

	/* [A] WAF/부하 — 정책 성능의 최종 지표 */
	uint64_t gc_cnt;            /* GC(victim 1개 처리) 횟수 */
	uint64_t gc_valid_copied;   /* GC가 옮긴 valid page (WAF 분자) */
	uint64_t host_write_pages;  /* host write page (WAF 분모) */
	uint64_t gc_victim_vpc_sum; /* 선택 victim vpc 누적 → 평균 vpc */

	/* [B] 정책 차이 — "두 정책이 실제로 다른 victim을 고르는가"
	 * 어떤 정책으로 돌리든, 매 GC마다 greedy 선택과 cost-benefit 선택을
	 * 둘 다 계산해서(선택 자체는 활성 정책만 반영) 서로 달랐던 횟수를 센다.
	 * diff==0 이면 두 정책이 사실상 같은 line을 고르고 있다는 직접 증거. */
	uint64_t sel_diff_g_vs_cb;  /* greedy_pick != cost_benefit_pick 인 GC 수 */

	/* [C] age 신호 — cost-benefit이 greedy로 "붕괴"하는지 진단.
	 * cb_age_sum : 실제 선택된 victim의 raw age(ns) 누적 → 평균 age.
	 *   평균 age가 0에 가까우면 age 신호가 죽어 있어 CB=greedy로 수렴. */
	uint64_t cb_age_sum;

	/* [D] age 분포 히스토그램 — step 함수 경계 튜닝 + 붕괴 시각화.
	 * 버킷 b = floor(log2(age_ns)) → 대략 2^b ns 스케일.
	 *   b=20 ≈ 1ms, b=30 ≈ 1s, b=33 ≈ 8s ...
	 * cand   : GC 시점 victim_pq 후보 전체의 age 분포(경계는 이 골에 둔다)
	 * victim : GC로 실제 선택된 victim의 age 분포
	 * 두 분포가 저(低)버킷 한 칸에 뭉쳐 있으면 age가 hot/cold를 못 가른다는 뜻. */
#define AGE_HIST_BUCKETS 40
	uint64_t cand_age_hist[AGE_HIST_BUCKETS];
	uint64_t victim_age_hist[AGE_HIST_BUCKETS];
	/* [E] RIPEN Phase 0 — 그룹 feature vs 라벨 계측 (ripen.h 참조) */
	struct ripen ripen;
};

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher);

void conv_remove_namespace(struct nvmev_ns *ns);

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			   struct nvmev_result *ret);

#endif