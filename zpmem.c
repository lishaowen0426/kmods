#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt


#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/zpool.h>
#include <linux/spinlock.h>
#include <linux/blkdev.h>
#include <linux/dax.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
#define NEW_FS_DAX_GET 
#endif



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
    struct zpmem_dev* pmem_dev;
    struct list_head lru;
    u64 pages_nr;
    const struct zpmem_ops *ops;
    struct zpool *zpool;
    const struct zpool_ops *zpool_ops;

};

static struct zpmem_pool* z_pool;

static int zpmem_zpool_evict(struct zpmem_pool *pool, unsigned long handle)
{
    return 0;
}

static const struct zpmem_ops zpmem_zpool_ops = {
	.evict =	zpmem_zpool_evict
};
static char *_zpmem_claim_ptr = "I belong to zpmem";

static struct zpmem_dev* zpmem_dev_ctr(void){

    dev_t dev;
    struct block_device* bdev;
    fmode_t mode;
    struct dax_device* dax_dev;
    struct zpmem_dev* zdev;
    u64 memory_map_size;
    u64 part_off __maybe_unused;

    if(major == 0){
        pr_err("major number is 0");
        return NULL;
    }

    dev = MKDEV(major, minor);
    if(MAJOR(dev) != major || MINOR(dev) != minor){
        pr_err("dev number overflow");
        return NULL;
    }
   
    mode = FMODE_READ | FMODE_WRITE | FMODE_LSEEK | FMODE_EXCL;
    bdev = blkdev_get_by_dev(dev,mode, _zpmem_claim_ptr); 
    if(IS_ERR(bdev)){
        pr_err("blkdev_get_by_dev failed");
        return NULL;
    } 
    memory_map_size = (loff_t)bdev_nr_sectors(bdev)<<SECTOR_SHIFT; 
    pr_info("memory_map_size: %llu", memory_map_size);

#ifdef NEW_FS_DAX_GET
    dax_dev = fs_dax_get_by_bdev(bdev, &part_off);
#else
    dax_dev = fs_dax_get_by_bdev(bdev);
#endif
    if(IS_ERR(dax_dev)){
        pr_err("fs_dax_get_by_bdev failed");
        return NULL;
    }

    zdev = kzalloc(sizeof(struct zpmem_dev), GFP_KERNEL);
    if(!zdev){
        pr_err("kzalloc zpmem_dev failed");
        return NULL;
    }

    zdev->dax_dev = dax_dev;
    pr_info("dev ctr success");
    return zdev;
}

static struct zpmem_pool *zpmem_create_pool(gfp_t gfp, const struct zpmem_ops *ops)
{
	struct zpmem_pool *pool;
	pool = kzalloc(sizeof(struct zpmem_pool), gfp);
	if (!pool){
            pr_err("kzalloc zpmem_pool failed");
            return NULL;
        }
	spin_lock_init(&pool->lock);
	INIT_LIST_HEAD(&pool->lru);
	pool->pages_nr = 0;
	pool->ops = ops;
        pool->pmem_dev = zpmem_dev_ctr(); 
	return pool;
}


static void *zpmem_zpool_create(const char *name, gfp_t gfp,
			       const struct zpool_ops *zpool_ops,
			       struct zpool *zpool)
{
    struct zpmem_pool *pool;

    pool = zpmem_create_pool(gfp, zpool_ops ? &zpmem_zpool_ops : NULL);
    if (pool) {
            pool->zpool = zpool;
            pool->zpool_ops = zpool_ops;
    }
    return pool;
}

static void zpmem_zpool_destroy(void *pool)
{
    put_dax(((struct zpmem_pool*)pool)->pmem_dev->dax_dev);
    kfree(pool);
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
    z_pool = zpmem_create_pool(GFP_KERNEL, &zpmem_zpool_ops); 
    if(!z_pool){
        return -ENOMEM;
    }
    pr_info("load success");
    return 0;
}


static void __exit exit_zpmem(void){
    zpmem_zpool_destroy(z_pool);
    pr_info("exit success\n");
}

module_init(init_zpmem);
module_exit(exit_zpmem);

MODULE_ALIAS("zpool-zpmem");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Li Shaowen");
MODULE_DESCRIPTION("PMEM allocator module");
