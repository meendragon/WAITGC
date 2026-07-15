# 안정적 greedy 승리 후보: 순차성 섞인 uniform (age가 선형창에 퍼지되 예측력 없음)
sudo fio --name=greedy_win2 --filename=/home/meen/mnt/testfile \
    --rw=randwrite --bs=4k --direct=1 --iodepth=4 --numjobs=1 \
    --random_distribution=random --norandommap --randrepeat=0 \
    --rate_iops=15000 --size=6G --io_size=10G