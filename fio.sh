#!/bin/bash
dir="$(cd -P -- "$(dirname -- "$0")" && pwd -P)"

if [ $# != 1 ]; then
    echo "Build requires one arg:" \
         "1: nvme_device"
    exit 1
fi

set -e
mkdir -p data/fio

for bs in 4096 8192 16384; do
    sudo nvme zns reset-zone -a /dev/${1};
    sleep 1;
    sudo ${dir}/fio3/fio \
        --ioengine=io_uring_cmd \
        --zone_append=0 \
        --zonemode=zbd \
        --direct=0 \
        --rw=write \
        --filename=/dev/$(echo ${1} | sed 's/nvme/ng/') \
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
            > data/fio/till_${1}_uring_without_append_${bs}
done

for bs in 4096 8192 16384; do
    for qd in 1 2 4 8 16 32 64; do
        sudo nvme zns reset-zone -a /dev/${1};
        sleep 1;
        sudo ${dir}/fio3/fio \
            --ioengine=io_uring_cmd \
            --zone_append=1 \
            --zonemode=zbd \
            --direct=0 \
            --rw=write \
            --filename=/dev/$(echo ${1} | sed 's/nvme/ng/') \
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
                > data/fio/till_${1}_uring_with_append_${bs}_${qd}
    done
done
