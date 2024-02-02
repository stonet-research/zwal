#!/bin/bash
set -e
dir="$(cd -P -- "$(dirname -- "$0")" && pwd -P)"
echo "Working dir: ${dir}"

. run_util.sh

if [ $# != 3 ]; then
    echo "Build requires two args:" \
         "1: nvme_device" \
         "2: real (yes or no)" \
         "3: with appends (yes or no)" 
    exit 1
fi

# Key is up to 23 characters.
# Value is fieldlength*fieldcount. We set it to (386*10)+23 = 3823 (to leave space for metadata)
case $2 in 
    y*)
        recordcount=26214400
        fieldlength=100
        fieldcount=10
        ;;
    *)
        recordcount=5242880
        fieldlength=386
        fieldcount=10
        ;;
esac

mkdir -p data
mkdir -p data/ycsb-perf


DEV_SECT_SIZE=$(cat /sys/block/$1/queue/hw_sector_size)
DEV_ZONE_SIZE_BLOCKS=$(cat /sys/block/$1/queue/chunk_sectors)
DEV_ZONE_SIZE=$(echo "${DEV_ZONE_SIZE_BLOCKS} * 512" | bc)

DEV_ZONE_CAP=$(sudo nvme zns report-zones /dev/$1 | head -n 2 | tail -n 1 | awk 'BEGIN { FS = " "}; { print $6 }')
DEV_ZONE_CAP=$(printf "(%d * ${DEV_SECT_SIZE}) / 1024\n" ${DEV_ZONE_CAP} | bc)


echo "Running workload in ${dir}"
for depth in 32; do
    for bs in 8 4; do
        # Setup environment
        case $3 in
            y*)
                appendstr="with-append"
                barrier=${DEV_ZONE_CAP}
                ./build.sh y ${bs} ${depth} y $1 ${barrier}
                ;;
            *)
                appendstr="without-append"
                barrier=none
                ./build.sh n ${bs} ${depth} y $1 ${barrier}
                ;;
        esac
        for workload in a b c d e; do
            setup_zenfs "${dir}/rocksdb-ycsb" $1;
            run="$(date +"%Y-%m-%d-%H-%M")-${appendstr}"
            echo "run ${run}"
            pushd ycsb
            sudo ./bin/ycsb load rocksdb \
                -s \
                -P ./workloads/workload${workload} \
                -p recordcount=${recordcount} \
                -p operationcount=1000000 \
                -p fieldlength=${fieldlength} \
                -p fieldcount=${fieldcount} \
                    1> ../data/ycsb-perf/real-${workload}-load-${barrier}-${depth}-${bs}-${run}.out \
                    2> ../data/ycsb-perf/real-${workload}-load-${barrier}-${depth}-${bs}-${run}.err;

            sudo ./bin/ycsb run rocksdb \
                -s \
                -P ./workloads/workload${workload} \
                -p recordcount=${recordcount} \
                -p operationcount=1000000 \
                -p fieldlength=${fieldlength} \
                -p fieldcount=${fieldcount} \
                    1> ../data/ycsb-perf/real-${workload}-run-${barrier}-${depth}-${bs}-${run}.out \
                    2> ../data/ycsb-perf/real-${workload}-run-${barrier}-${depth}-${bs}-${run}.err;
            popd
        done
    done
done
