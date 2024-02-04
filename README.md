# ZWAL : Rethinking Write-ahead Logs for ZNS SSDs

ZWAL is a new WAL design for ZNS that uses `zone appends` instead of `writes`. It leads to significantly higher write throughput and concurrency compared to writes without resorting to buffered I/O on the host. The implementation is build on top of [ZenFS](https://github.com/westerndigitalcorporation/zenfs).

# Dependencies

ZWALs have the same requirements as ZenFS, but have one additional requirement. This requirement is Linux with support fo io_uring with NVMe passthrough is required (> 6.0). ZenFS depends on libzbd. To use ZenFS on SSDs with Zoned Namespaces, Linux kernel 5.9 or later is required. ZenFS works with RocksDB version v6.19.3 or later.

# Build

ZWALs come with a number of configuration options that are defined in `#define`.
These must be set before compilation. Apart from this the build is no different from ZenFS. ZWALs *do* require a specific change in RocksDB, hence we ship RocksDB along with ZWALs (see `rocksdb-raw`).

```bash
rm -r rocksdb-raw/plugin/zenfs
cp zenfs plugin/zenfs-appends rocksdb-raw/plugin/zenfs

# Set WAL buffersize
BUFFSIZE=4
sed -i "s/#define SPARSE_BUFFER_SIZE_IN_KB.*/#define SPARSE_BUFFER_SIZE_IN_KB ${BUFFSIZE}UL/g" rocksdb-raw/plugin/zenfs/fs/io_zenfs.h

# set WAL max depth
MAXWALDEPTH=32
sed -i "s/NAMELESS_WAL_DEPTH.*/NAMELESS_WAL_DEPTH ${MAXWALDEPTH}/g" rocksdb-raw/plugin/zenfs/fs/zbd_zenfs.h

# Set WAL barriersize
WALBARRIERSIZE=16384
sed -i "s/#define WAL_BARRIER_SIZE_IN_KB.*/#define WAL_BARRIER_SIZE_IN_KB ${6}UL/g" rocksdb-raw/plugin/zenfs/fs/io_zenfs.h

cd rocksdb
DEBUG_LEVEL=0 ROCKSDB_PLUGINS=zenfs make -j48 db_bench
sudo DEBUG_LEVEL=0 ROCKSDB_PLUGINS=zenfs make install
cd plugin/zenfs/util
make
```

# Creating ZenFS with ZWALs

The build procedure is the same as for ZenFS. However, we only evaluated extensively under the default configuration of:

```bash
echo deadline | sudo tee /sys/class/block/<zoned block device>/queue/scheduler
rocksdb-raw/plugin/zenfs/util/zenfs mkfs --zbd=<zoned block device> --aux_path=<path to store LOG and LOCK files>
```

We provide no guarantees over other ZenFS functionalities.

# Structure of this repository

* `zenfs-appends`: ZenFS with ZWALs implemented.
* `zenfs-default`: Standard ZenFS (included for easy experimentation). We added support for variable buffer sizes.
* `rocksdb-raw`: RocksDB modified to delete WALs instantly instead of archiving them
* `rocksdb-ycsb`: The same as `rocksdb-raw`, but always forces RocksDB to use ZenFS. Necessary to use with YCSB.
* `ycsb`: The YCSB benchmark, modified to support RocksDB with ZenFS
* `data`: raw data from all our experiments
* `fio-with-appends`: fio modified to support appends for io_uring with NVMe passthrough
* `AE.md`: Artifact Evaluation. Contains a description of how to reproduce all results from the paper.
