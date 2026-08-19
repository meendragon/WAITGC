#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Example: ./run_fio_once.sh p1-greedy
MODULE_TAG=${1:-p1-greedy}
RUN_ID=$(date +%Y%m%d-%H%M%S)
LOG_DIR=/home/meen/WAITGC/script/fio_logs
SUM_LOG=${LOG_DIR}/summary.log
LOG_PREFIX=${LOG_DIR}/fio_rw_${MODULE_TAG}_${RUN_ID}

mkdir -p "${LOG_DIR}"

MODULE_ACTIVE=0
cleanup() {
	if (( MODULE_ACTIVE )); then
		./end_virt.sh || true
	fi
}
trap cleanup EXIT

field_value() {
	local line=$1
	local key=$2
	local token

	for token in ${line}; do
		case "${token}" in
			"${key}"=*)
				printf '%s\n' "${token#*=}"
				return 0
				;;
		esac
	done
	return 1
}

csv_delta7() {
	local final_csv=$1
	local prepare_csv=$2
	local -a final_values prepare_values delta_values
	local i

	IFS=',' read -r -a final_values <<< "${final_csv}"
	IFS=',' read -r -a prepare_values <<< "${prepare_csv}"
	if (( ${#final_values[@]} != 7 || ${#prepare_values[@]} != 7 )); then
		echo "ERROR: expected a seven-bin flush histogram" >&2
		return 1
	fi
	for ((i = 0; i < 7; i++)); do
		if (( final_values[i] < prepare_values[i] )); then
			echo "ERROR: cumulative histogram moved backwards at bin ${i}" >&2
			return 1
		fi
		delta_values[i]=$((final_values[i] - prepare_values[i]))
	done
	local IFS=','
	printf '%s\n' "${delta_values[*]}"
}

capture_flush() {
	local phase=$1
	local output_file=$2

	CAPTURED_RESULT=$(sudo dmesg | grep '\[FLUSH-RESULT\]' | tail -n 1 || true)
	CAPTURED_EVIDENCE=$(sudo dmesg | grep '\[FLUSH-EVIDENCE\]' | tail -n 1 || true)
	CAPTURED_HIST=$(sudo dmesg | grep '\[FLUSH-LEVEL-HIST\]' | tail -n 1 || true)

	if [[ -z "${CAPTURED_RESULT}" || -z "${CAPTURED_EVIDENCE}" ||
	      -z "${CAPTURED_HIST}" ]]; then
		echo "ERROR: incomplete flush evidence for phase=${phase}" >&2
		sudo dmesg > "${LOG_PREFIX}_${phase}_kernel.log"
		return 1
	fi

	{
		printf '[PHASE] %s\n' "${phase}"
		printf '%s\n' "${CAPTURED_RESULT}"
		printf '%s\n' "${CAPTURED_EVIDENCE}"
		printf '%s\n' "${CAPTURED_HIST}"
	} | tee "${output_file}"
}

sudo dmesg -C
./start_virt.sh "${MODULE_TAG}"
MODULE_ACTIVE=1

fio prepare.fio | tee "${LOG_PREFIX}_prepare.log"

# Establish the cumulative baseline after media preparation.  The final
# counters can then be subtracted to obtain test-only WAF.
sync
sudo nvme flush /dev/nvme1n1 -n 1
sleep 1
capture_flush prepare "${LOG_PREFIX}_prepare_flush.log"
PREP_RESULT=${CAPTURED_RESULT}
PREP_EVIDENCE=${CAPTURED_EVIDENCE}
PREP_HIST=${CAPTURED_HIST}
sudo dmesg > "${LOG_PREFIX}_prepare_kernel.log"

# Clearing dmesg does not reset the in-module cumulative counters.
sudo dmesg -C
sleep 5
fio test2.fio | tee "${LOG_PREFIX}_fio.log"

# Emit the final cumulative GC evidence after all workload I/O has completed.
sync
sudo nvme flush /dev/nvme1n1 -n 1
sleep 1
capture_flush final "${LOG_PREFIX}_flush.log"
FINAL_RESULT=${CAPTURED_RESULT}
FINAL_EVIDENCE=${CAPTURED_EVIDENCE}
FINAL_HIST=${CAPTURED_HIST}

PREP_HOST=$(field_value "${PREP_RESULT}" host_pages)
PREP_GC_COPIED=$(field_value "${PREP_RESULT}" gc_copied)
PREP_GC_CNT=$(field_value "${PREP_RESULT}" gc_cnt)
PREP_DIFF=$(field_value "${PREP_RESULT}" diff_greedy)
FINAL_HOST=$(field_value "${FINAL_RESULT}" host_pages)
FINAL_GC_COPIED=$(field_value "${FINAL_RESULT}" gc_copied)
FINAL_GC_CNT=$(field_value "${FINAL_RESULT}" gc_cnt)
FINAL_DIFF=$(field_value "${FINAL_RESULT}" diff_greedy)
FINAL_POLICY=$(field_value "${FINAL_RESULT}" policy)
FINAL_PROFILE=$(field_value "${FINAL_RESULT}" age_profile)
FINAL_STEPS=$(field_value "${FINAL_RESULT}" steps)

PREP_CANDIDATES=$(field_value "${PREP_EVIDENCE}" candidates_sum)
PREP_EVALS=$(field_value "${PREP_EVIDENCE}" candidate_evals)
PREP_CANDIDATE_AGE=$(field_value "${PREP_EVIDENCE}" candidate_age_sum_ns)
PREP_SELECTED_AGE=$(field_value "${PREP_EVIDENCE}" selected_age_sum_ns)
PREP_GREEDY_AGE=$(field_value "${PREP_EVIDENCE}" greedy_ref_age_sum_ns)
PREP_VICTIM_VPC=$(field_value "${PREP_EVIDENCE}" victim_vpc_sum)
PREP_GREEDY_VPC=$(field_value "${PREP_EVIDENCE}" greedy_ref_vpc_sum)
PREP_EXTRA_VPC=$(field_value "${PREP_EVIDENCE}" extra_vpc_sum)
PREP_FORCED=$(field_value "${PREP_EVIDENCE}" forced_gc)
PREP_ZERO_VPC=$(field_value "${PREP_EVIDENCE}" zero_vpc_victims)
FINAL_CANDIDATES=$(field_value "${FINAL_EVIDENCE}" candidates_sum)
FINAL_EVALS=$(field_value "${FINAL_EVIDENCE}" candidate_evals)
FINAL_CANDIDATE_AGE=$(field_value "${FINAL_EVIDENCE}" candidate_age_sum_ns)
FINAL_SELECTED_AGE=$(field_value "${FINAL_EVIDENCE}" selected_age_sum_ns)
FINAL_GREEDY_AGE=$(field_value "${FINAL_EVIDENCE}" greedy_ref_age_sum_ns)
FINAL_VICTIM_VPC=$(field_value "${FINAL_EVIDENCE}" victim_vpc_sum)
FINAL_GREEDY_VPC=$(field_value "${FINAL_EVIDENCE}" greedy_ref_vpc_sum)
FINAL_EXTRA_VPC=$(field_value "${FINAL_EVIDENCE}" extra_vpc_sum)
FINAL_FORCED=$(field_value "${FINAL_EVIDENCE}" forced_gc)
FINAL_ZERO_VPC=$(field_value "${FINAL_EVIDENCE}" zero_vpc_victims)

if (( FINAL_HOST < PREP_HOST || FINAL_GC_COPIED < PREP_GC_COPIED ||
      FINAL_GC_CNT < PREP_GC_CNT || FINAL_DIFF < PREP_DIFF ||
      FINAL_CANDIDATES < PREP_CANDIDATES || FINAL_EVALS < PREP_EVALS ||
      FINAL_CANDIDATE_AGE < PREP_CANDIDATE_AGE ||
      FINAL_SELECTED_AGE < PREP_SELECTED_AGE ||
      FINAL_GREEDY_AGE < PREP_GREEDY_AGE ||
      FINAL_VICTIM_VPC < PREP_VICTIM_VPC ||
      FINAL_GREEDY_VPC < PREP_GREEDY_VPC ||
      FINAL_EXTRA_VPC < PREP_EXTRA_VPC || FINAL_FORCED < PREP_FORCED ||
      FINAL_ZERO_VPC < PREP_ZERO_VPC )); then
	echo "ERROR: a cumulative flush counter moved backwards" >&2
	exit 1
fi

DELTA_HOST=$((FINAL_HOST - PREP_HOST))
DELTA_GC_COPIED=$((FINAL_GC_COPIED - PREP_GC_COPIED))
DELTA_GC_CNT=$((FINAL_GC_CNT - PREP_GC_CNT))
DELTA_DIFF=$((FINAL_DIFF - PREP_DIFF))
DELTA_CANDIDATES=$((FINAL_CANDIDATES - PREP_CANDIDATES))
DELTA_EVALS=$((FINAL_EVALS - PREP_EVALS))
DELTA_CANDIDATE_AGE=$((FINAL_CANDIDATE_AGE - PREP_CANDIDATE_AGE))
DELTA_SELECTED_AGE=$((FINAL_SELECTED_AGE - PREP_SELECTED_AGE))
DELTA_GREEDY_AGE=$((FINAL_GREEDY_AGE - PREP_GREEDY_AGE))
DELTA_VICTIM_VPC=$((FINAL_VICTIM_VPC - PREP_VICTIM_VPC))
DELTA_GREEDY_VPC=$((FINAL_GREEDY_VPC - PREP_GREEDY_VPC))
DELTA_EXTRA_VPC=$((FINAL_EXTRA_VPC - PREP_EXTRA_VPC))
DELTA_FORCED=$((FINAL_FORCED - PREP_FORCED))
DELTA_ZERO_VPC=$((FINAL_ZERO_VPC - PREP_ZERO_VPC))
if (( DELTA_HOST > 0 )); then
	DELTA_WAF_X1000=$(((DELTA_HOST + DELTA_GC_COPIED) * 1000 / DELTA_HOST))
else
	DELTA_WAF_X1000=1000
fi
if (( DELTA_GC_CNT > 0 )); then
	DELTA_AVG_CANDIDATES_X1000=$((DELTA_CANDIDATES * 1000 / DELTA_GC_CNT))
	DELTA_SELECTED_AGE_AVG=$((DELTA_SELECTED_AGE / DELTA_GC_CNT))
	DELTA_GREEDY_AGE_AVG=$((DELTA_GREEDY_AGE / DELTA_GC_CNT))
	DELTA_VICTIM_VPC_X1000=$((DELTA_VICTIM_VPC * 1000 / DELTA_GC_CNT))
	DELTA_GREEDY_VPC_X1000=$((DELTA_GREEDY_VPC * 1000 / DELTA_GC_CNT))
	DELTA_EXTRA_VPC_X1000=$((DELTA_EXTRA_VPC * 1000 / DELTA_GC_CNT))
	DELTA_DIFF_PCT_X1000=$((DELTA_DIFF * 100000 / DELTA_GC_CNT))
else
	DELTA_AVG_CANDIDATES_X1000=0
	DELTA_SELECTED_AGE_AVG=0
	DELTA_GREEDY_AGE_AVG=0
	DELTA_VICTIM_VPC_X1000=0
	DELTA_GREEDY_VPC_X1000=0
	DELTA_EXTRA_VPC_X1000=0
	DELTA_DIFF_PCT_X1000=0
fi
if (( DELTA_EVALS > 0 )); then
	DELTA_CANDIDATE_AGE_AVG=$((DELTA_CANDIDATE_AGE / DELTA_EVALS))
else
	DELTA_CANDIDATE_AGE_AVG=0
fi

PREP_CANDIDATE_HIST=$(field_value "${PREP_HIST}" candidate)
PREP_SELECTED_HIST=$(field_value "${PREP_HIST}" selected)
FINAL_CANDIDATE_HIST=$(field_value "${FINAL_HIST}" candidate)
FINAL_SELECTED_HIST=$(field_value "${FINAL_HIST}" selected)
DELTA_CANDIDATE_HIST=$(csv_delta7 "${FINAL_CANDIDATE_HIST}" "${PREP_CANDIDATE_HIST}")
DELTA_SELECTED_HIST=$(csv_delta7 "${FINAL_SELECTED_HIST}" "${PREP_SELECTED_HIST}")

DELTA_RESULT="[DELTA-RESULT] module=${MODULE_TAG} policy=${FINAL_POLICY} age_profile=${FINAL_PROFILE} steps=${FINAL_STEPS} host_pages=${DELTA_HOST} gc_copied=${DELTA_GC_COPIED} gc_cnt=${DELTA_GC_CNT} diff_greedy=${DELTA_DIFF} diff_pct_x1000=${DELTA_DIFF_PCT_X1000} waf_x1000=${DELTA_WAF_X1000}"
DELTA_EVIDENCE="[DELTA-EVIDENCE] candidates_sum=${DELTA_CANDIDATES} avg_candidates_x1000=${DELTA_AVG_CANDIDATES_X1000} candidate_evals=${DELTA_EVALS} candidate_age_avg_ns=${DELTA_CANDIDATE_AGE_AVG} selected_age_avg_ns=${DELTA_SELECTED_AGE_AVG} greedy_ref_age_avg_ns=${DELTA_GREEDY_AGE_AVG} selected_vpc_x1000=${DELTA_VICTIM_VPC_X1000} greedy_ref_vpc_x1000=${DELTA_GREEDY_VPC_X1000} extra_vpc_sum=${DELTA_EXTRA_VPC} extra_vpc_avg_x1000=${DELTA_EXTRA_VPC_X1000} zero_vpc_victims=${DELTA_ZERO_VPC} forced_gc=${DELTA_FORCED}"
DELTA_HIST="[DELTA-LEVEL-HIST] steps=${FINAL_STEPS} candidate=${DELTA_CANDIDATE_HIST} selected=${DELTA_SELECTED_HIST}"
{
	printf '%s\n' "${DELTA_RESULT}"
	printf '%s\n' "${DELTA_EVIDENCE}"
	printf '%s\n' "${DELTA_HIST}"
} | tee "${LOG_PREFIX}_delta.log"

{
	printf '===== module=%s run=%s =====\n' "${MODULE_TAG}" "${RUN_ID}"
	printf '%s\n' '[PREPARE-CUMULATIVE]'
	printf '%s\n%s\n%s\n' "${PREP_RESULT}" "${PREP_EVIDENCE}" "${PREP_HIST}"
	printf '%s\n' '[FINAL-CUMULATIVE]'
	printf '%s\n%s\n%s\n' "${FINAL_RESULT}" "${FINAL_EVIDENCE}" "${FINAL_HIST}"
	printf '%s\n%s\n%s\n\n' "${DELTA_RESULT}" "${DELTA_EVIDENCE}" "${DELTA_HIST}"
} >> "${SUM_LOG}"

sudo dmesg > "${LOG_PREFIX}_kernel.log"
sudo dmesg | tail -n 50

./end_virt.sh
MODULE_ACTIVE=0
trap - EXIT

echo "[DONE] module=${MODULE_TAG}"
echo "[LOG]  ${LOG_PREFIX}_fio.log"
echo "[LOG]  ${LOG_PREFIX}_prepare_flush.log"
echo "[LOG]  ${LOG_PREFIX}_flush.log"
echo "[LOG]  ${LOG_PREFIX}_delta.log"
echo "[LOG]  ${LOG_PREFIX}_kernel.log"
echo "[LOG]  ${SUM_LOG}"
