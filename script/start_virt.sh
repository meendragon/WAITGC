#!/bin/bash
# $1 = gc_policy (0=greedy, 1=cost-benefit, 2=waitgc). 기본 0
GC_POLICY=${1:-0}

DEV=/dev/nvme1n1 
MNT_DIR=/home/meen/mnt

if lsmod | grep -q "^nvmev"; then
    sudo umount ${MNT_DIR} 2>/dev/null
    sudo rmmod nvmev
    sleep 1
fi

# 2. NVMeVirt 드라이버 로드
sudo insmod ./nvmev.ko \
    memmap_start=4G \
    memmap_size=8192M \
    cpus=1,2 \
    gc_policy=${GC_POLICY}

sleep 1 # 장치가 /dev에 정상 등록될 때까지 잠시 대기

# 3. 새 가상 하드디스크 포맷 (Ext4 파일시스템 생성)
echo "Format..."
echo y | sudo mkfs -t ext4 ${DEV}

# 4. 마운트 폴더 연결
echo "Mount..."
sudo mount ${DEV} ${MNT_DIR}

# 5. 질문자님 계정(meen)으로 권한 넘겨주기 (sudo 없이 fio 돌리기 위함)
sudo chown -R meen:meen ${MNT_DIR}
echo "Virt Ready"