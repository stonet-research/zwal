#!/bin/bash
set -e
dir="$(cd -P -- "$(dirname -- "$0")" && pwd -P)"
echo "Working dir: ${dir}"

. run_util.sh

if [ $# != 2 ]; then
    echo "run-zwal-recovery requires two args:" \
         "1: nvme_device" \
         "2: runid" \
    exit 1
fi

DEV_SECT_SIZE=$(cat /sys/block/$1/queue/hw_sector_size)
DEV_ZONE_SIZE_BLOCKS=$(cat /sys/block/$1/queue/chunk_sectors)
DEV_ZONE_SIZE=$(echo "${DEV_ZONE_SIZE_BLOCKS} * 512" | bc)

DEV_ZONE_CAP=$(sudo nvme zns report-zones /dev/$1 | head -n 2 | tail -n 1 | awk 'BEGIN { FS = " "}; { print $6 }')
DEV_ZONE_CAP=$(printf "(%d * ${DEV_SECT_SIZE}) / 1024\n" ${DEV_ZONE_CAP} | bc)

mkdir -p data
recov_dir=data/recovery
mkdir -p ${recov_dir}
echo "Running workload in ${dir}"

# Enable barriers
echo "Running with barriers and appends"
sed -i "s/.*#define WAL_BARRIERS.*/#define WAL_BARRIERS/g" ${dir}/rocksdb-raw/plugin/zenfs/fs/io_zenfs.h
for bs in 4; do
    for walsize in 16384 32768 65536 131072 262144; do
        for barrier in 4 32 16384 ${DEV_ZONE_CAP}; do
            for depth in 32; do
                # Setup environment
                echo "barrier: ${barrier}KiB depth:${depth}QD bs:${bs}KiB walsize:${walsize}KiB"
                ./build.sh y ${bs} ${depth} n $1 $barrier

                setup_zenfs "${dir}/rocksdb-raw" $1;
                ## fillrandom
                pushd  "${dir}/rocksdb-raw"

                echo "FILLRANDOM"
                sudo ./db_bench \
                        --fs_uri=zenfs://dev:${1} \
                        --benchmarks=fillrandom \
                        --use_direct_io_for_flush_and_compaction \
                        --value_size=3980 \
                        --key_size=16 \
                        --num=${walsize} \
                        --compression_type=none \
                        --threads=1 \
                        --histogram \
                        --use_existing_db=0 \
                        --wal_ttl_seconds=1 \
                        --write_buffer_size=$((1024*1024*1024*2)) \
                        --target_file_size_base=2147483648 \
                            1> ../${recov_dir}/fillrandom_out_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 \
                            2> ../${recov_dir}/fillrandom_err_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2

                echo "FILLOVERWRITE"
                sudo ./db_bench \
                        --fs_uri=zenfs://dev:${1} \
                        --benchmarks=overwrite \
                        --use_direct_io_for_flush_and_compaction \
                        --value_size=3980 \
                        --key_size=16 \
                        --num=10 \
                        --compression_type=none \
                        --threads=1 \
                        --histogram \
                        --use_existing_db=1 \
                        --wal_ttl_seconds=1 \
                        --write_buffer_size=$((1024*1024*1024*2)) \
                        --target_file_size_base=2147483648 \
                            1> ../${recov_dir}/overwrite_out_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 \
                            2> ../${recov_dir}/overwrite_err_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 
                popd
            done
        done
    done
done

echo "Running ZenFS with writes"
for bs in 4; do
    for walsize in 16384 32768 65536 131072 262144; do
        for barrier in none; do
            for depth in 32; do
                # Setup environment
                echo "barrier: ${barrier}KiB depth:${depth}QD bs:${bs}KiB walsize:${walsize}KiB"
                        
                appendstr="without-append"
                ./build.sh n ${bs} ${depth} n $1 $barrier

                setup_zenfs "${dir}/rocksdb-raw" $1;
                ## fillrandom
                pushd  "${dir}/rocksdb-raw"

                echo "FILLRANDOM"
                sudo ./db_bench \
                        --fs_uri=zenfs://dev:${1} \
                        --benchmarks=fillrandom \
                        --use_direct_io_for_flush_and_compaction \
                        --value_size=3980 \
                        --key_size=16 \
                        --num=${walsize} \
                        --compression_type=none \
                        --threads=1 \
                        --histogram \
                        --use_existing_db=0 \
                        --wal_ttl_seconds=1 \
                        --write_buffer_size=$((1024*1024*1024*2)) \
                        --target_file_size_base=2147483648 \
                            1> ../${recov_dir}/fillrandom_out_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 \
                            2> ../${recov_dir}/fillrandom_err_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2


                echo "FILLOVERWRITE"
                sudo ./db_bench \
                        --fs_uri=zenfs://dev:${1} \
                        --benchmarks=overwrite \
                        --use_direct_io_for_flush_and_compaction \
                        --value_size=3980 \
                        --key_size=16 \
                        --num=10 \
                        --compression_type=none \
                        --threads=1 \
                        --histogram \
                        --use_existing_db=1 \
                        --wal_ttl_seconds=1 \
                        --write_buffer_size=$((1024*1024*1024*2)) \
                        --target_file_size_base=2147483648 \
                            1> ../${recov_dir}/overwrite_out_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 \
                            2> ../${recov_dir}/overwrite_err_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 
                popd
            done
        done
    done
done
