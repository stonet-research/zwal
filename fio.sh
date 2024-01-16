#!/bin/bash

set -e
mkdir -p data/fio


# sudo PCI_ALLOWED="0000:00:04.0" /home/user/src/spdk/scripts/setup.sh
# for bs in 4096 8192 16384; do
#     sudo LD_PRELOAD=/home/user/src/spdk/build/fio/spdk_nvme ./fio3/fio \
#         --ioengine=spdk \
#         --zone_append=0 \
#         --zonemode=zbd \
#         --direct=1 \
#         --rw=write \
#         --filename='trtype=PCIe traddr=0000.00.04.0 ns=2' \
#         --size=500z \
#         --time_based=1 \
#         --runtime=60s \
#         --ramp_time=10s \
#         --name=spdk_without_append \
#         --bs=${bs} \
#         --iodepth=1 \
#         --numjobs=1 \
#         --initial_zone_reset=1 \
#         --thread=1 \
#         --output-format=json \
#             > data/fio/spdk_without_append_${bs}
# done

# for bs in 4096 8192 16384; do
#     for qd in 1 2 4 8 16; do
#         sudo LD_PRELOAD=/home/user/src/spdk/build/fio/spdk_nvme ./fio3/fio \
#             --ioengine=spdk \
#             --zone_append=1 \
#             --zonemode=zbd \
#             --direct=1 \
#             --rw=write \
#             --filename='trtype=PCIe traddr=0000.00.04.0 ns=2' \
#             --size=500z \
#             --time_based=1 \
#             --runtime=60s \
#             --ramp_time=10s \
#             --name=spdk_with_append \
#             --bs=${bs} \
#             --iodepth=${qd} \
#             --numjobs=1 \
#             --initial_zone_reset=1 \
#             --thread=1 \
#             --output-format=json \
#                 > data/fio/spdk_with_append_${bs}_${qd}
#     done
# done
# sudo PCI_ALLOWED="0000:00:04.0" /home/user/src/spdk/scripts/setup.sh reset

for bs in 8192; do
    sudo nvme zns reset-zone -a /dev/nvme1n1;
    sleep 1;
    sudo LD_PRELOAD=/home/user/src/spdk/build/fio/spdk_nvme ./fio3/fio \
        --ioengine=io_uring_cmd \
        --zone_append=0 \
        --zonemode=zbd \
        --direct=0 \
        --rw=write \
        --filename=/dev/ng1n1 \
        --size=100% \
        --time_based=1 \
        --runtime=60s \
        --ramp_time=10s \
        --name=spdk_without_append \
        --bs=${bs} \
        --iodepth=1 \
        --numjobs=1 \
        --fixedbufs=1 \
        --registerfiles=1 \
        --hipri \
        --sqthread_poll=1 \
        --thread=1 \
        --iodepth_low=0 \
        --output-format=json \
            > data/fio/till_nvme1n1_uring_without_append_${bs}
done

for bs in 8192; do
    for qd in 1 2 4 8 16 32 64; do
        sudo nvme zns reset-zone -a /dev/nvme1n1;
        sleep 1;
        sudo LD_PRELOAD=/home/user/src/spdk/build/fio/spdk_nvme ./fio3/fio \
            --ioengine=io_uring_cmd \
            --zone_append=1 \
            --zonemode=zbd \
            --direct=0 \
            --rw=write \
            --filename=/dev/ng1n1 \
            --size=100% \
            --time_based=1 \
            --runtime=60s \
            --ramp_time=10s \
            --name=spdk_with_append \
            --bs=${bs} \
            --iodepth=${qd} \
            --numjobs=1 \
            --fixedbufs=1 \
            --registerfiles=1 \
            --hipri \
            --sqthread_poll=1 \
            --thread=1 \
            --iodepth_low=0 \
            --output-format=json \
                > data/fio/till_nvme1n1_uring_with_append_${bs}_${qd}
    done
done
