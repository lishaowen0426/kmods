obj-m += zpmem.o
obj-m += ztest.o
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD)  modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
