

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/blkdev.h>
#include <linux/dax.h>
#include <linux/zpool.h>
#include <linux/pfn_t.h>
#include <asm/io.h>
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

/*****************
 * Structures
*****************/
/*
 * NCHUNKS_ORDER determines the internal allocation granularity, effectively
 * adjusting internal fragmentation.  It also determines the number of
 * freelists maintained in each pool. NCHUNKS_ORDER of 6 means that the
 * allocation granularity will be in chunks of size PAGE_SIZE/64. As one chunk
 * in allocated page is occupied by zbud header, NCHUNKS will be calculated to
 * 63 which shows the max number of free chunks in zbud page, also there will be
 * 63 freelists per pool.
 */
#define NCHUNKS_ORDER	6

#define CHUNK_SHIFT	(PAGE_SHIFT - NCHUNKS_ORDER)
#define CHUNK_SIZE	(1 << CHUNK_SHIFT)
#define ZHDR_SIZE_ALIGNED CHUNK_SIZE
#define NCHUNKS		((PAGE_SIZE - ZHDR_SIZE_ALIGNED) >> CHUNK_SHIFT)

struct zpmem_pool;

struct zpmem_ops {
	int (*evict)(struct zpmem_pool *pool, unsigned long handle);
};




/**
 * struct zpmem_pool - stores metadata for each zbud pool
 * @lock:	protects all pool fields and first|last_chunk fields of any
 *		zbud page in the pool
 * @unbuddied:	array of lists tracking zbud pages that only contain one buddy;
 *		the lists each zbud page is added to depends on the size of
 *		its free region.
 * @buddied:	list tracking the zbud pages that contain two buddies;
 *		these zbud pages are full
 * @lru:	list tracking the zbud pages in LRU order by most recently
 *		added buddy.
 * @pages_nr:	number of zbud pages in the pool.
 * @ops:	pointer to a structure of user defined operations specified at
 *		pool creation time.
 * @zpool:	zpool driver
 * @zpool_ops:	zpool operations structure with an evict callback
 *
 * This structure is allocated at pool creation time and maintains metadata
 * pertaining to a particular zbud pool.
 */
struct zpmem_pool {
	spinlock_t lock;
	union {
		/*
		 * Reuse unbuddied[0] as buddied on the ground that
		 * unbuddied[0] is unused.
		 */
		struct list_head buddied;
		struct list_head unbuddied[NCHUNKS];
	};
	struct list_head lru;
	u64 pages_nr;
	const struct zpmem_ops *ops;
	struct zpool *zpool;
	const struct zpool_ops *zpool_ops;

        // PMEM 
        struct dax_device* dax_dev;
        struct block_device* bdev;
        fmode_t mode;
        void* memory_map;
        u64 memory_map_size;
        struct page** pages;
        struct list_head free;
        u64 free_nr;
        struct list_head used;
        u64 used_nr;

};

/*
 * struct zbud_header - zbud page metadata occupying the first chunk of each
 *			zbud page.
 * @buddy:	links the zbud page into the unbuddied/buddied lists in the pool
 * @lru:	links the zbud page into the lru list in the pool
 * @first_chunks:	the size of the first buddy in chunks, 0 if free
 * @last_chunks:	the size of the last buddy in chunks, 0 if free
 */
struct zpmem_header {
        
        struct list_head alloc;
        
	struct list_head buddy;
	struct list_head lru;
	unsigned int first_chunks;
	unsigned int last_chunks;
	bool under_reclaim;
};


enum buddy {
	FIRST,
	LAST
};

static int size_to_chunks(size_t size)
{
	return (size + CHUNK_SIZE - 1) >> CHUNK_SHIFT;
}

#define for_each_unbuddied_list(_iter, _begin) \
	for ((_iter) = (_begin); (_iter) < NCHUNKS; (_iter)++)

static struct zpmem_header *init_zpmem_page(struct page *page)
{
	struct zpmem_header *zhdr = page_address(page);
	zhdr->first_chunks = 0;
	zhdr->last_chunks = 0;
	INIT_LIST_HEAD(&zhdr->buddy);
	INIT_LIST_HEAD(&zhdr->lru);
	zhdr->under_reclaim = false;
	return zhdr;
}
/**
 * alloc and free require pool->lock is hold
 *
 * */
static struct page *__alloc_pmem_page(struct zpmem_pool *pool) {
        struct page *p;
        struct zpmem_header *zhdr;
        if(pool->free_nr == 0)
            goto alloc_pmem_err;

        zhdr = list_first_entry(&pool->free, struct zpmem_header, alloc);
	list_del(&zhdr->alloc);
        --pool->free_nr;
        list_add(&zhdr->alloc, &pool->used);
        ++pool->used_nr;

        p = virt_to_page((void*)zhdr);
        clear_page(p); 

alloc_pmem_err:
        return p;

}

static void __free_pmem_page(struct zpmem_pool *pool, struct zpmem_header *zhdr){

    list_del(&zhdr->alloc);
    --pool->used_nr;
    list_add(&zhdr->alloc, &pool->free);
    ++pool->free_nr;
}

static void free_zpmem_page(struct zpmem_pool *pool, struct zpmem_header *zhdr)
{
	//__free_page(virt_to_page(zhdr));
        __free_pmem_page(pool, zhdr);
}

/*
 * Encodes the handle of a particular buddy within a zbud page
 * Pool lock should be held as this function accesses first|last_chunks
 */
static unsigned long encode_handle(struct zpmem_header *zhdr, enum buddy bud)
{
	unsigned long handle;

	/*
	 * For now, the encoded handle is actually just the pointer to the data
	 * but this might not always be the case.  A little information hiding.
	 * Add CHUNK_SIZE to the handle if it is the first allocation to jump
	 * over the zbud header in the first chunk.
	 */
	handle = (unsigned long)zhdr;
	if (bud == FIRST)
		/* skip over zbud header */
		handle += ZHDR_SIZE_ALIGNED;
	else /* bud == LAST */
		handle += PAGE_SIZE - (zhdr->last_chunks  << CHUNK_SHIFT);
	return handle;
}

/* Returns the zbud page where a given handle is stored */
static struct zpmem_header *handle_to_zpmem_header(unsigned long handle)
{
	return (struct zpmem_header *)(handle & PAGE_MASK);
}

/* Returns the number of free chunks in a zbud page */
static int num_free_chunks(struct zpmem_header *zhdr)
{
	/*
	 * Rather than branch for different situations, just use the fact that
	 * free buddies have a length of zero to simplify everything.
	 */
	return NCHUNKS - zhdr->first_chunks - zhdr->last_chunks;
}



/*****************
 * API Functions
*****************/


static char *_zpmem_claim_ptr = "I belong to zpmem";
static int init_zpmem_dax(struct zpmem_pool* pool){

    dev_t dev;
    struct block_device* bdev;
    fmode_t mode;
    struct dax_device* dax_dev;
    int r;
    u64 memory_map_size;
    u64 part_off __maybe_unused;

    if(major == 0){
        pr_err("major number is 0");
        return -ENODEV;
    }

    dev = MKDEV(major, minor);
    if(MAJOR(dev) != major || MINOR(dev) != minor){
        pr_err("dev number overflow");
        return -EOVERFLOW;
    }
   
    mode = FMODE_READ | FMODE_WRITE | FMODE_LSEEK | FMODE_EXCL;
    bdev = blkdev_get_by_dev(dev,mode, _zpmem_claim_ptr); 
    if(IS_ERR(bdev)){
        pr_err("blkdev_get_by_dev failed, %ld", PTR_ERR(bdev));
        return PTR_ERR(bdev);
    } 
    memory_map_size = (loff_t)bdev_nr_sectors(bdev)<<SECTOR_SHIFT; 
    pr_info("memory_map_size: %llu", memory_map_size);

#ifdef NEW_FS_DAX_GET
    dax_dev = fs_dax_get_by_bdev(bdev, &part_off);
#else
    dax_dev = fs_dax_get_by_bdev(bdev);
#endif
    if(IS_ERR(dax_dev)){
        pr_err("fs_dax_get failed");
        r = PTR_ERR(dax_dev);
        goto dax_err;
    }

    pool->dax_dev = dax_dev;
    pool->bdev = bdev;
    pool->mode = mode;
    pool->memory_map_size = memory_map_size;
    pr_info("init_zpmem_dax succeed");
    return 0;

dax_err:
    blkdev_put(bdev,mode);
    return r;
}

static int init_zpmem_pages(struct zpmem_pool* pool){
    
    struct page** pages;
    struct page* pp;
    struct zpmem_header *zhdr;
    u64 s;
    long p, da,i;
    int id;
    sector_t offset;
    pfn_t pfn;

    if(!pool->dax_dev || !pool->bdev){
        pr_err("null device");
        return -ENODEV; 
    }

    s = pool->memory_map_size;
    p = s >> PAGE_SHIFT;
    if(!p){
        pr_err("0 page");
        return -EINVAL;
    }
    if(p != s >> PAGE_SHIFT){
        pr_err("number of pages overflows");
        return -EOVERFLOW;
    }

    offset = get_start_sect(pool->bdev);
    if(offset & (PAGE_SIZE/512 -1)){
        pr_err("offset is not aligned");
        return -EINVAL;
    }

    id = dax_read_lock();
    da = dax_direct_access(pool->dax_dev, offset, p, &pool->memory_map, &pfn);
    if(da < 0){
        pr_err("dax_direct_access failed: %ld", da);
        pool->memory_map = NULL;
        return da;
    }

    if(!pfn_t_has_page(pfn)){
        pr_err("pft_t_has_page failed");
        pool->memory_map = NULL;
        return -EOPNOTSUPP;
    }
    if(!pool->memory_map){
        pr_err("memory_map is null");
        return  -EOPNOTSUPP;
    }

    if(da != p){
        pr_err("da != p, da = %ld, p = %ld", da,p);
        return -EOPNOTSUPP;
    }
    pr_info("return pages: %ld, request pages: %ld, pfn:%llu, pfn_to_virt: %p , memory_map:%p", da,p,pfn.val, phys_to_virt(pfn_t_to_phys(pfn)), pool->memory_map);



    pages = kvmalloc_array(p, sizeof(struct page *), GFP_KERNEL);
    if (!pages) {
        pr_err("malloc page array failed");
        return -ENOMEM;
    }
    i = 0;
    while(i < da){
        pages[i++] = pfn_t_to_page(pfn);
        pfn.val++;
    }
    pool->pages = pages;

    INIT_LIST_HEAD(&pool->free);
    INIT_LIST_HEAD(&pool->used);

    i = 0;
    while(i < da){
        
	zhdr = page_address(pages[i++]);
	INIT_LIST_HEAD(&zhdr->alloc);
	list_add(&zhdr->alloc,&pool->free);
    }
    pool->free_nr = da;
    pool->used_nr = 0;

    {
        //test
        list_for_each_entry(zhdr, &pool->free, alloc){
            pp = virt_to_page((void*)zhdr);
            if(page_address(pp) != zhdr) {
                pr_err("wrong address");
                return -EINVAL;
            }

        }
    }

    pr_info("Initialize %ld free pmem pages success", da);
    return 0;
}


/**
 * zbud_create_pool() - create a new zbud pool
 * @gfp:	gfp flags when allocating the zbud pool structure
 * @ops:	user-defined operations for the zbud pool
 *
 * Return: pointer to the new zbud pool or NULL if the metadata allocation
 * failed.
 */
static struct zpmem_pool *zpmem_create_pool(gfp_t gfp, const struct zpmem_ops *ops)
{
	struct zpmem_pool *pool;
	int i;

	pool = kzalloc(sizeof(struct zpmem_pool), gfp);
	if (!pool)
		return NULL;
	spin_lock_init(&pool->lock);
        
        if(init_zpmem_dax(pool)){
            pr_err("init_zpmem_dax failed");
            kfree(pool); 
            return NULL;
        }

        if(init_zpmem_pages(pool)){
            pr_err("init_zpmem_pages failed");
            kfree(pool);
            return NULL;
        }

	for_each_unbuddied_list(i, 0)
		INIT_LIST_HEAD(&pool->unbuddied[i]);
	INIT_LIST_HEAD(&pool->buddied);
	INIT_LIST_HEAD(&pool->lru);
	pool->pages_nr = 0;
	pool->ops = ops;
	return pool;
}

/**
 * zbud_destroy_pool() - destroys an existing zbud pool
 * @pool:	the zbud pool to be destroyed
 *
 * The pool should be emptied before this function is called.
 */
static void zpmem_destroy_pool(struct zpmem_pool *pool)
{
        blkdev_put(pool->bdev, pool->mode);
        put_dax(pool->dax_dev);
        kvfree(pool->pages);
	kfree(pool);

}

/**
 * zbud_alloc() - allocates a region of a given size
 * @pool:	zbud pool from which to allocate
 * @size:	size in bytes of the desired allocation
 * @gfp:	gfp flags used if the pool needs to grow
 * @handle:	handle of the new allocation
 *
 * This function will attempt to find a free region in the pool large enough to
 * satisfy the allocation request.  A search of the unbuddied lists is
 * performed first. If no suitable free region is found, then a new page is
 * allocated and added to the pool to satisfy the request.
 *
 * gfp should not set __GFP_HIGHMEM as highmem pages cannot be used
 * as zbud pool pages.
 *
 * Return: 0 if success and handle is set, otherwise -EINVAL if the size or
 * gfp arguments are invalid or -ENOMEM if the pool was unable to allocate
 * a new page.
 */
static int zpmem_alloc(struct zpmem_pool *pool, size_t size, gfp_t gfp,
			unsigned long *handle)
{
	int chunks, i, freechunks;
	struct zpmem_header *zhdr = NULL;
	enum buddy bud;
	struct page *page;
        int r;

	if (!size || (gfp & __GFP_HIGHMEM))
		return -EINVAL;
	if (size > PAGE_SIZE - ZHDR_SIZE_ALIGNED - CHUNK_SIZE)
		return -ENOSPC;
	chunks = size_to_chunks(size);
	spin_lock(&pool->lock);

	/* First, try to find an unbuddied zbud page. */
	for_each_unbuddied_list(i, chunks) {
		if (!list_empty(&pool->unbuddied[i])) {
			zhdr = list_first_entry(&pool->unbuddied[i],
					struct zpmem_header, buddy);
			list_del(&zhdr->buddy);
			if (zhdr->first_chunks == 0)
				bud = FIRST;
			else
				bud = LAST;
			goto found;
		}
	}

	/* Couldn't find unbuddied zbud page, create new one */
	//spin_unlock(&pool->lock);
	//page = alloc_page(gfp);
	page = __alloc_pmem_page(pool);
	if (!page){
            r = -ENOMEM;
            goto unlock_and_return;
        }
	//spin_lock(&pool->lock);
	pool->pages_nr++;
	zhdr = init_zpmem_page(page);
	bud = FIRST;

found:
	if (bud == FIRST)
		zhdr->first_chunks = chunks;
	else
		zhdr->last_chunks = chunks;

	if (zhdr->first_chunks == 0 || zhdr->last_chunks == 0) {
		/* Add to unbuddied list */
		freechunks = num_free_chunks(zhdr);
		list_add(&zhdr->buddy, &pool->unbuddied[freechunks]);
	} else {
		/* Add to buddied list */
		list_add(&zhdr->buddy, &pool->buddied);
	}

	/* Add/move zbud page to beginning of LRU */
	if (!list_empty(&zhdr->lru))
		list_del(&zhdr->lru);
	list_add(&zhdr->lru, &pool->lru);

	*handle = encode_handle(zhdr, bud);
unlock_and_return:
	spin_unlock(&pool->lock);

	return r;
}

/**
 * zbud_free() - frees the allocation associated with the given handle
 * @pool:	pool in which the allocation resided
 * @handle:	handle associated with the allocation returned by zbud_alloc()
 *
 * In the case that the zbud page in which the allocation resides is under
 * reclaim, as indicated by the PG_reclaim flag being set, this function
 * only sets the first|last_chunks to 0.  The page is actually freed
 * once both buddies are evicted (see zbud_reclaim_page() below).
 */
static void zpmem_free(struct zpmem_pool *pool, unsigned long handle)
{
	struct zpmem_header *zhdr;
	int freechunks;

	spin_lock(&pool->lock);
	zhdr = handle_to_zpmem_header(handle);

	/* If first buddy, handle will be page aligned */
	if ((handle - ZHDR_SIZE_ALIGNED) & ~PAGE_MASK)
		zhdr->last_chunks = 0;
	else
		zhdr->first_chunks = 0;

	if (zhdr->under_reclaim) {
		/* zbud page is under reclaim, reclaim will free */
		spin_unlock(&pool->lock);
		return;
	}

	/* Remove from existing buddy list */
	list_del(&zhdr->buddy);

	if (zhdr->first_chunks == 0 && zhdr->last_chunks == 0) {
		/* zbud page is empty, free */
		list_del(&zhdr->lru);
		free_zpmem_page(pool, zhdr);
		pool->pages_nr--;
	} else {
		/* Add to unbuddied list */
		freechunks = num_free_chunks(zhdr);
		list_add(&zhdr->buddy, &pool->unbuddied[freechunks]);
	}

	spin_unlock(&pool->lock);
}

/**
 * zbud_reclaim_page() - evicts allocations from a pool page and frees it
 * @pool:	pool from which a page will attempt to be evicted
 * @retries:	number of pages on the LRU list for which eviction will
 *		be attempted before failing
 *
 * zbud reclaim is different from normal system reclaim in that the reclaim is
 * done from the bottom, up.  This is because only the bottom layer, zbud, has
 * information on how the allocations are organized within each zbud page. This
 * has the potential to create interesting locking situations between zbud and
 * the user, however.
 *
 * To avoid these, this is how zbud_reclaim_page() should be called:
 *
 * The user detects a page should be reclaimed and calls zbud_reclaim_page().
 * zbud_reclaim_page() will remove a zbud page from the pool LRU list and call
 * the user-defined eviction handler with the pool and handle as arguments.
 *
 * If the handle can not be evicted, the eviction handler should return
 * non-zero. zbud_reclaim_page() will add the zbud page back to the
 * appropriate list and try the next zbud page on the LRU up to
 * a user defined number of retries.
 *
 * If the handle is successfully evicted, the eviction handler should
 * return 0 _and_ should have called zbud_free() on the handle. zbud_free()
 * contains logic to delay freeing the page if the page is under reclaim,
 * as indicated by the setting of the PG_reclaim flag on the underlying page.
 *
 * If all buddies in the zbud page are successfully evicted, then the
 * zbud page can be freed.
 *
 * Returns: 0 if page is successfully freed, otherwise -EINVAL if there are
 * no pages to evict or an eviction handler is not registered, -EAGAIN if
 * the retry limit was hit.
 */
static int zpmem_reclaim_page(struct zpmem_pool *pool, unsigned int retries)
{
	int i, ret, freechunks;
	struct zpmem_header *zhdr;
	unsigned long first_handle = 0, last_handle = 0;

	spin_lock(&pool->lock);
	if (!pool->ops || !pool->ops->evict || list_empty(&pool->lru) ||
			retries == 0) {
		spin_unlock(&pool->lock);
		return -EINVAL;
	}
	for (i = 0; i < retries; i++) {
		zhdr = list_last_entry(&pool->lru, struct zpmem_header, lru);
		list_del(&zhdr->lru);
		list_del(&zhdr->buddy);
		/* Protect zbud page against free */
		zhdr->under_reclaim = true;
		/*
		 * We need encode the handles before unlocking, since we can
		 * race with free that will set (first|last)_chunks to 0
		 */
		first_handle = 0;
		last_handle = 0;
		if (zhdr->first_chunks)
			first_handle = encode_handle(zhdr, FIRST);
		if (zhdr->last_chunks)
			last_handle = encode_handle(zhdr, LAST);
		spin_unlock(&pool->lock);

		/* Issue the eviction callback(s) */
		if (first_handle) {
			ret = pool->ops->evict(pool, first_handle);
			if (ret)
				goto next;
		}
		if (last_handle) {
			ret = pool->ops->evict(pool, last_handle);
			if (ret)
				goto next;
		}
next:
		spin_lock(&pool->lock);
		zhdr->under_reclaim = false;
		if (zhdr->first_chunks == 0 && zhdr->last_chunks == 0) {
			/*
			 * Both buddies are now free, free the zbud page and
			 * return success.
			 */
			free_zpmem_page(pool, zhdr);
			pool->pages_nr--;
			spin_unlock(&pool->lock);
			return 0;
		} else if (zhdr->first_chunks == 0 ||
				zhdr->last_chunks == 0) {
			/* add to unbuddied list */
			freechunks = num_free_chunks(zhdr);
			list_add(&zhdr->buddy, &pool->unbuddied[freechunks]);
		} else {
			/* add to buddied list */
			list_add(&zhdr->buddy, &pool->buddied);
		}

		/* add to beginning of LRU */
		list_add(&zhdr->lru, &pool->lru);
	}
	spin_unlock(&pool->lock);
	return -EAGAIN;
}

/**
 * zbud_map() - maps the allocation associated with the given handle
 * @pool:	pool in which the allocation resides
 * @handle:	handle associated with the allocation to be mapped
 *
 * While trivial for zbud, the mapping functions for others allocators
 * implementing this allocation API could have more complex information encoded
 * in the handle and could create temporary mappings to make the data
 * accessible to the user.
 *
 * Returns: a pointer to the mapped allocation
 */
static void *zpmem_map(struct zpmem_pool *pool, unsigned long handle)
{
	return (void *)(handle);
}

/**
 * zbud_unmap() - maps the allocation associated with the given handle
 * @pool:	pool in which the allocation resides
 * @handle:	handle associated with the allocation to be unmapped
 */
static void zpmem_unmap(struct zpmem_pool *pool, unsigned long handle)
{
}

/**
 * zbud_get_pool_size() - gets the zbud pool size in pages
 * @pool:	pool whose size is being queried
 *
 * Returns: size in pages of the given pool.  The pool lock need not be
 * taken to access pages_nr.
 */
static u64 zpmem_get_pool_size(struct zpmem_pool *pool)
{
	return pool->pages_nr;
}

/*****************
 * zpool
 ****************/

static int zpmem_zpool_evict(struct zpmem_pool *pool, unsigned long handle)
{
	if (pool->zpool && pool->zpool_ops && pool->zpool_ops->evict)
		return pool->zpool_ops->evict(pool->zpool, handle);
	else
		return -ENOENT;
}

static const struct zpmem_ops zpmem_zpool_ops = {
	.evict =	zpmem_zpool_evict
};

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
	zpmem_destroy_pool(pool);
}

static int zpmem_zpool_malloc(void *pool, size_t size, gfp_t gfp,
			unsigned long *handle)
{
	return zpmem_alloc(pool, size, gfp, handle);
}
static void zpmem_zpool_free(void *pool, unsigned long handle)
{
	zpmem_free(pool, handle);
}

static int zpmem_zpool_shrink(void *pool, unsigned int pages,
			unsigned int *reclaimed)
{
	unsigned int total = 0;
	int ret = -EINVAL;

	while (total < pages) {
		ret = zpmem_reclaim_page(pool, 8);
		if (ret < 0)
			break;
		total++;
	}

	if (reclaimed)
		*reclaimed = total;

	return ret;
}

static void *zpmem_zpool_map(void *pool, unsigned long handle,
			enum zpool_mapmode mm)
{
	return zpmem_map(pool, handle);
}
static void zpmem_zpool_unmap(void *pool, unsigned long handle)
{
	zpmem_unmap(pool, handle);
}

static u64 zpmem_zpool_total_size(void *pool)
{
	return zpmem_get_pool_size(pool) * PAGE_SIZE;
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

MODULE_ALIAS("zpool-zpmem");

static struct zpmem_pool *pool;

static int __init init_zpmem(void)
{
	/* Make sure the zbud header will fit in one chunk */
	BUILD_BUG_ON(sizeof(struct zpmem_header) > ZHDR_SIZE_ALIGNED);


        {
            //test
            pool = zpmem_create_pool(GFP_KERNEL, &zpmem_zpool_ops );
        }
        
	pr_info("loaded\n");

	zpool_register_driver(&zpmem_zpool_driver);

	return 0;
}

static void __exit exit_zpmem(void)
{

        {
            //test
            zpmem_destroy_pool(pool);            
        }
	zpool_unregister_driver(&zpmem_zpool_driver);
	pr_info("unloaded\n");
}

module_init(init_zpmem);
module_exit(exit_zpmem);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Li Shaowen");
MODULE_DESCRIPTION("PMEM Buddy Allocator for Compressed Pages");
