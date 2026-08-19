#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Usage: ./run_fio_all.sh [repeat-count]
# Three repetitions are the minimum default for an experimental comparison;
# pass 1 explicitly only for a smoke test.
REPEATS=${1:-3}
[[ "${REPEATS}" =~ ^[1-9][0-9]*$ ]] || {
	echo "Usage: $0 [positive-repeat-count]" >&2
	exit 2
}

MODULES=(
	"p1-greedy"
	"p2-cb-profile0-raw"
	"p2-cb-profile1-flat"
	"p2-cb-profile2-cat7"
	"p2-cb-profile3-coarse3"
	"p2-cb-profile4-early_sat"
	"p2-cb-profile5-late_ramp"
	"p2-cb-profile6-aggressive7"
)

[[ -x ./fio.sh ]] || {
	echo "fio.sh is missing or not executable" >&2
	exit 2
}
[[ -f prepare.fio && -f test2.fio ]] || {
	echo "prepare.fio or test2.fio is missing" >&2
	exit 2
}
for module in "${MODULES[@]}"
do
	[[ -f "../buildoutput/nvmev-${module}.ko" ]] || {
		echo "missing module: ../buildoutput/nvmev-${module}.ko" >&2
		exit 2
	}
done

for ((repeat = 1; repeat <= REPEATS; repeat++))
do
	echo "========== FIO MATRIX repeat=${repeat}/${REPEATS} =========="
	for module in "${MODULES[@]}"
	do
		echo "[MATRIX] repeat=${repeat}/${REPEATS} module=${module}"
		./fio.sh "${module}"
	done
done

echo "[DONE] repeats=${REPEATS} modules=${#MODULES[@]}"
echo "[SUMMARY] /home/meen/WAITGC/script/fio_logs/summary.log"
