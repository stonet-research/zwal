#!/bin/bash
set -e
dir="$(cd -P -- "$(dirname -- "$0")" && pwd -P)"
echo "Working dir: ${dir}"

if [ $# != 6 ]; then
    echo "Build requires six args:" \
         "1: use_appends (yes or no)" \
         "2: buffsize (in KiB)" \
         "3: WAL depth (max QD)" \
         "4: build .jar for YCSB" \
         "5: ZenFS device (for YCSB hack)" \
         "6: WAL barrier size (in KiB)"
    exit 1
fi

case $4 in 
    y*)
        rocksdbpath="${dir}/rocksdb-ycsb"
        ;;
    *)
        rocksdbpath="${dir}/rocksdb-raw"
        ;;
esac

# Reset ZenFS
sudo rm -rf ${rocksdbpath}/plugin/zenfs
case $1 in
    y*)
        cp -r zenfs-appends ${rocksdbpath}/plugin/zenfs 
        ;;
    *)
        cp -r zenfs-default ${rocksdbpath}/plugin/zenfs 
        ;;
esac

# Move to subdir
pushd ${rocksdbpath}
echo "Compiling at ${rocksdbpath}"

# Cleanup
mkdir -p buildlog
case $4 in 
    y*)
        make clean 2>&1 >buildlog/clean.out
        echo "Cleaned old RocksDB"
        ;;
    *)
        ;;
esac

# Apply buffersize
sed -i "s/#define SPARSE_BUFFER_SIZE_IN_KB.*/#define SPARSE_BUFFER_SIZE_IN_KB ${2}UL/g" plugin/zenfs/fs/io_zenfs.h
sed -i "s/#define WAL_BARRIER_SIZE_IN_KB.*/#define WAL_BARRIER_SIZE_IN_KB ${6}UL/g" plugin/zenfs/fs/io_zenfs.h
sed -i "s/NAMELESS_WAL_DEPTH.*/NAMELESS_WAL_DEPTH ${3}/g" plugin/zenfs/fs/zbd_zenfs.h
# Apply YCSB hack
sed -i "s/#define SED_DEVICE.*/#define SED_DEVICE \"${5}\"/g" env/env_posix.cc


# Install db_bench
echo "Building db_bench..."
LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:/home/user/src/spdk/dpdk/build/lib/" \
    DEBUG_LEVEL=0 \
    DISABLE_WARNING_AS_ERROR=1 \
    ROCKSDB_PLUGINS=zenfs \
    make -j 4 db_bench 2>&1 >buildlog/db_bench.out
echo "Completed building db_bench"

# Install globally for ZenFS
sudo LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:/home/user/src/spdk/dpdk/build/lib/" \
    DEBUG_LEVEL=0 \
    DISABLE_WARNING_AS_ERROR=1 \
    ROCKSDB_PLUGINS=zenfs \
    make install 2>&1 >buildlog/install.out
echo "Installing RocksDB"

# Build ZenFS util
echo "Building ZenFS at ${rocksdbpath}/plugin/zenfs/util..."
pushd plugin/zenfs/util
make clean
LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:/home/user/src/spdk/dpdk/build/lib/" \
    make  2>&1 >../../../buildlog/zenfs.out
popd
echo "Completed building ZenFS"

case $4 in 
    y*)
        # Build jni
       echo "Building RocksDB JNI..."
        LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:/home/user/src/spdk/dpdk/build/lib/" \
            JAVA_HOME=/usr/lib/jvm/java-11-openjdk-amd64 \
            DEBUG_LEVEL=0 \
            DISABLE_WARNING_AS_ERROR=1 \
            ROCKSDB_PLUGINS=zenfs \
            make -j 4 rocksdbjava 2>&1 >buildlog/java.out
        echo "Completed building RocksDB JNI"

        # Setup YCSB
        popd
        pushd ycsb
        sed  "s|ROCKS_PATH|${rocksdbpath}|g" \
            rocksdb/pom-template.xml > rocksdb/pom.xml
        mvn -pl site.ycsb:rocksdb-binding -am clean package 2>&1 >${rocksdbpath}/buildlog/ycsb.out
        echo "Modified YCSB"
        popd
        ;;
    *);;
esac
echo "DONE"
