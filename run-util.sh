#!/bin/bash

setup_zenfs() {
    rocksd_dir=$1
    sudo rm -rf /tmp/zenfs
    echo mq-deadline | sudo tee /sys/block/$2/queue/scheduler > /dev/null
    sudo nvme zns reset-zone -a /dev/$2
    sudo ${rocksd_dir}/plugin/zenfs/util/zenfs mkfs \
            --zbd=$2 \
            --aux_path=/tmp/zenfs \
            --force
}
