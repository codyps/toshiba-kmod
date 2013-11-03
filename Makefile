
ifneq ($(KERNELRELEASE),)
obj-m := toshiba.o

else
KVER_ := $(shell uname -r)
KVER  ?= $(KVER_)
KDIR  ?= /lib/modules/$(KVER)/build

modules :
	make -C $(KDIR) M=$(PWD) modules

clean :
	make -C $(KDIR) M=$(PWD) clean

install :
	make -C $(KDIR) M=$(PWD) modules_install

endif
