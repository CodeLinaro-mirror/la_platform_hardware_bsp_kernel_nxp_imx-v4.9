#  Makefile for Gasket device drivers.

obj-$(CONFIG_GASKET_FRAMEWORK) += gasket.o
obj-$(CONFIG_APEX_DRIVER) += apex.o

PWD := $(shell pwd)

# Set this based on your kernel config
MODULE_HASH := sha1

gasket-objs	:= gasket_core.o gasket_interrupt.o gasket_page_table.o gasket_sysfs.o gasket_ioctl.o
ifeq ($(APEX_VARIANT),)
apex-objs	:= apex_driver.o
else
# Allows for in kernel compiling in x86 kernel
apex-objs	:= $(APEX_VARIANT)_driver.o
endif

default:
	$(MAKE) CFLAGS='-s' ARCH=arm64 CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_DIR) SUBDIRS=$(PWD) modules
ifeq ($(SIGN_MODULE),yes)
	$(KERNEL_SCRIPTS_DIR)/sign-file $(MODULE_HASH) $(KERNEL_DIR)/signing_key.priv $(KERNEL_DIR)/signing_key.x509 gasket.ko
	$(KERNEL_SCRIPTS_DIR)/sign-file $(MODULE_HASH) $(KERNEL_DIR)/signing_key.priv $(KERNEL_DIR)/signing_key.x509 apex.ko
endif

clean:
	@rm -rf *.o .*.cmd .tmp_versions/ Module.symvers *.ko *.mod.c *.mod.o modules.order
