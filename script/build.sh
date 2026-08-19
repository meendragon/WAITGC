#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WAITGC_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
NVMEV_ROOT="${NVMEV_ROOT:-${WAITGC_ROOT}/nvmevirt_test}"
BUILD_OUTPUT_DIR="${BUILD_OUTPUT_DIR:-${WAITGC_ROOT}/buildoutput}"
KERNEL_BUILD_DIR="${KERNEL_BUILD_DIR:-/lib/modules/$(uname -r)/build}"
DEFAULT_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
JOBS="${JOBS:-${DEFAULT_JOBS}}"
BASE_EXTRA_CFLAGS="${EXTRA_CFLAGS:-}"

usage() {
	cat <<'EOF'
Usage:
  ./build.sh
  ./build.sh greedy [-- MAKE_ARGS...]
  ./build.sh collect [HORIZON_NS] [-- MAKE_ARGS...]
  ./build.sh cb [raw|flat|cat7|coarse3|early_sat|late_ramp|aggressive7] [-- MAKE_ARGS...]
  ./build.sh isotonic FIT_HEADER [-- MAKE_ARGS...]
  ./build.sh matrix [FIT_HEADER] [-- MAKE_ARGS...]

Environment:
  NVMEV_ROOT       NVMeVirt source directory (default: ../nvmevirt_test)
  KERNEL_BUILD_DIR Kernel build directory (default: /lib/modules/$(uname -r)/build)
  BUILD_OUTPUT_DIR Output root (default: ../buildoutput)
  JOBS             Parallel make jobs (default: online CPU count)
  NVMEV_KO_PATH    Built module path if it is not $NVMEV_ROOT/nvmev.ko

Examples:
  ./build.sh
  ./build.sh greedy
  ./build.sh collect 10000000000
  ./build.sh cb cat7
  ./build.sh isotonic ../buildoutput/conv_isotonic_fit.h
  ./build.sh matrix ../buildoutput/conv_isotonic_fit.h
  KERNEL_BUILD_DIR=/path/to/kernel/build ./build.sh greedy
  ./build.sh greedy -- LLVM=1
EOF
}

die() {
	echo "build.sh: $*" >&2
	exit 2
}

absolute_path() {
	local path="$1"
	[[ -e "${path}" ]] || die "file not found: ${path}"
	realpath "${path}"
}

profile_id() {
	case "$1" in
		raw|raw_linear|0) echo 0 ;;
		flat|1) echo 1 ;;
		cat7|2) echo 2 ;;
		coarse3|3) echo 3 ;;
		early_sat|4) echo 4 ;;
		late_ramp|5) echo 5 ;;
		aggressive7|6) echo 6 ;;
		*) die "unknown CB profile: $1" ;;
	esac
}

profile_name() {
	case "$1" in
		0) echo raw ;;
		1) echo flat ;;
		2) echo cat7 ;;
		3) echo coarse3 ;;
		4) echo early_sat ;;
		5) echo late_ramp ;;
		6) echo aggressive7 ;;
		*) die "unknown CB profile id: $1" ;;
	esac
}

find_module() {
	local configured="${NVMEV_KO_PATH:-${NVMEV_ROOT}/nvmev.ko}"
	local found

	if [[ -f "${configured}" ]]; then
		echo "${configured}"
		return
	fi
	found="$(find "${NVMEV_ROOT}" -maxdepth 4 -type f -name nvmev.ko -print -quit)"
	[[ -n "${found}" ]] || die "build completed but nvmev.ko was not found"
	echo "${found}"
}

flag_value() {
	local flags="$1"
	local name="$2"
	local pattern="(^|[[:space:]])-D${name}=([^[:space:]]+)"

	if [[ "${flags}" =~ ${pattern} ]]; then
		echo "${BASH_REMATCH[2]}"
	fi
}

verify_module() {
	local module_path="$1"
	local policy_flags="$2"
	local expected

	command -v modinfo >/dev/null 2>&1 || \
		die "modinfo is required to verify the compiled module"

	VERIFIED_POLICY="$(modinfo -F waitgc_policy "${module_path}")"
	VERIFIED_PROFILE="$(modinfo -F waitgc_cb_profile "${module_path}")"
	VERIFIED_HOT_COLD="$(modinfo -F waitgc_hot_cold_relocation "${module_path}")"
	VERIFIED_ISO_LOG="$(modinfo -F waitgc_iso_log_samples "${module_path}")"
	VERIFIED_ISO_HORIZON="$(modinfo -F waitgc_iso_horizon_ns "${module_path}")"
	[[ -n "${VERIFIED_POLICY}" ]] || \
		die "${module_path} has no WAITGC compile metadata; update conv_ftl.c"
	[[ -n "${VERIFIED_HOT_COLD}" ]] || \
		die "${module_path} has no relocation compile metadata; update conv_ftl.c"

	expected="$(flag_value "${policy_flags}" CONV_GC_POLICY)"
	[[ -z "${expected}" || "${VERIFIED_POLICY}" == "${expected}" ]] || \
		die "policy verification failed: expected=${expected} actual=${VERIFIED_POLICY}"

	expected="$(flag_value "${policy_flags}" CONV_CB_AGE_PROFILE)"
	[[ -z "${expected}" || "${VERIFIED_PROFILE}" == "${expected}" ]] || \
		die "CB profile verification failed: expected=${expected} actual=${VERIFIED_PROFILE}"

	expected="$(flag_value "${policy_flags}" CONV_GC_HOT_COLD_RELOCATION)"
	[[ -z "${expected}" || "${VERIFIED_HOT_COLD}" == "${expected}" ]] || \
		die "relocation verification failed: expected=${expected} actual=${VERIFIED_HOT_COLD}"

	expected="$(flag_value "${policy_flags}" CONV_ISO_LOG_SAMPLES)"
	[[ -z "${expected}" || "${VERIFIED_ISO_LOG}" == "${expected}" ]] || \
		die "sample-log verification failed: expected=${expected} actual=${VERIFIED_ISO_LOG}"

	expected="$(flag_value "${policy_flags}" CONV_ISO_SAMPLE_HORIZON_NS)"
	[[ -z "${expected}" || "${VERIFIED_ISO_HORIZON}" == "${expected}" ]] || \
		die "sample-horizon verification failed: expected=${expected} actual=${VERIFIED_ISO_HORIZON}"

	read -r MODULE_SHA256 _ < <(sha256sum "${module_path}")
	printf '[VERIFY] policy=%s profile=%s hot_cold=%s iso_log=%s horizon=%s sha256=%s\n' \
		"${VERIFIED_POLICY}" "${VERIFIED_PROFILE}" "${VERIFIED_HOT_COLD}" \
		"${VERIFIED_ISO_LOG}" "${VERIFIED_ISO_HORIZON}" "${MODULE_SHA256}"
}

build_one() {
	local label="$1"
	local policy_flags="$2"
	local fit_header="${3:-}"
	local module_output="${BUILD_OUTPUT_DIR}/nvmev-${label}.ko"
	local info_output="${BUILD_OUTPUT_DIR}/nvmev-${label}.build-info.txt"
	local module_path
	local all_flags="${BASE_EXTRA_CFLAGS} ${policy_flags}"

	echo "[BUILD] ${label}"
	echo "[FLAGS] ${all_flags}"
	make -C "${KERNEL_BUILD_DIR}" M="${NVMEV_ROOT}" \
		clean "${MAKE_ARGS[@]}"
	make -C "${KERNEL_BUILD_DIR}" M="${NVMEV_ROOT}" \
		-j"${JOBS}" modules "${MAKE_ARGS[@]}" \
		EXTRA_CFLAGS="${all_flags}"

	module_path="$(find_module)"
	mkdir -p "${BUILD_OUTPUT_DIR}"
	install -m 0644 "${module_path}" "${module_output}"
	verify_module "${module_output}" "${all_flags}"

	if [[ -n "${fit_header}" ]]; then
		install -m 0644 "${fit_header}" \
			"${BUILD_OUTPUT_DIR}/nvmev-${label}.fit.h"
	fi

	{
		echo "label=${label}"
		echo "built_at=$(date '+%Y-%m-%dT%H:%M:%S%z')"
		echo "source_root=${NVMEV_ROOT}"
		echo "kernel_build_dir=${KERNEL_BUILD_DIR}"
		echo "module_source=${module_path}"
		echo "extra_cflags=${all_flags}"
		echo "verified_waitgc_policy=${VERIFIED_POLICY}"
		echo "verified_waitgc_cb_profile=${VERIFIED_PROFILE}"
		echo "verified_waitgc_hot_cold_relocation=${VERIFIED_HOT_COLD}"
		echo "verified_waitgc_iso_log_samples=${VERIFIED_ISO_LOG}"
		echo "verified_waitgc_iso_horizon_ns=${VERIFIED_ISO_HORIZON}"
		echo "module_sha256=${MODULE_SHA256}"
		if [[ -n "${fit_header}" ]]; then
			echo "fit_header=${fit_header}"
		fi
	} > "${info_output}"

	echo "[OUTPUT] ${module_output}"
}

if [[ $# -eq 0 ]]; then
	set -- matrix
fi

MODE="$1"
shift
MODE_ARG=""

case "${MODE}" in
	greedy)
		;;
	collect)
		if [[ $# -gt 0 && "$1" != "--" ]]; then
			MODE_ARG="$1"
			shift
		else
			MODE_ARG="10000000000"
		fi
		[[ "${MODE_ARG}" =~ ^[1-9][0-9]*$ ]] || die "HORIZON_NS must be a positive integer"
		;;
	cb)
		if [[ $# -gt 0 && "$1" != "--" ]]; then
			MODE_ARG="$1"
			shift
		else
			MODE_ARG="cat7"
		fi
		;;
	isotonic)
		[[ $# -gt 0 && "$1" != "--" ]] || die "isotonic mode requires FIT_HEADER"
		MODE_ARG="$(absolute_path "$1")"
		shift
		;;
	matrix)
		if [[ $# -gt 0 && "$1" != "--" ]]; then
			MODE_ARG="$(absolute_path "$1")"
			shift
		fi
		;;
	-h|--help|help)
		usage
		exit 0
		;;
	*)
		usage
		die "unknown mode: ${MODE}"
		;;
esac

if [[ $# -gt 0 ]]; then
	[[ "$1" == "--" ]] || die "put additional make arguments after --"
	shift
fi
MAKE_ARGS=("$@")

[[ -f "${NVMEV_ROOT}/Makefile" ]] || \
	die "Makefile not found in ${NVMEV_ROOT}; keep build.sh under WAITGC/script or set NVMEV_ROOT"
[[ -f "${KERNEL_BUILD_DIR}/Makefile" ]] || \
	die "kernel build Makefile not found in ${KERNEL_BUILD_DIR}; install matching kernel headers or set KERNEL_BUILD_DIR"
mkdir -p "${BUILD_OUTPUT_DIR}"

case "${MODE}" in
	greedy)
		build_one "p1-greedy" "-DCONV_GC_POLICY=1"
		;;
	collect)
		build_one "p1-greedy-collect-h${MODE_ARG}ns" \
			"-DCONV_GC_POLICY=1 -DCONV_ISO_LOG_SAMPLES=1 -DCONV_ISO_SAMPLE_HORIZON_NS=${MODE_ARG}ULL"
		;;
	cb)
		PROFILE_ID="$(profile_id "${MODE_ARG}")"
		PROFILE_NAME="$(profile_name "${PROFILE_ID}")"
		build_one "p2-cb-profile${PROFILE_ID}-${PROFILE_NAME}" \
			"-DCONV_GC_POLICY=2 -DCONV_CB_AGE_PROFILE=${PROFILE_ID}"
		;;
	isotonic)
		build_one "p3-isotonic" \
			"-DCONV_GC_POLICY=3 -include ${MODE_ARG}" "${MODE_ARG}"
		;;
	matrix)
		build_one "p1-greedy" "-DCONV_GC_POLICY=1"
		for PROFILE_ID in 0 1 2 3 4 5 6; do
			PROFILE_NAME="$(profile_name "${PROFILE_ID}")"
			build_one "p2-cb-profile${PROFILE_ID}-${PROFILE_NAME}" \
				"-DCONV_GC_POLICY=2 -DCONV_CB_AGE_PROFILE=${PROFILE_ID}"
		done
		if [[ -n "${MODE_ARG}" ]]; then
			build_one "p3-isotonic" \
				"-DCONV_GC_POLICY=3 -include ${MODE_ARG}" "${MODE_ARG}"
		else
			echo "[SKIP] isotonic (no FIT_HEADER supplied)"
		fi
		;;
esac

echo "[DONE] build artifacts: ${BUILD_OUTPUT_DIR}"
