obj-$(CONFIG_MLX5_CORE)		+= mlx5_core.o

mlx5_core-y :=	main.o cmd.o debugfs.o fw.o eq.o uar.o pagealloc.o \
		health.o mcg.o cq.o srq.o alloc.o qp.o port.o mr.o pd.o   \
		mad.o transobj.o vport.o sriov.o fs_cmd.o fs_core.o
mlx5_core-$(CONFIG_MLX5_CORE_EN) += wq.o eswitch.o \
		en_main.o en_fs.o en_ethtool.o en_tx.o en_rx.o \
		en_txrx.o
