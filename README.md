# ZWAL

# Motivation: Why use zone appends?

To repeat the experiment for `Motivation: Why use zone appends?`, first build our custom version of fio with:

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

All data is stored in: `data/fio/f2_<nvmexny>_uring_*`.
The format is `f2_<nvmexny>_uring_with_append_<bs>_<qd>` or `f2_<nvmexny>_uring_without_append_<bs>`.

Note that we do not include scripts/data for different blocksizes than 8192, this is, however, trivial to change. Change the `for bs in 8192` in the bash script.

# WAL write performance

To repeat the experiments necessary to get the data of figure 4, run:

```bash
for i in 1 2 3; do bash ./run-zwal-scaling.sh <nvmexny> $i; done
```

This script builds all tooling automatically.

The data is stored in: `data/scaling/`. The `err_` files contain stderr and `out_` stdout. The general format is `<barrier_size>_<QD>QD_<BS>KiB.<runid>` with appends and `none_<BS>KiB.<runid>` without.

# WAL recovery

To repeat the experiments necessary to get the data of figure 5, run:

```bash
for i in 1 2 3 4 5; do bash ./run-zwal-recovery.sh <nvmexny> $i; done
```

This script builds all tooling automatically.

The data is stored in: `data/recovery/`. The `overwrite_err_` files contain stderr and `overwrite_out_` stdout. The general format is `<barrier_size>_<QD>QD_<BS>KiB.<runid>` with appends and `none_<QD>QD_<BS>KiB.<runid>` without.

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

The data is stored in: `data/ycsb/`. Data for the commercial device is stored in `real*`, for the emulated device in `emu*`. The rest of the file format is `<workload>-load-<barriersize>-<datetime>.out` for loading the workload and `<workload>-run-<barriersize>-<datetime>.out` for running the workload. Note that ZenFS without appends uses a barrier of `none`.

# Barrier size

All data for the barrier size is already present in `WAL write performance` and `WAL recovery`, no additional experiment needs to be run.
