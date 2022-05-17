obj-m += zpmem.o
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) $(MAC) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
