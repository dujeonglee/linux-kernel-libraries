obj-m += bhm_test.o
bhm_test-y := bh_manager.o bhm_harness.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load: all
	sudo insmod bhm_test.ko

unload:
	sudo rmmod bhm_test

reload: unload load

log:
	sudo dmesg -w

test:
	@./run_test.sh

.PHONY: all clean load unload reload log test
