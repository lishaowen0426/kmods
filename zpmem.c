#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt


#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/zpool.h>
#include <linux/spinlock.h>
#include <linux/blkdev.h>
#include <linux/dax.h>
#include <linux/pfn_t.h>
#include <linux/version.h>
#include <asm/io.h>

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
    struct block_device* bdev;
    fmode_t mode;

    void* memory_map;
    u64 memory_map_size;
    struct page** pages;
};


struct zpmem_pool;

struct zpmem_ops {
	int (*evict)(struct zpmem_pool *pool, unsigned long handle);
};
struct zpmem_pool{
    spinlock_t lock;
    struct zpmem_dev* pmem_dev;
    struct list_head lru;
    struct page** pages;
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
        pr_err("blkdev_get_by_dev failed, %ld", PTR_ERR(bdev));
        return NULL;
    } 
    memory_map_size = (loff_t)bdev_nr_sectors(bdev)<<SECTOR_SHIFT; 
    pr_info("memory_map_size: %llu", memory_map_size);

#ifdef NEW_FS_DAX_GET
    dax_dev = fs_dax_get_by_bdev(bdev, &part_off);
#else
    dax_dev = fs_dax_get_by_bdev(bdev);
#endif
    if(!dax_dev){
        pr_err("fs_dax_get returns null dax_device");
        goto dev_ctr_err1;
    }


    zdev = kzalloc(sizeof(struct zpmem_dev), GFP_KERNEL);
    if(!zdev){
        pr_err("kzalloc zpmem_dev failed");
        goto dev_ctr_err2;
    }

    zdev->dax_dev = dax_dev;
    zdev->bdev = bdev;
    zdev->mode = mode;
    zdev->memory_map_size = memory_map_size;
    pr_info("dev ctr success");
    return zdev;

dev_ctr_err1:
    blkdev_put(bdev, mode);
    return NULL;
dev_ctr_err2:
    blkdev_put(bdev, mode);
    put_dax(dax_dev);
    return NULL;

}

static int zpmem_page_create(struct zpmem_dev* zdev){

   
    int r;
    struct page** pages;
    u64 s;
    long p, da,i;
    int id;
    sector_t offset;
    pfn_t pfn;

    s = zdev->memory_map_size;
    p = s >> PAGE_SHIFT;
    if(!p){
        r = -EINVAL;
        goto page_create_err1;
    }
    if(p != s>> PAGE_SHIFT){
        r = -EOVERFLOW;
        goto page_create_err1;
    }

    offset = get_start_sect(zdev->bdev);
    pr_info("pmem start offset: %llu", offset);
    //page aligned offset
    if (offset & (PAGE_SIZE / 512 - 1)) {
            r = -EINVAL;
            goto page_create_err1;
    }
    
    id = dax_read_lock();

    da = dax_direct_access(zdev->dax_dev, offset, p, &zdev->memory_map, &pfn);
    if(da < 0){
        pr_err("dax_direct_access failed: %ld", da);
        zdev->memory_map = NULL;
        r = da;
        goto page_create_err2; 
    }
    if(!pfn_t_has_page(pfn)){
        pr_err("pfn_t_has_page failed");
        zdev->memory_map = NULL;
        r = -EOPNOTSUPP;
        goto page_create_err2;
    }
    if(!zdev->memory_map){
        pr_err("memory_map is null");
        goto page_create_err2;
    }
    if(da != p){
        //ONLY FOR PMEM, offset = 0, da = p, not sure if this is exactly right...
        pr_err("da != p, da = %ld, p = %ld", da ,p);
        goto page_create_err2;
    }
    pr_info("return pages: %ld, request pages: %ld, pfn:%llu, pfn_to_virt: %p , memory_map:%p", da,p,pfn.val, phys_to_virt(pfn_t_to_phys(pfn)), zdev->memory_map);
    

    pages = kvmalloc_array(p, sizeof(struct page *), GFP_KERNEL);
    if (!pages) {
            r = -ENOMEM;
            goto page_create_err2;
    }
    i = 0;
    while(i < da){
        pages[i++] = pfn_t_to_page(pfn);
        pfn.val++;
    }
    pr_info("Initialize pmem pages success");
    
    zdev->pages = pages;

    dax_read_unlock(id); 
    return 0;

page_create_err3:
    kvfree(pages);
page_create_err2:
    dax_read_unlock(id);
page_create_err1:
    return r;


}

static struct zpmem_pool *zpmem_create_pool(gfp_t gfp, const struct zpmem_ops *ops)
{
	struct zpmem_pool *pool;
        struct zpmem_dev* zdev;
	pool = kzalloc(sizeof(struct zpmem_pool), gfp);
	if (!pool){
            pr_err("kzalloc zpmem_pool failed");
            return NULL;
        }
        zdev = zpmem_dev_ctr();
        if(!zdev){
            goto create_pool_err1;
        }

        if(zpmem_page_create(zdev)){
           goto create_pool_err2; 
        }
	spin_lock_init(&pool->lock);
	INIT_LIST_HEAD(&pool->lru);
	pool->pages_nr = 0;
	pool->ops = ops;
        pool->pmem_dev = zdev; 
	return pool;

create_pool_err2:
        kfree(zdev);
create_pool_err1:
        kfree(pool);
        return NULL;
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
    struct zpmem_pool* zpool = (struct zpmem_pool*)pool;
    blkdev_put(zpool->pmem_dev->bdev, zpool->pmem_dev->mode);
    put_dax(zpool->pmem_dev->dax_dev);
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
