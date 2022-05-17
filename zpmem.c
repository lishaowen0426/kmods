#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt


#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/zpool.h>
#include <linux/spinlock.h>
#include <linux/blkdev.h>
#include <linux/dax.h>


static int major = 0;
static int minor = 0;
module_param( major, int,  0644);
module_param( minor, int,  0644);
MODULE_PARM_DESC(major, "pmem device major number");
MODULE_PARM_DESC(minor, "pmem device minor number");

struct zpmem_dev{
    struct dax_device* dax_dev;
};

struct zpmem_pool;

struct zpmem_ops {
	int (*evict)(struct zpmem_pool *pool, unsigned long handle);
};
struct zpmem_pool{
    spinlock_t lock;

    struct list_head lru;
    u64 pages_nr;
    const struct zpmem_ops *ops;
    struct zpool *zpool;
    const struct zpool_ops *zpool_ops;

};


static char *_zpmem_claim_ptr = "I belong to zpmem";

static int zpmem_dev_ctr(void){

    dev_t dev;
    struct block_device* bdev;
    fmode_t mode;
    struct dax_device* dax_dev;

#ifndef LABOS
    u64 part_off;
#endif

    if(major == 0){
        pr_err("major number is 0");
        return -ENODEV;
    }

    dev = MKDEV(major, minor);
    if(MAJOR(dev) != major || MINOR(dev) != minor)
        return -EOVERFLOW;
    else
        pr_info("find dev\n");
   
    mode = FMODE_READ | FMODE_WRITE | FMODE_LSEEK | FMODE_EXCL;
    bdev = blkdev_get_by_dev(dev,mode, _zpmem_claim_ptr); 
    if(IS_ERR(bdev))
        return PTR_ERR(bdev);

    part_off;

#ifdef LABOS
    dax_dev = fs_dax_get_by_bdev(bdev);
#else
    dax_dev = fs_dax_get_by_bdev(bdev, &part_off);
#endif

    if(IS_ERR(dax_dev))
        return PTR_ERR(dax_dev);
    return 0;
}


static void *zpmem_zpool_create(const char *name, gfp_t gfp,
			       const struct zpool_ops *zpool_ops,
			       struct zpool *zpool)
{
    return NULL;
}

static void zpmem_zpool_destroy(void *pool)
{
}

static int zpmem_zpool_malloc(void *pool, size_t size, gfp_t gfp,
			unsigned long *handle)
{
    return 0;
}
static void zpmem_zpool_free(void *pool, unsigned long handle)
{
}

static int zpmem_zpool_shrink(void *pool, unsigned int pages,
			unsigned int *reclaimed)
{
    return 0;

}

static void *zpmem_zpool_map(void *pool, unsigned long handle,
			enum zpool_mapmode mm)
{
    return NULL;
}
static void zpmem_zpool_unmap(void *pool, unsigned long handle)
{
}

static u64 zpmem_zpool_total_size(void *pool)
{
    return 0;
}

static struct zpool_driver zpmem_zpool_driver = {
	.type =		"zpmem",
	.sleep_mapped = true,
	.owner =	THIS_MODULE,
	.create =	zpmem_zpool_create,
	.destroy =	zpmem_zpool_destroy,
	.malloc =	zpmem_zpool_malloc,
	.free =		zpmem_zpool_free,
	.shrink =	zpmem_zpool_shrink,
	.map =		zpmem_zpool_map,
	.unmap =	zpmem_zpool_unmap,
	.total_size =	zpmem_zpool_total_size,

};


static int __init init_zpmem(void){
    pr_info("loaded\n");
    

    return zpmem_dev_ctr();
}


static void __exit exit_zpmem(void){
    pr_info("exited\n");
}

module_init(init_zpmem);
module_exit(exit_zpmem);

MODULE_ALIAS("zpool-zpmem");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Li Shaowen");
MODULE_DESCRIPTION("PMEM allocator module");
