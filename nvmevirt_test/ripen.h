// SPDX-License-Identifier: GPL-2.0-only
/*
 * MN: RIPEN-GC Phase 0 — feature 계측 전용 (모델 없음)
 *
 * 목적: logistic regression 붙이기 전에, 그룹 단위 raw feature(x1~x4)가
 *       "τ 안에 재기록되나(y)"를 실제로 가르는지 분포로 확인한다.
 *       = age vs PoI 상관분석의 실질. 결과는 논문 motivation 그림 재료.
 *
 * 동작:
 *   [write 훅]  conv_write 루프에서 그룹 통계 갱신 (freq/age/gap/mapped)
 *   [GC 훅]     victim 후보 line의 valid page 그룹을 표본 추출
 *               → feature 스냅샷 + t0 + wseq0 를 ring 에 push
 *   [resolve]   t0 + τ 지난 표본: 그룹 wseq 가 늘었으면 y=1, 아니면 y=0
 *               → feature별 hist[y][bucket] 누적
 *   [flush]     hist 를 dmesg 로 덤프. y=0 / y=1 두 분포가 갈라져 있으면
 *               그 feature 는 예측기 입력으로 신호가 있다는 뜻.
 */

#ifndef _NVMEVIRT_RIPEN_H
#define _NVMEVIRT_RIPEN_H

#include <linux/types.h>

/* ---- 튜닝 파라미터 (Phase 0 은 #define, 필요시 module_param 승격) ---- */
#define RIPEN_GROUP_LPNS     256                      /* 그룹 입자: local_lpn / 256 */
#define RIPEN_TAU_NS  (256ULL * 1000 * 1000)   /* 256ms: 봉우리(b24) 오른쪽 밖 */
#define RIPEN_DECAY_NS       (1000ULL * 1000 * 1000)  /* freq 반감 주기 = 1s */
#define RIPEN_RING_SZ        4096                     /* pending 표본 ring (2^n) */
#define RIPEN_SAMPLES_PER_GC 8                        /* line 하나에서 뽑는 그룹 수 상한 */

/* ---- 히스토그램 버킷 ---- */
#define RIPEN_LOG_BUCKETS    40   /* log2(ns) — AGEHIST 와 동일 스케일 */
#define RIPEN_FREQ_BUCKETS   17   /* fls(count): 0, 1, 2~3, 4~7, ... */
#define RIPEN_DENS_BUCKETS   11   /* mapped 밀도 0~100% 를 10% 단위 */
/* ---- 2D 교차표: gap × age 결합 예측력 확인용 ----
 * 1D 표에서 gap 봉우리(b24~28)는 p 극단인데 gap 몸통(b31~33)이 애매(p 0.2~0.35).
 * 그 애매 구간을 age가 갈라주는지 = 결합 오류율이 단독보다 내려가는지 확인.
 * 40×40은 커널 로그로 과하니 log2 버킷을 8구간(coarse)으로 압축한다.
 *   coarse c = clamp((log2_bucket - RIPEN_2D_BASE) / RIPEN_2D_STEP, 0, 7)
 *   base=18(≈256us), step=3 → c0:<256us c1:~2ms c2:~16ms c3:~128ms
 *                              c4:~1s c5:~8s c6:~64s c7:>=64s */
#define RIPEN_2D_BINS  8
#define RIPEN_2D_BASE  18   /* 이 log2버킷 미만은 전부 c0 */
#define RIPEN_2D_STEP  3    /* coarse 한 칸 = log2 3칸 = 8배 */
/* 그룹별 상태 — 예측기 feature 의 원천 */
struct ripen_group {
	uint64_t last_inval_ns; /* 마지막 overwrite(=invalidation) 시각. 0 = 이력 없음 */
	uint64_t ewma_gap_ns;   /* 재기록 간격 EWMA (1/8 가중). 0 = 이력 없음 */
	uint32_t wseq;          /* overwrite 누적 시퀀스 — 라벨 판정용 */
	uint16_t freq;          /* 감쇠(1s 반감) 빈도 카운터, 포화 */
	uint16_t mapped;        /* 그룹 내 mapped LPN 수 → 밀도 */
};

/* pending 표본 — t0 시점 feature 스냅샷 */
struct ripen_sample {
	uint64_t t0;       /* 표본 시각 */
	uint64_t age_ns;   /* x2: t0 - last_inval_ns (~0ULL = 이력 없음) */
	uint64_t gap_ns;   /* x3: ewma_gap_ns 스냅샷 */
	uint32_t grp;
	uint32_t seq0;     /* t0 시점 wseq */
	uint16_t freq;     /* x1 */
	uint16_t dens_pct; /* x4: 0~100 */
};

struct ripen {
	struct ripen_group *g;
	uint32_t ngroups;
	uint64_t last_decay_ns;

	/* pending ring: head = resolve 위치, tail = 삽입 위치 */
	struct ripen_sample *ring;
	uint32_t head, tail;
	uint64_t dropped;   /* ring 가득이라 버린 표본 수 */

	/* [결과] feature별 라벨 분리 히스토그램: h_X[y][bucket]
	 * y=1: τ 안에 그룹 재기록됨(곧 죽을 것) / y=0: 안 됨(계속 살 것) */
	uint64_t h_freq[2][RIPEN_FREQ_BUCKETS];
	uint64_t h_age [2][RIPEN_LOG_BUCKETS];
	uint64_t h_gap [2][RIPEN_LOG_BUCKETS];
	uint64_t h_dens[2][RIPEN_DENS_BUCKETS];
	/* [2D] gap(coarse) × age(coarse) × label. p=y1/(y0+y1)를 칸마다 계산.
	 * gap 단독이 애매한 칸을 age가 분리하면 이 표의 각 칸 p가 극단으로 갈린다. */
	uint64_t h_ga[2][RIPEN_2D_BINS][RIPEN_2D_BINS];  /* [y][gap_c][age_c] */
	uint64_t n_label[2];   /* y 별 표본 수 — τ 적정성(라벨 균형) 판단 */
};

int  ripen_init(struct ripen *rp, unsigned long tt_pgs);
void ripen_free(struct ripen *rp);

/* conv_write 루프에서 per-LPN 호출. overwrite = 기존 매핑이 있었는가 */
void ripen_on_host_write(struct ripen *rp, uint64_t local_lpn, uint64_t now_ns,
			 bool overwrite);

/* GC 시점 표본 추출: lpn 이 속한 그룹의 feature 스냅샷을 ring 에 push */
void ripen_sample_group(struct ripen *rp, uint64_t local_lpn, uint64_t now_ns);

/* t0 + τ 가 지난 표본들 라벨 확정 → 히스토그램 누적. GC/flush 때 호출 */
void ripen_resolve(struct ripen *rp, uint64_t now_ns);

/* flush 덤프: 파티션별 ripen 포인터 배열을 받아 합산 출력 */
void ripen_dump(struct ripen **rps, int n);

#endif /* _NVMEVIRT_RIPEN_H */