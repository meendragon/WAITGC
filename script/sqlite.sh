#!/bin/bash
export JAVA_TOOL_OPTIONS="-Xshare:off"
# =============================================================
# sqlite.sh - YCSB(SQLite/JDBC) workloada 전용, gc_mode 0/1/2 스윕 (컴팩트판)
#   모드: 0=greedy 1=cb 2=wait
#   사용: ./sqlite.sh                 # 0 1 2 전부
#         MODES_TO_RUN="2" ./sqlite.sh  # wait 하나만
# =============================================================
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

MODES=("greedy" "cb" "wait")
MODES_TO_RUN="${MODES_TO_RUN:-0 1 2}"

DEV="/dev/nvme1n1"
MOUNT_POINT="/home/meen/mnt"
YCSB_DIR="/home/meen/YCSB"
LOGS_DIR="${SCRIPT_DIR}/sqlite_logs"
SQLITE_LIB="/home/meen/YCSB/jdbc-binding/lib"
DB_FILE="${MOUNT_POINT}/ycsb.db"
WL="${SCRIPT_DIR}/workloada"

# ── 사전 체크 ──
[ -f "$WL" ]                         || { echo "❌ workloada 없음: $WL"; exit 1; }
[ -d "$YCSB_DIR" ]                   || { echo "❌ YCSB_DIR 없음: $YCSB_DIR"; exit 1; }
command -v sqlite3 >/dev/null        || { echo "❌ sqlite3 미설치"; exit 1; }
[ -x "${SCRIPT_DIR}/start_virt.sh" ] || { echo "❌ start_virt.sh 없음/실행불가"; exit 1; }
[ -x "${SCRIPT_DIR}/end_virt.sh" ]   || { echo "❌ end_virt.sh 없음/실행불가"; exit 1; }
mkdir -p "${LOGS_DIR}/a_log"

# ─── YCSB 워크로드 실행 ───
run_ycsb_workload() {
    local logfile=$1
    echo "===== YCSB workloada (sqlite/jdbc) @ ${MOUNT_POINT} : $(date) =====" > "$logfile"

    rm -f "${DB_FILE}" "${DB_FILE}-journal" "${DB_FILE}-wal" "${DB_FILE}-shm" 2>/dev/null

    echo ">> sqlite3 schema 생성" >> "$logfile"
    sqlite3 "$DB_FILE" \
      "PRAGMA page_size=4096; PRAGMA synchronous=FULL;
       CREATE TABLE usertable (YCSB_KEY VARCHAR(255) PRIMARY KEY,
         FIELD0 TEXT,FIELD1 TEXT,FIELD2 TEXT,FIELD3 TEXT,FIELD4 TEXT,
         FIELD5 TEXT,FIELD6 TEXT,FIELD7 TEXT,FIELD8 TEXT,FIELD9 TEXT);" >> "$logfile" 2>&1

    ( while true; do sleep 4; sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"; done ) &
    local dc_pid=$!

    pushd "$YCSB_DIR" >/dev/null
    echo ">> ycsb load" >> "$logfile"
    ./bin/ycsb load jdbc -s -P "$WL" -cp "${SQLITE_LIB}/*" \
        -p db.driver=org.sqlite.JDBC \
        -p db.url="jdbc:sqlite:${DB_FILE}" \
        -p db.user= -p db.passwd= \
        -p db.batchsize=1000 -p jdbc.autocommit=false >> "$logfile" 2>&1

    echo ">> ycsb run" >> "$logfile"
    ./bin/ycsb run jdbc -s -P "$WL" -cp "${SQLITE_LIB}/*" \
        -p db.driver=org.sqlite.JDBC \
        -p db.url="jdbc:sqlite:${DB_FILE}" \
        -p db.user= -p db.passwd= \
        -p hdrhistogram.percentiles=95,99,99.9,99.99,99.999,99.9999 >> "$logfile" 2>&1
    popd >/dev/null

    sudo kill "$dc_pid" 2>/dev/null; wait "$dc_pid" 2>/dev/null

    sync
    sudo nvme flush "${DEV}" -n 1 >/dev/null 2>&1
    sleep 2
    {
        echo ""
        echo "===== NVMeVirt GC stats ($(date)) ====="
        sudo dmesg | grep "NVMeVirt:" | tail -200
    } >> "$logfile"
}

# ─── 메인 루프 ───
GLOBAL_START=$(date +%s)
echo "============================================"
echo " sqlite 시작: $(date)   모드: ${MODES_TO_RUN}"
echo "============================================"

for i in ${MODES_TO_RUN}; do
    [ -z "${MODES[$i]}" ] && { echo "❌ 잘못된 mode: $i"; continue; }
    MODE_START=$(date +%s)
    echo ""
    echo "############# gc_mode=${i} (${MODES[$i]}) - $(date) #############"

    "${SCRIPT_DIR}/start_virt.sh" "${i}"
    sleep 3

    LOG_FILE="${LOGS_DIR}/a_log/${i}_${MODES[$i]}.txt"
    run_ycsb_workload "${LOG_FILE}"

    echo "  · end_virt"
    "${SCRIPT_DIR}/end_virt.sh"
    sleep 3

    MODE_MIN=$(( ($(date +%s) - MODE_START) / 60 ))
    echo "✅ [${MODES[$i]}] 완료 - ${MODE_MIN}분 → ${LOG_FILE}"
done

ELAPSED=$(( $(date +%s) - GLOBAL_START ))
echo ""
echo "============================================"
echo " 🎉 종료: $(date)   총 $((ELAPSED/3600))h $(((ELAPSED%3600)/60))m"
echo "============================================"
ls -la "${LOGS_DIR}/a_log/"