obj-$(CONFIG_WILC1000) += wilc1000.o

ccflags-$(CONFIG_WILC1000_SDIO) += -DCOMPLEMENT_BOOT

ccflags-y += -DSTA_FIRMWARE=\"atmel/wilc1000_fw.bin\" \
		-DAP_FIRMWARE=\"atmel/wilc1000_ap_fw.bin\" \
		-DP2P_CONCURRENCY_FIRMWARE=\"atmel/wilc1000_p2p_fw.bin\"

ccflags-y += -I$(src)/ -D__CHECK_ENDIAN__ -DWILC_ASIC_A0 -DWILC_DEBUGFS
#ccflags-y += -DTCP_ACK_FILTER

ccflags-$(CONFIG_WILC1000_PREALLOCATE_AT_LOADING_DRIVER) += -DMEMORY_STATIC \
								-DWILC_PREALLOC_AT_INSMOD

ccflags-$(CONFIG_WILC1000_DYNAMICALLY_ALLOCATE_MEMROY) += -DWILC_NORMAL_ALLOC


wilc1000-objs := wilc_wfi_cfgoperations.o linux_wlan.o linux_mon.o \
			wilc_msgqueue.o \
			coreconfigurator.o host_interface.o \
			wilc_wlan_cfg.o wilc_debugfs.o \
			wilc_wlan.o

obj-$(CONFIG_WILC1000_SDIO) += wilc1000-sdio.o
wilc1000-sdio-objs += linux_wlan_sdio.o wilc_sdio.o

obj-$(CONFIG_WILC1000_SPI) += wilc1000-spi.o
wilc1000-spi-objs += linux_wlan_spi.o wilc_spi.o
