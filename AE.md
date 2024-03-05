# ZWAL Artifact Evaluation

This Artifact Evaluation describes how to reproduce the results for the CHEOPS 2024 "ZWAL: Rethinking Write-ahead Logs for ZNS SSDs" paper.
We detail the code and plots for each individual paper section into its own Markdown section. The instructions are written as if the experiments are conducted in order.

# Setup

For most experiments, we make use of [ConfZNS](https://github.com/DKU-StarLab/ConfZNS).
In order to run ZenFS with ConfZNS, it is needed to change `#define MK_ZONE_CONVENTIONAL 5` in `hw/femu/nvme.h` to `#define MK_ZONE_CONVENTIONAL 0`. ZenFS does not support conventional zones.

Additionally, we change the default onfigurations in `ConfZNS` to achieve higher intra-zone parallelism:

* `NVME_DEFAULT_ZONE_SIZE` :`2 * GiB`
* `NVME_DEFAULT_MAX_AZ_SIZE` : `4 * MiB`
* `spp->pg_rd_lat`: `NAND_READ_LATENCY`
* `spp->pg_wrt_lat`: `NAND_PROG_LATENCY`
* `spp->blk_er_lat`: 1000
* `spp->nchnls`: 32
* `spp->chnls_per_zone`: 32
* `spp->ways`: 2
* `spp->ways_per_zone`: 2
* `spp->dies_per_chip`: 1
* `spp->planes_per_die`: 4
* `spp->register_model`: 1

We provide the other arguments we use to run the emulated SSD in `run-femu.sh`.
To get similar results as provided in the paper, it is recommended to use the same VM setup.

# Motivation: Why use zone appends?

To repeat the experiment for the `Motivation: Why use zone appends?` section, first build our custom version of fio with:

```bash
pushd fio-with-appends
./configure
make
popd
```

Then run the experiment on a ZNS SSD with (WARNING deletes all data on the SSD):

```bash
bash run-fio-scaling.sh <nvmexny>
```

All of the experiment's data is stored in: `data/fio/f2_<nvmexny>_uring_*`. By default it contains the data of our runs (can be used as a sanity check).
We use the following naming convention in the data dir:`f2_<nvmexny>_uring_with_append_<bs>_<qd>` for appends and `f2_<nvmexny>_uring_without_append_<bs>` for writes.

Note that we do not include scripts/data for blocksizes different than 8192 in this repository, this is, however, trivial to change. Change the `for bs in 8192` in the bash script.

# WAL write performance

To repeat the experiments necessary to get the data of figure 4, run:

```bash
for i in 1 2 3; do bash ./run-zwal-scaling.sh <nvmexny> $i; done
```

This script (re)builds all tooling automatically, including RocksDB, ZenFS and ZWALs.

The data is stored in: `data/scaling/`. The `err_` files contain stderr and `out_` stdout. The naming convention for files is `<barrier_size>_<QD>QD_<BS>KiB.<runid>` for ZWALs and `none_<BS>KiB.<runid>` for traditional ZenFS.

# WAL recovery

To repeat the experiments necessary to get the data of figure 5, we first have to enable various "timers". These are disabled by default to reduce overhead.
To enable the timers, change `//#define MEASURE_WAL_LAT` to `#define MEASURE_WAL_LAT` in the `zenfs-appends/fs/io_zenfs.h` file. Please comment the define again after the experiment.

Then run (the rebuild is already included in the script):

```bash
for i in 1 2 3 4 5; do bash ./run-zwal-recovery.sh <nvmexny> $i; done
```

The data is stored in: `data/recovery/`. The `overwrite_err_` files contain stderr and `overwrite_out_` stdout. The naming convention for files is `<barrier_size>_<QD>QD_<BS>KiB.<runid>` for ZWALs and `none_<QD>QD_<BS>KiB.<runid>` for traditional ZenFS.

# YCSB

To repeat the experiments necessary to get the data of figure 6, run:

```bash
# Commercial SSD, appends
bash ./run-zwal-ycsb.sh <nvmexny> y y
# Commercial SSD, writes
bash ./run-zwal-ycsb.sh <nvmexny> y n
# Emulated SSD, appends
bash ./run-zwal-ycsb.sh <nvmexny> n y
# Emulated SSD, writes
bash ./run-zwal-ycsb.sh <nvmexny> n n
```

The data is stored in: `data/ycsb/`. Data for the commercial device is stored in `real*`, for the emulated device in `emu*`. The rest of the file name consists out of `<workload>-load-<barriersize>-<datetime>.out` for loading the workload and `<workload>-run-<barriersize>-<datetime>.out` for running the workload (e.g., A--F). Note that ZenFS without appends uses a barrier of `none` as there is none.

# Barrier size

All data for the barrier size is already present in `WAL write performance` and `WAL recovery` experiemnts, no additional experiment needs to be run.
