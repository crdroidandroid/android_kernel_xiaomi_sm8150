ccflags-y += -Iinclude/linux
obj-$(CONFIG_SEEMP_CORE) += seemp_core.o
seemp_core-objs:= seemp_ringbuf.o seemp_event_encoder.o

ifdef CONFIG_DEBUG_KERNEL
seemp_core-objs += seemp_logk.o
endif
