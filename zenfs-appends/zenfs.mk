
zenfs_SOURCES-y = \
	fs/fs_zenfs.cc \
	fs/zbd_zenfs.cc \
	fs/io_zenfs.cc \
	fs/zonefs_zenfs.cc \
	fs/zbdlib_zenfs.cc

zenfs_HEADERS-y = \
	fs/fs_zenfs.h \
	fs/zbd_zenfs.h \
	fs/io_zenfs.h \
	fs/version.h \
	fs/metrics.h \
	fs/snapshot.h \
	fs/filesystem_utility.h \
	fs/zonefs_zenfs.h \
	fs/zbdlib_zenfs.h


PKG_CONFIG_PATH = $(SPDK_DIR)/build/lib/pkgconfig
SPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_nvme)
DPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_env_dpdk)
SYS_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)

zenfs_PKGCONFIG_REQUIRES-y += "libzbd >= 1.5.0"

ZENFS_EXPORT_PROMETHEUS ?= n
zenfs_HEADERS-$(ZENFS_EXPORT_PROMETHEUS) += fs/metrics_prometheus.h
zenfs_SOURCES-$(ZENFS_EXPORT_PROMETHEUS) += fs/metrics_prometheus.cc
zenfs_CXXFLAGS-$(ZENFS_EXPORT_PROMETHEUS) += -DZENFS_EXPORT_PROMETHEUS=1
zenfs_PKGCONFIG_REQUIRES-$(ZENFS_EXPORT_PROMETHEUS) += ", prometheus-cpp-pull == 1.1.0"

zenfs_SOURCES += $(zenfs_SOURCES-y)
zenfs_HEADERS += $(zenfs_HEADERS-y)
zenfs_CXXFLAGS += $(zenfs_CXXFLAGS-y)
zenfs_CXXFLAGS += -I/usr/local/include
zenfs_LDFLAGS += -u zenfs_filesystem_reg -lszd -lszd_extended -Wl,--no-as-needed -luuid $(SPDK_LIB) $(DPDK_LIB) -Wl,--as-needed

#-Wl,--whole-archive spdk_log.a spdk_env_dpdk.a spdk_nvme.a spdk_util.a spdk_sock.a spdk_json.a spdk_vfio_user.a spdk_rpc.a spdk_jsonrpc.a spdk_trace.a -Wl,--no-whole-archive rte_eal.a rte_mempool.a rte_telemetry.a rte_ring.a rte_kvargs.a rte_bus_pci.a rte_pci.a rte_vhost.a rte_power.a isal.a uuid numa dl rt

ZENFS_ROOT_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

zenfs_PKGCONFIG_REQUIRES = $(zenfs_PKGCONFIG_REQUIRES-y)
