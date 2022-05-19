#### Get Device number
```
ls -l /dev | grep ....
```

The first column is major, the second is minor


#### Install module

```
make
sudo insmod zpmem.ko major=XX minor=XX

dmesg | grep zpmem

echo zpmem | sudo tee /sys/module/zswap/parameters/zpool
```
