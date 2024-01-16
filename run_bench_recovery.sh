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

mkdir -p data
mkdir -p data/dbbench

# 1310720

DEV_SECT_SIZE=$(cat /sys/block/$1/queue/hw_sector_size)
DEV_ZONE_SIZE_BLOCKS=$(cat /sys/block/$1/queue/chunk_sectors)
DEV_ZONE_SIZE=$(echo "${DEV_ZONE_SIZE_BLOCKS} * 512" | bc)

DEV_ZONE_CAP=$(sudo nvme zns report-zones /dev/$1 | head -n 2 | tail -n 1 | awk 'BEGIN { FS = " "}; { print $6 }')
DEV_ZONE_CAP=$(printf "(%d * ${DEV_SECT_SIZE}) / 1024\n" ${DEV_ZONE_CAP} | bc)

recov_dir=data/recovery-final
mkdir -p ${recov_dir}


echo "Running workload in ${dir}"
# for barrier in 64 256 1024 2048 $((1024*1024)); do
#for barrier in 1; do #  32 ${DEV_ZONE_CAP} 128 256 512; do

sed -i "s/.*#define WAL_BARRIERS.*/#define WAL_BARRIERS/g" ${dir}/rocksdb-raw/plugin/zenfs/fs/io_zenfs.h
for bs in 4; do
    # for barrier in ${bs} $((${bs} * 2)) $((${bs} * 4)) $((${bs} * 5)) 32 64 $((${DEV_ZONE_CAP})); do


for walsize in 262144 16384 32768 65536 131072; do
    # for barrier in $((8192*2)) 8192 4096 128 256 512 1024 2048; do
    # for barrier in $((${DEV_ZONE_CAP})); do
    for barrier in 4; do
        for depth in 32; do
            #for walsize in 16384 32768 65536 131072 262144; do
            # Setup environment
            echo "barrier: ${barrier}KiB depth:${depth}QD bs:${bs}KiB walsize:${walsize}KiB"

            appendstr="with-append"
            ./build.sh y ${bs} ${depth} n $1 $barrier

            setup_zenfs "${dir}/rocksdb-raw" $1;
            ## fillrandom
            pushd  "${dir}/rocksdb-raw"

            echo "FILLRANDOM"
            sudo LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/user/src/spdk/dpdk/build/lib/ \
                ./db_bench \
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
                        1> ../${recov_dir}/emu_fillrandom_out_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 \
                        2> ../${recov_dir}/emu_fillrandom_err_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2

            echo "FILLOVERWRITE"
            sudo LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/user/src/spdk/dpdk/build/lib/ \
                ./db_bench \
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
                        1> ../${recov_dir}/emu_overwrite_out_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 \
                        2> ../${recov_dir}/emu_overwrite_err_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 
            popd
            done
        done
    done
done


# for bs in 4; do
# for walsize in 262144 16384 32768 65536 131072; do
#     # for barrier in $((8192*2)) 8192 4096 128 256 512 1024 2048; do
#     for barrier in none; do
#         for depth in 32; do
#             #for walsize in 16384 32768 65536 131072 262144; do
#             # Setup environment
#             echo "barrier: ${barrier}KiB depth:${depth}QD bs:${bs}KiB walsize:${walsize}KiB"
                    
#             appendstr="without-append"
#             ./build.sh n ${bs} ${depth} n $1 $barrier

#             setup_zenfs "${dir}/rocksdb-raw" $1;
#             ## fillrandom
#             pushd  "${dir}/rocksdb-raw"

#             echo "FILLRANDOM"
#             sudo LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/user/src/spdk/dpdk/build/lib/ \
#                 ./db_bench \
#                     --fs_uri=zenfs://dev:${1} \
#                     --benchmarks=fillrandom \
#                     --use_direct_io_for_flush_and_compaction \
#                     --value_size=3980 \
#                     --key_size=16 \
#                     --num=${walsize} \
#                     --compression_type=none \
#                     --threads=1 \
#                     --histogram \
#                     --use_existing_db=0 \
#                     --wal_ttl_seconds=1 \
#                     --write_buffer_size=$((1024*1024*1024*2)) \
#                     --target_file_size_base=2147483648 \
#                         1> ../${recov_dir}/emu_fillrandom_out_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 \
#                         2> ../${recov_dir}/emu_fillrandom_err_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2


#             echo "FILLOVERWRITE"
#             sudo LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/user/src/spdk/dpdk/build/lib/ \
#                 ./db_bench \
#                     --fs_uri=zenfs://dev:${1} \
#                     --benchmarks=overwrite \
#                     --use_direct_io_for_flush_and_compaction \
#                     --value_size=3980 \
#                     --key_size=16 \
#                     --num=10 \
#                     --compression_type=none \
#                     --threads=1 \
#                     --histogram \
#                     --use_existing_db=1 \
#                     --wal_ttl_seconds=1 \
#                     --write_buffer_size=$((1024*1024*1024*2)) \
#                     --target_file_size_base=2147483648 \
#                         1> ../${recov_dir}/emu_overwrite_out_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 \
#                         2> ../${recov_dir}/emu_overwrite_err_${barrier}_${depth}QD_${bs}KiB_${walsize}.$2 
#             popd
#             done
#         done
#     done
# done
