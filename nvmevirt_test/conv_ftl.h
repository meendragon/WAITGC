// SPDX-License-Identifier: GPL-2.0-only
 
#ifndef _NVMEVIRT_CONV_FTL_H
#define _NVMEVIRT_CONV_FTL_H
 
#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"
 
/*
 * Victim selection is fixed at build time.  Build one module per mode:
 *
 *   -DCONV_GC_POLICY=1  Greedy (minimum VPC)
 *   -DCONV_GC_POLICY=2  Cost-benefit with a compile-time Age profile
 *   -DCONV_GC_POLICY=3  Cost-benefit with the fitted isotonic step function
 *
 * Mode 2 is the preliminary experiment mode.  Rebuild it with a different
 * CONV_CB_AGE_PROFILE for every Age-normalization experiment.
 */
#define GC_POLICY_GREEDY 1
#define GC_POLICY_AGE_CB 2
#define GC_POLICY_ISOTONIC 3

#ifndef CONV_GC_POLICY
#define CONV_GC_POLICY GC_POLICY_GREEDY
#endif

#if CONV_GC_POLICY != GC_POLICY_GREEDY && \
	CONV_GC_POLICY != GC_POLICY_AGE_CB && \
	CONV_GC_POLICY != GC_POLICY_ISOTONIC
#error "CONV_GC_POLICY must be 1 (Greedy), 2 (CB-Age), or 3 (Isotonic)"
#endif

/* Mode-2 Age profiles.  RAW is the original linear Age term. */
#define CB_AGE_PROFILE_RAW_LINEAR     0
#define CB_AGE_PROFILE_FLAT           1
#define CB_AGE_PROFILE_CAT7           2
#define CB_AGE_PROFILE_COARSE3        3
#define CB_AGE_PROFILE_EARLY_SAT      4
#define CB_AGE_PROFILE_LATE_RAMP      5
#define CB_AGE_PROFILE_AGGRESSIVE7    6
#define CONV_AGE_LEVEL_MAX            7

#ifndef CONV_CB_AGE_PROFILE
#define CONV_CB_AGE_PROFILE CB_AGE_PROFILE_CAT7
#endif

#if CONV_CB_AGE_PROFILE < CB_AGE_PROFILE_RAW_LINEAR || \
	CONV_CB_AGE_PROFILE > CB_AGE_PROFILE_AGGRESSIVE7
#error "CONV_CB_AGE_PROFILE must be in [0, 6]"
#endif

/*
 * Mode-3 deployment table.  Replace these constants with the step count,
 * boundaries, and relative values exported by fit_isotonic.py.  The default
 * is the CAT7 shape, so isotonic mode is deterministic before fitted values
 * are exported.
 * Boundaries are upper bounds in nanoseconds; values are positive and
 * monotonically non-decreasing.  V1 is anchored at 1 because a common scale
 * factor does not change victim ranking.
 */
#ifndef CONV_ISO_STEP_COUNT
#define CONV_ISO_STEP_COUNT 7
#endif

#if CONV_ISO_STEP_COUNT < 1 || CONV_ISO_STEP_COUNT > 7
#error "CONV_ISO_STEP_COUNT must be in [1, 7]"
#endif

#ifndef CONV_ISO_AGE_T1_NS
#define CONV_ISO_AGE_T1_NS 10000000000ULL
#endif
#ifndef CONV_ISO_AGE_T2_NS
#define CONV_ISO_AGE_T2_NS 20000000000ULL
#endif
#ifndef CONV_ISO_AGE_T3_NS
#define CONV_ISO_AGE_T3_NS 45000000000ULL
#endif
#ifndef CONV_ISO_AGE_T4_NS
#define CONV_ISO_AGE_T4_NS 90000000000ULL
#endif
#ifndef CONV_ISO_AGE_T5_NS
#define CONV_ISO_AGE_T5_NS 180000000000ULL
#endif
#ifndef CONV_ISO_AGE_T6_NS
#define CONV_ISO_AGE_T6_NS 360000000000ULL
#endif

#ifndef CONV_ISO_AGE_V1
#define CONV_ISO_AGE_V1 1ULL
#endif
#ifndef CONV_ISO_AGE_V2
#define CONV_ISO_AGE_V2 2ULL
#endif
#ifndef CONV_ISO_AGE_V3
#define CONV_ISO_AGE_V3 3ULL
#endif
#ifndef CONV_ISO_AGE_V4
#define CONV_ISO_AGE_V4 4ULL
#endif
#ifndef CONV_ISO_AGE_V5
#define CONV_ISO_AGE_V5 5ULL
#endif
#ifndef CONV_ISO_AGE_V6
#define CONV_ISO_AGE_V6 6ULL
#endif
#ifndef CONV_ISO_AGE_V7
#define CONV_ISO_AGE_V7 7ULL
#endif

#if CONV_ISO_AGE_V1 != 1
#error "CONV_ISO_AGE_V1 must stay anchored at 1"
#endif
#if CONV_ISO_STEP_COUNT >= 2 && \
	(CONV_ISO_AGE_T1_NS == 0 || CONV_ISO_AGE_V2 < CONV_ISO_AGE_V1)
#error "Isotonic step 2 must have a positive boundary and non-decreasing value"
#endif
#if CONV_ISO_STEP_COUNT >= 3 && \
	(CONV_ISO_AGE_T2_NS <= CONV_ISO_AGE_T1_NS || CONV_ISO_AGE_V3 < CONV_ISO_AGE_V2)
#error "Isotonic step 3 must have an increasing boundary and non-decreasing value"
#endif
#if CONV_ISO_STEP_COUNT >= 4 && \
	(CONV_ISO_AGE_T3_NS <= CONV_ISO_AGE_T2_NS || CONV_ISO_AGE_V4 < CONV_ISO_AGE_V3)
#error "Isotonic step 4 must have an increasing boundary and non-decreasing value"
#endif
#if CONV_ISO_STEP_COUNT >= 5 && \
	(CONV_ISO_AGE_T4_NS <= CONV_ISO_AGE_T3_NS || CONV_ISO_AGE_V5 < CONV_ISO_AGE_V4)
#error "Isotonic step 5 must have an increasing boundary and non-decreasing value"
#endif
#if CONV_ISO_STEP_COUNT >= 6 && \
	(CONV_ISO_AGE_T5_NS <= CONV_ISO_AGE_T4_NS || CONV_ISO_AGE_V6 < CONV_ISO_AGE_V5)
#error "Isotonic step 6 must have an increasing boundary and non-decreasing value"
#endif
#if CONV_ISO_STEP_COUNT >= 7 && \
	(CONV_ISO_AGE_T6_NS <= CONV_ISO_AGE_T5_NS || CONV_ISO_AGE_V7 < CONV_ISO_AGE_V6)
#error "Isotonic step 7 must have an increasing boundary and non-decreasing value"
#endif

/* Set to 1 at build time to emit one [GC-CAND] record per scanned line. */
#ifndef CONV_GC_LOG_CANDIDATES
#define CONV_GC_LOG_CANDIDATES 0
#endif

/* Set to 1 to emit one [GC-SELECT] record per completed victim selection. */
#ifndef CONV_GC_LOG_SELECTIONS
#define CONV_GC_LOG_SELECTIONS 0
#endif

/*
 * Optional fixed-horizon training samples for fit_isotonic.py.  Candidate
 * lines are sampled only at GC decisions, so normal builds should leave this
 * disabled.  The same horizon must be used for every run in one dataset.
 */
#ifndef CONV_ISO_LOG_SAMPLES
#define CONV_ISO_LOG_SAMPLES 0
#endif

#ifndef CONV_ISO_SAMPLE_HORIZON_NS
#define CONV_ISO_SAMPLE_HORIZON_NS 10000000000ULL
#endif

#if CONV_ISO_SAMPLE_HORIZON_NS == 0
#error "CONV_ISO_SAMPLE_HORIZON_NS must be positive"
#endif
 
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
	uint64_t last_modified_time; /* timestamp of the latest invalidation */

	/* Fixed-horizon observation used only when CONV_ISO_LOG_SAMPLES=1. */
	bool iso_sample_active;
	int iso_sample_ipc;
	uint64_t iso_sample_age_ns;
	uint64_t iso_sample_start_ns;
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
 
	uint32_t part_id;
	uint64_t cur_nsecs; /* timestamp source for Age */
	uint64_t dsm_invalidated_pages;

	/* WAF 측정 카운터 */
	uint64_t gc_cnt;
	uint64_t gc_valid_copied;
	uint64_t host_write_pages;
	uint64_t gc_victim_vpc_sum;

	/* Victim-selection diagnostics for the flush-time preliminary results. */
	uint64_t gc_diff_from_greedy;
	uint64_t gc_selected_age_ns_sum;
	uint64_t gc_selected_age_value_sum;
	uint64_t gc_selected_score_sum;

	/* Aggregate causal evidence; avoids high-volume per-candidate printk. */
	uint64_t gc_candidates_available_sum;
	uint64_t gc_candidate_eval_count;
	uint64_t gc_candidate_age_ns_sum;
	uint64_t gc_candidate_age_ns_min;
	uint64_t gc_candidate_age_ns_max;
	uint64_t gc_candidate_level_hist[CONV_AGE_LEVEL_MAX];
	uint64_t gc_selected_level_hist[CONV_AGE_LEVEL_MAX];
	uint64_t gc_selected_age_ns_min;
	uint64_t gc_selected_age_ns_max;
	uint64_t gc_greedy_ref_age_ns_sum;
	uint64_t gc_greedy_ref_vpc_sum;
	uint64_t gc_extra_vpc_vs_greedy_sum;
	uint64_t gc_forced_cnt;
	uint64_t gc_zero_vpc_victim_cnt;
	uint64_t gc_victim_vpc_min;
	uint64_t gc_victim_vpc_max;

	uint64_t iso_samples_completed;
	uint64_t iso_samples_censored;
};
 
void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher);
 
void conv_remove_namespace(struct nvmev_ns *ns);
 
bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			   struct nvmev_result *ret);
 
#endif
