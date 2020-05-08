#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/highmem.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/kallsyms.h>
#include <linux/sysfs.h>
#include <linux/device-mapper.h>
#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <linux/frontswap.h>
#include <linux/delay.h>

#define DEFAULT_QUARANTINE_TIME 360
#define PREFETCH_BUFFER_SIZE (1 << (34 - PAGE_SHIFT))
#define PREFETCH_GRACE_TIME 600

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Oliver Karaage");
MODULE_DESCRIPTION("tswap");
MODULE_VERSION("0.01");

/*
 * Data Structures
 */

struct tswap_entry {
	struct spinlock lock;
	int state;
	unsigned type;  /* swap type */
	unsigned long offset;  /* offset within a swap type */
	struct radix_tree_root *tswap_tree_root;  /* radix tree of the corresponding swap type;
                                               * valid after being inserted into the radix tree;
                                               * invalid after being removed from the radix tree
                                               */
	union {
		struct {
			unsigned long time_stamp;
			void *data;
			struct list_head list;  /* in quarantine list */
		};
		struct rcu_head rcu_head;
	};
};

enum tswap_entry_state {
	INVALID = 0,
	ZOMBIE = 1,
	IN_MEMORY = 2,
	IN_MEMORY_FOLLOWED_BY_ZOMBIE = 3,
	IN_FLIGHT = 4,
};

struct tswap_stat {
	atomic_long_t nr_zombie_page;
	atomic_long_t nr_in_memory_page;
	atomic_long_t nr_in_memory_zombie_page;
	atomic_long_t nr_in_flight_page;

	atomic_long_t len_quarantine_list;

	atomic_long_t nr_async_io;
	atomic_long_t nr_async_end_io;
	atomic_long_t nr_async_io_fail;
	atomic_long_t nr_async_end_io_fail;

	atomic_long_t nr_thp;
	atomic_long_t nr_malloc_fail;
	atomic_long_t nr_kmap_fail;
	atomic_long_t nr_radix_tree_insert_fail;
	atomic_long_t nr_radix_tree_delete_fail;

	atomic_long_t nr_store;
	atomic_long_t nr_load;
	atomic_long_t nr_invalid_load;
	atomic_long_t nr_overwrite_store;
	atomic_long_t nr_invalidate_page;
	atomic_long_t nr_invalid_invalidate_page;
	atomic_long_t nr_invalidate_area;
	atomic_long_t nr_init;

	atomic_long_t nr_quarantine_delete_stale;
	atomic_long_t nr_quarantine_skip_mem_zombie;

	atomic_long_t nr_promoted_page;  /* has exclusive sysfs file */
	atomic_long_t nr_disk_promoted_page;  /* has exclusive sysfs file */
} tswap_stat;

struct tswap_prefetch_info {
	int type;
	unsigned long offset;
};

/*
 * Global Variables
 */

static struct swap_info_struct *(*swap_info)[];

static struct radix_tree_root __rcu *tswap_tree_roots[MAX_SWAPFILES];
static struct spinlock tswap_tree_locks[MAX_SWAPFILES];

static atomic_t quarantine_time;

static struct list_head quarantine_list;
static struct spinlock quarantine_lock;

static struct task_struct *discharge_thread;

static struct kobject *kobject;

static struct spinlock tswap_prefetch_buffer_lock;
static struct tswap_prefetch_info tswap_prefetch_buffer[PREFETCH_BUFFER_SIZE];
static int tswap_prefetch_buffer_head;
static int tswap_prefetch_buffer_tail;

/*
 * Helper Functions
 */

/*
 * Since frontswap is a read-write workload,
 * we need to use kfree_rcu() instead of synchronize_rcu()
 * in general for performance concern
 */

static struct tswap_entry *atomic_entry_lookup_lock(unsigned type,
                                                    unsigned long offset,
                                                    unsigned long *flags)
{
	struct radix_tree_root *tswap_tree_root;
	struct tswap_entry *entry;

	rcu_read_lock();
	tswap_tree_root = rcu_dereference(tswap_tree_roots[type]);
	if (!tswap_tree_root) {
		rcu_read_unlock();
		return NULL;
	}
	entry = radix_tree_lookup(tswap_tree_root, offset);
	if (entry) {
		spin_lock_irqsave(&entry->lock, *flags);
		if (entry->state == INVALID) {
			/* this entry is going to be deleted, DO NOT USE IT */
			spin_unlock_irqrestore(&entry->lock, *flags);
			entry = NULL;
		}
	}
	rcu_read_unlock();
	/*
	 * locked non-invalid entry is guaranteed to be valid
	 * after rcu_read_unlock
	 */
	return entry;
}

/* tswap_tree_lock should be acquired *after* acquiring entry's lock */

static int atomic_entry_delete(struct tswap_entry *entry)
{
	/* entry lock must be hold */
	struct radix_tree_root *tswap_tree_root;
	struct spinlock *tswap_tree_lock;
	int ret;
	void *item;
	unsigned long flags;

	/*
	 * non-empty radix tree is guaranteed to exist,
	 * no need for rcu_read_lock
	 */
	tswap_tree_root = entry->tswap_tree_root;
	tswap_tree_lock = &tswap_tree_locks[entry->type];

	/*
	 * only delete the entry from the radix tree,
	 * will not free the entry
	 */
	spin_lock_irqsave(tswap_tree_lock, flags);
	item = radix_tree_delete(tswap_tree_root, entry->offset);
	spin_unlock_irqrestore(tswap_tree_lock, flags);
	if (item != NULL)
		entry->tswap_tree_root = NULL;

	ret = (item == NULL) ? -EINVAL : 0;
	return ret;
}

static int atomic_entry_insert(struct tswap_entry *entry)
{
	/* entry lock must be hold */
	struct radix_tree_root *tswap_tree_root;
	struct spinlock *tswap_tree_lock;
	int ret;
	unsigned long flags;

	rcu_read_lock();
	tswap_tree_root = rcu_dereference(tswap_tree_roots[entry->type]);
	if (!tswap_tree_root) {
		rcu_read_unlock();
		return -EINVAL;
	}
	tswap_tree_lock = &tswap_tree_locks[entry->type];

	spin_lock_irqsave(tswap_tree_lock, flags);
	ret = radix_tree_insert(tswap_tree_root, entry->offset, entry);
	spin_unlock_irqrestore(tswap_tree_lock, flags);
	entry->tswap_tree_root = (ret == 0) ? tswap_tree_root : NULL;

	rcu_read_unlock();

	return ret;
}

/*
 * quarantine_lock should be acquired *after* acquiring entry's lock
 * (in the case we need to acquire the lock in the reversed order, we
 *  may only try lock entry's lock)
 */

static void atomic_entry_quarantine(struct tswap_entry *entry)
{
	unsigned long flags;

	/* entry lock must be hold */
	spin_lock_irqsave(&quarantine_lock, flags);
	if (list_empty(&entry->list)) {
		list_add_tail(&entry->list, &quarantine_list);
		atomic_long_inc(&tswap_stat.len_quarantine_list);
	} else {
		list_del(&entry->list);
		list_add_tail(&entry->list, &quarantine_list);
	}
	spin_unlock_irqrestore(&quarantine_lock, flags);
}

static void atomic_entry_dequarantine(struct tswap_entry *entry)
{
	unsigned long flags;

	/* entry lock must be hold */
	spin_lock_irqsave(&quarantine_lock, flags);
	if (!list_empty(&entry->list)) {
		list_del_init(&entry->list);
		atomic_long_dec(&tswap_stat.len_quarantine_list);
	}
	spin_unlock_irqrestore(&quarantine_lock, flags);
}

static void discharge_async_io_end(struct bio *bio)
{
	struct tswap_entry *entry = bio->bi_private;
	unsigned long flags;
	int bio_err;
	int err;

	atomic_long_inc(&tswap_stat.nr_async_end_io);
	bio_err = bio->bi_error;
	if (bio_err < 0) {
		atomic_long_inc(&tswap_stat.nr_async_end_io_fail);
		pr_err("tswap: async io failed, ret: %d\n", bio_err);
	}

	spin_lock_irqsave(&entry->lock, flags);
	if (entry->state == IN_MEMORY_FOLLOWED_BY_ZOMBIE) {
		atomic_long_dec(&tswap_stat.nr_in_memory_zombie_page);
		atomic_long_inc(&tswap_stat.nr_in_memory_page);

		entry->state = IN_MEMORY;
		spin_unlock_irqrestore(&entry->lock, flags);
	} else if (entry->state == IN_FLIGHT) {
		if (bio_err < 0) {
			atomic_long_dec(&tswap_stat.nr_in_flight_page);
			atomic_long_inc(&tswap_stat.nr_in_memory_page);

			entry->state = IN_MEMORY;
			atomic_entry_quarantine(entry);
			spin_unlock_irqrestore(&entry->lock, flags);
		} else {
			atomic_long_dec(&tswap_stat.nr_in_flight_page);

			entry->state = INVALID;
			kfree(entry->data);
			/*
			 * don't need to dequarantine entry here, since IN_FLIGHT page
			 * will never appear in the quarantine list
			 */
			err = atomic_entry_delete(entry);
			if (err < 0) {
				pr_err("tswap: failed to delete tswap entry\n");
				atomic_long_inc(&tswap_stat.nr_radix_tree_delete_fail);
			}
			spin_unlock_irqrestore(&entry->lock, flags);
			kfree_rcu(entry, rcu_head);
		}
	} else if (entry->state == ZOMBIE) {
		atomic_long_dec(&tswap_stat.nr_zombie_page);

		entry->state = INVALID;
		kfree(entry->data);
		/*
		 * don't need to dequarantine entry here, since ZOMBIE page
		 * will never appear in the quarantine list
		 */
		err = atomic_entry_delete(entry);
		if (err < 0) {
			pr_err("tswap: failed to delete tswap entry\n");
			atomic_long_inc(&tswap_stat.nr_radix_tree_delete_fail);
		}
		spin_unlock_irqrestore(&entry->lock, flags);
		kfree_rcu(entry, rcu_head);
	} else {
		BUG();
	}

	bio_put(bio);
}

static int discharge_async_io(struct block_device *bdev, unsigned long offset,
                              struct page *page, int write, struct tswap_entry *entry) {
	struct bio *bio;
	int ret;

	atomic_long_inc(&tswap_stat.nr_async_io);

	bio = bio_alloc(GFP_NOIO, 1);
	if (!bio) {
		pr_err("tswap: failed to allocate bio in tswap\n");
		ret = -ENOMEM;
		atomic_long_inc(&tswap_stat.nr_malloc_fail);
		goto out;
	}

	bio->bi_bdev = bdev;
	bio_set_op_attrs(bio, write ? REQ_OP_WRITE : REQ_OP_READ, 0);

	bio->bi_iter.bi_sector = offset;
	bio->bi_iter.bi_sector <<= PAGE_SHIFT - SECTOR_SHIFT;
	bio_add_page(bio, page, PAGE_SIZE, 0);

	bio->bi_private = entry;
	bio->bi_end_io = discharge_async_io_end;

	submit_bio(bio);
	return 0;

out:
	return ret;
}

static int sync_io(struct block_device *bdev, unsigned long offset,
                   struct page *page, int write) {
	struct bio *bio;
	int ret;

	bio = bio_alloc(GFP_NOIO, 1);
	if (!bio) {
		pr_err("tswap: failed to allocate bio in tswap\n");
		ret = -ENOMEM;
		atomic_long_inc(&tswap_stat.nr_malloc_fail);
		goto out;
	}

	bio->bi_bdev = bdev;
	bio_set_op_attrs(bio, write ? REQ_OP_WRITE : REQ_OP_READ, 0);

	bio->bi_iter.bi_sector = offset;
	bio->bi_iter.bi_sector <<= PAGE_SHIFT - SECTOR_SHIFT;
	bio_add_page(bio, page, PAGE_SIZE, 0);

	ret = submit_bio_wait(bio);

out:
	return ret;
}

static void prefetch_async_io_end(struct bio *bio)
{
	struct tswap_entry *entry = bio->bi_private;
	unsigned long flags;
	int bio_err;
	int err;

	atomic_long_inc(&tswap_stat.nr_async_end_io);

	spin_lock_irqsave(&entry->lock, flags);
	bio_err = bio->bi_error;
	if (bio_err < 0) {
		atomic_long_inc(&tswap_stat.nr_async_end_io_fail);
		pr_err("tswap: prefetch async io failed, ret: %d\n", bio_err);

		goto invalidate_entry;
	}

	entry->time_stamp = jiffies + msecs_to_jiffies(PREFETCH_GRACE_TIME * 1000);
	err = atomic_entry_insert(entry);
	if (err < 0) {
		pr_err("tswap: failed to insert prefetched entry into radix tree, ret: %d\n", err);

		atomic_long_inc(&tswap_stat.nr_radix_tree_insert_fail);
		goto invalidate_entry;
	}
	atomic_entry_quarantine(entry);
	spin_unlock_irqrestore(&entry->lock, flags);

	bio_put(bio);
	return;

invalidate_entry:
	atomic_long_dec(&tswap_stat.nr_in_memory_page);
	entry->state = INVALID;
	kfree(entry->data);
	spin_unlock_irqrestore(&entry->lock, flags);
	kfree(entry);
	bio_put(bio);
}

static int prefetch_async_io(struct block_device *bdev, unsigned long offset,
                            struct page *page, int write, struct tswap_entry *entry) {
	struct bio *bio;
	int ret;

	atomic_long_inc(&tswap_stat.nr_async_io);

	bio = bio_alloc(GFP_NOIO, 1);
	if (!bio) {
		pr_err("tswap: failed to allocate bio in tswap\n");
		ret = -ENOMEM;
		atomic_long_inc(&tswap_stat.nr_malloc_fail);
		goto out;
	}

	bio->bi_bdev = bdev;
	bio_set_op_attrs(bio, write ? REQ_OP_WRITE : REQ_OP_READ, 0);

	bio->bi_iter.bi_sector = offset;
	bio->bi_iter.bi_sector <<= PAGE_SHIFT - SECTOR_SHIFT;
	bio_add_page(bio, page, PAGE_SIZE, 0);

	bio->bi_private = entry;
	bio->bi_end_io = prefetch_async_io_end;

	submit_bio(bio);
	return 0;

out:
	return ret;
}

static int invalidate_entry_struct(struct tswap_entry *entry, bool free_page)
{
	int old_state = entry->state;

	if (entry->state == IN_MEMORY) {
		atomic_long_dec(&tswap_stat.nr_in_memory_page);

		if (free_page)
			kfree(entry->data);
		entry->state = INVALID;
	} else if (entry->state == IN_MEMORY_FOLLOWED_BY_ZOMBIE) {
		atomic_long_dec(&tswap_stat.nr_in_memory_zombie_page);
		atomic_long_inc(&tswap_stat.nr_zombie_page);

		entry->state = ZOMBIE;
	} else if (entry->state == IN_FLIGHT) {
		atomic_long_dec(&tswap_stat.nr_in_flight_page);
		atomic_long_inc(&tswap_stat.nr_zombie_page);

		entry->state = ZOMBIE;
	} else if (entry->state == ZOMBIE) {
		/* do nothing */
	} else if (entry->state == INVALID){
		/* do nothing */
	}

	return old_state;
}

static int symbol_walk_callback(void *data, const char *name,
                                struct module *mod, unsigned long addr)
{
	/*
	 * skip the symbol if it belongs to a module rather
	 * than to the kernel
	 */
	if (mod != NULL)
		return 0;

	if (strcmp(name, "swap_info") == 0) {
		if (swap_info != NULL) {
			pr_err("tswap: found two swap_info, cannot continue\n");
			return -EFAULT;
		}
		swap_info = (typeof(swap_info))addr;
	}
	return 0;
}

/*
 * Frontswap Interface Functions
 */

static int tswap_frontswap_store(unsigned type, pgoff_t offset,
                                 struct page *page)
{
	struct tswap_entry *prev_entry, *entry;
	int old_state, ret, err;
	unsigned long flags;
	void *page_addr;

	atomic_long_inc(&tswap_stat.nr_store);

	spin_lock_irqsave(&tswap_prefetch_buffer_lock, flags);
	if ((tswap_prefetch_buffer_tail + 1) % PREFETCH_BUFFER_SIZE == tswap_prefetch_buffer_head)
		tswap_prefetch_buffer_head = (tswap_prefetch_buffer_head + 1) % PREFETCH_BUFFER_SIZE;
	tswap_prefetch_buffer[tswap_prefetch_buffer_tail].type = type;
	tswap_prefetch_buffer[tswap_prefetch_buffer_tail].offset = offset;
	tswap_prefetch_buffer_tail = (tswap_prefetch_buffer_tail + 1) % PREFETCH_BUFFER_SIZE;
	spin_unlock_irqrestore(&tswap_prefetch_buffer_lock, flags);

	if (PageTransHuge(page)) {
		/* should be careful about THP after 4.11 */
		atomic_long_inc(&tswap_stat.nr_thp);
		ret = -EINVAL;
		goto reject;
	}

	prev_entry = atomic_entry_lookup_lock(type, offset, &flags);

	if (!prev_entry) {
		entry = kmalloc(sizeof(struct tswap_entry), GFP_NOIO);
		if (!entry) {
			pr_err("tswap: cannot allocate memory for tswap entry\n");
			ret = -ENOMEM;
			atomic_long_inc(&tswap_stat.nr_malloc_fail);
			goto reject;
		}
		spin_lock_init(&entry->lock);
		entry->state = INVALID;
		entry->type = type;
		entry->offset = offset;
		INIT_LIST_HEAD(&entry->list);

		spin_lock_irqsave(&entry->lock, flags);
	} else {
		entry = prev_entry;
	}

	old_state = invalidate_entry_struct(entry, false);
	if (old_state == INVALID) {
		entry->data = kmalloc(PAGE_SIZE, GFP_NOIO);
		if (!entry->data) {
			pr_err("tswap: failed to allocate memory for tswap page\n");
			ret = -ENOMEM;
			atomic_long_inc(&tswap_stat.nr_malloc_fail);
			goto free_entry;
		}
	}
	if (old_state != entry->state) {
		/* in-disk overwrite is not tracked */
		atomic_long_inc(&tswap_stat.nr_overwrite_store);
	}

	page_addr = kmap_atomic(page);
	if (!page_addr) {
		pr_err("tswap: failed to map user's memory in tswap\n");
		ret = -ENOMEM;
		atomic_long_inc(&tswap_stat.nr_kmap_fail);
		goto free_mem;
	}
	memcpy(entry->data, page_addr, PAGE_SIZE);
	kunmap_atomic(page_addr);

	entry->time_stamp = jiffies;
	if (entry->state == INVALID) {
		atomic_long_inc(&tswap_stat.nr_in_memory_page);

		entry->state = IN_MEMORY;
	} else if (entry->state == ZOMBIE) {
		atomic_long_dec(&tswap_stat.nr_zombie_page);
		atomic_long_inc(&tswap_stat.nr_in_memory_zombie_page);

		entry->state = IN_MEMORY_FOLLOWED_BY_ZOMBIE;
	} else {
		BUG();
	}

	if (!prev_entry) {
		ret = atomic_entry_insert(entry);
		if (ret < 0) {
			pr_err("tswap: failed to insert entry into radix tree, ret: %d\n", ret);
			ret = -EINVAL;

			atomic_long_inc(&tswap_stat.nr_radix_tree_insert_fail);
			goto invalidate_entry;
		}
	}
	atomic_entry_quarantine(entry);
	spin_unlock_irqrestore(&entry->lock, flags);

	return 0;

invalidate_entry:
	invalidate_entry_struct(entry, false);
free_mem:
	if (entry->state == INVALID)
		kfree(entry->data);
free_entry:
	atomic_entry_dequarantine(entry);
	if (entry->state == INVALID) {
		if (prev_entry) {
			err = atomic_entry_delete(entry);
			if (err < 0) {
				pr_err("tswap: failed to delete tswap entry\n");
				atomic_long_inc(&tswap_stat.nr_radix_tree_delete_fail);
			}
		}
		spin_unlock_irqrestore(&entry->lock, flags);
		if (prev_entry) {
			kfree_rcu(entry, rcu_head);
		} else {
			kfree(entry);
		}
	} else {
		spin_unlock_irqrestore(&entry->lock, flags);
	}
reject:
	return ret;
}

static int tswap_frontswap_load(unsigned type, pgoff_t offset,
                                struct page *page)
{
	struct tswap_entry *entry;
	struct swap_info_struct *sis;
	struct gendisk *disk;
	int ret, err;
	unsigned long flags;
	void *page_addr;

	atomic_long_inc(&tswap_stat.nr_load);
	atomic_long_inc(&tswap_stat.nr_promoted_page);

	entry = atomic_entry_lookup_lock(type, offset, &flags);
	if (!entry) {
		/* in-disk page */
		atomic_long_inc(&tswap_stat.nr_invalid_load);
		atomic_long_inc(&tswap_stat.nr_disk_promoted_page);

		sis = (*swap_info)[type];
		ret = sync_io(sis->bdev, offset, page, 0);
		if (ret < 0) {
			pr_err("tswap: failed to read in-disk page\n");
			ret = -EINVAL;
			goto out;
		}
		disk = sis->bdev->bd_disk;
		if (disk->fops->swap_slot_free_notify) {
			disk->fops->swap_slot_free_notify(sis->bdev, offset);
		}
		ret = 0;
		goto out;
	}

	page_addr = kmap_atomic(page);
	if (!page_addr) {
		pr_err("tswap: failed to map user's memory in tswap\n");
		ret = -ENOMEM;
		atomic_long_inc(&tswap_stat.nr_kmap_fail);
		goto invalidate_entry;
	}
	memcpy(page_addr, entry->data, PAGE_SIZE);
	kunmap_atomic(page_addr);

	ret = 0;

invalidate_entry:
	invalidate_entry_struct(entry, true);
	atomic_entry_dequarantine(entry);
	if (entry->state == INVALID) {
		err = atomic_entry_delete(entry);
		if (err < 0) {
			pr_err("tswap: failed to delete tswap entry\n");
			atomic_long_inc(&tswap_stat.nr_radix_tree_delete_fail);
		}
		spin_unlock_irqrestore(&entry->lock, flags);
		kfree_rcu(entry, rcu_head);
	} else {
		spin_unlock_irqrestore(&entry->lock, flags);
	}
out:
	return ret;
}

static void tswap_frontswap_invalidate_page(unsigned type, pgoff_t offset)
{
	struct tswap_entry *entry;
	unsigned long flags;
	int err;

	atomic_long_inc(&tswap_stat.nr_invalidate_page);

	entry = atomic_entry_lookup_lock(type, offset, &flags);
	if (!entry) {
		/* could be in-disk page */
		atomic_long_inc(&tswap_stat.nr_invalid_invalidate_page);
	} else {
		invalidate_entry_struct(entry, true);
		atomic_entry_dequarantine(entry);
		if (entry->state == INVALID) {
			err = atomic_entry_delete(entry);
			if (err < 0) {
				pr_err("tswap: failed to delete tswap entry\n");
				atomic_long_inc(&tswap_stat.nr_radix_tree_delete_fail);
			}
			spin_unlock_irqrestore(&entry->lock, flags);
			kfree_rcu(entry, rcu_head);
		} else {
			spin_unlock_irqrestore(&entry->lock, flags);
		}
	}
}

static void tswap_frontswap_invalidate_area(unsigned type)
{
	struct radix_tree_root *tswap_tree_root;
	struct tswap_entry *entry;
	unsigned long flags;
	int err;

	atomic_long_inc(&tswap_stat.nr_invalidate_area);

	/*
	 * assume no concurrent invalidator and no invalid invalidation
	 * for a given swap type
	 */
	tswap_tree_root = tswap_tree_roots[type];
	BUG_ON(tswap_tree_root == NULL);
	rcu_assign_pointer(tswap_tree_roots[type], NULL);

	/*
	 * area invalidation is rather infrequent (e.g., when swap off),
	 * so we can use synchronize_rcu() here
	 */
	synchronize_rcu();

	/*
	 * should wait for all with-zombie pages to be terminated by themselves
	 * before reclaiming the radix tree
	 * (i.e., radix tree is guaranteed to exist when zombies exist)
	 */
	while (1) {
		/*
		 * might spin on zombie pages
		 */
		entry = NULL;
		rcu_read_lock();
		radix_tree_gang_lookup(tswap_tree_root, (void **)&entry, 0, 1);
		if (entry) {
			spin_lock_irqsave(&entry->lock, flags);
			if (entry->state == INVALID) {
				/* invalid page will never appear in the next lookup */
				spin_unlock_irqrestore(&entry->lock, flags);
				rcu_read_unlock();
				continue;
			}
		}
		rcu_read_unlock();

		if (!entry)
			break;

		invalidate_entry_struct(entry, true);
		atomic_entry_dequarantine(entry);
		if (entry->state == INVALID) {
			err = atomic_entry_delete(entry);
			if (err < 0) {
				pr_err("tswap: failed to delete tswap entry\n");
				atomic_long_inc(&tswap_stat.nr_radix_tree_delete_fail);
			}
			spin_unlock_irqrestore(&entry->lock, flags);
			kfree_rcu(entry, rcu_head);
		} else {
			/*
			 * should let zombie pages be freed by the discharge_async_io callback
			 */
			spin_unlock_irqrestore(&entry->lock, flags);
		}
	}

	kfree(tswap_tree_root);
}

static void tswap_frontswap_init(unsigned type)
{
	struct radix_tree_root *tswap_tree_root;

	atomic_long_inc(&tswap_stat.nr_init);

	/* does not consider re-init on a given swap type */
	tswap_tree_root = kmalloc(sizeof(struct radix_tree_root), GFP_KERNEL);
	if (!tswap_tree_root) {
		pr_err("tswap: failed to allocate memory for tswap radix tree root\n");
		return;
	}
	INIT_RADIX_TREE(tswap_tree_root, GFP_KERNEL);
	spin_lock_init(&tswap_tree_locks[type]);
	rcu_assign_pointer(tswap_tree_roots[type], tswap_tree_root);
}

static struct frontswap_ops tswap_frontswap_ops = {
		.store = tswap_frontswap_store,
		.load = tswap_frontswap_load,
		.invalidate_page = tswap_frontswap_invalidate_page,
		.invalidate_area = tswap_frontswap_invalidate_area,
		.init = tswap_frontswap_init
};

/*
 * Discharge Thread Functions
 */

static int tswap_discharge_threadfn(void *data)
{
	long nr_scan;
	struct tswap_entry *entry;
	struct swap_info_struct *sis;
	unsigned long cur_jiffies;
	unsigned long flags, outer_flags;
	int ret, err;

	while (!kthread_should_stop())
	{
		nr_scan = atomic_long_read(&tswap_stat.len_quarantine_list);

		while (nr_scan > 0) {
			spin_lock_irqsave(&quarantine_lock, outer_flags);
			if (!list_empty(&quarantine_list)) {
				entry = list_first_entry(&quarantine_list, struct tswap_entry, list);
				if (spin_trylock_irqsave(&entry->lock, flags)) {
					list_del_init(&entry->list);
					atomic_long_dec(&tswap_stat.len_quarantine_list);
				} else {
					spin_unlock_irqrestore(&quarantine_lock, outer_flags);
					continue;
					/* spin on this entry */
				}
			} else {
				spin_unlock_irqrestore(&quarantine_lock, outer_flags);
				break;
			}
			spin_unlock_irqrestore(&quarantine_lock, flags);
			flags = outer_flags;

			--nr_scan;

			if (entry->state == INVALID
			    || entry->state == ZOMBIE
			    || entry->state == IN_FLIGHT) {
				/*
				 * unlocked entry with those states should never appear
				 * in the quarantine list
				 */
				BUG();
			}
			if (entry->state == IN_MEMORY_FOLLOWED_BY_ZOMBIE) {
				atomic_entry_quarantine(entry);
				spin_unlock_irqrestore(&entry->lock, flags);

				atomic_long_inc(&tswap_stat.nr_quarantine_skip_mem_zombie);
				continue;
			}

			cur_jiffies = jiffies;
			if (time_after(cur_jiffies,
			               entry->time_stamp
			               + msecs_to_jiffies(atomic_read(&quarantine_time) * 1000))) {
				entry->state = IN_FLIGHT;
				atomic_long_dec(&tswap_stat.nr_in_memory_page);
				atomic_long_inc(&tswap_stat.nr_in_flight_page);
				spin_unlock_irqrestore(&entry->lock, flags);

				/* MUST release spinlock before entering "might sleep" region */
				sis = (*swap_info)[entry->type];
				ret = discharge_async_io(sis->bdev, entry->offset, virt_to_page(entry->data), 1, entry);
				if (ret < 0) {
					pr_err("tswap: failed to discharge tswap page to swap device\n");

					spin_lock_irqsave(&entry->lock, flags);
					if (entry->state == IN_MEMORY_FOLLOWED_BY_ZOMBIE) {
						atomic_long_dec(&tswap_stat.nr_in_memory_zombie_page);
						atomic_long_inc(&tswap_stat.nr_in_memory_page);

						entry->state = IN_MEMORY;
						spin_unlock_irqrestore(&entry->lock, flags);
					} else if (entry->state == IN_FLIGHT) {
						atomic_long_dec(&tswap_stat.nr_in_flight_page);
						atomic_long_inc(&tswap_stat.nr_in_memory_page);

						entry->state = IN_MEMORY;
						atomic_entry_quarantine(entry);
						spin_unlock_irqrestore(&entry->lock, flags);
					} else if (entry->state == ZOMBIE) {
						atomic_long_dec(&tswap_stat.nr_zombie_page);

						entry->state = INVALID;
						kfree(entry->data);
						/*
						 * don't need to dequarantine entry here, since ZOMBIE page
						 * will never appear in the quarantine list
						 */
						err = atomic_entry_delete(entry);
						if (err < 0) {
							pr_err("tswap: failed to delete tswap entry\n");
							atomic_long_inc(&tswap_stat.nr_radix_tree_delete_fail);
						}
						spin_unlock_irqrestore(&entry->lock, flags);
						kfree_rcu(entry, rcu_head);
					} else {
						BUG();
					}

					atomic_long_inc(&tswap_stat.nr_async_io_fail);
					continue;
				}
			} else {
				atomic_entry_quarantine(entry);
				spin_unlock_irqrestore(&entry->lock, flags);
			}
		}
		msleep((atomic_read(&quarantine_time) >> 1) * 1000);
	}
	return 0;
}

/*
 * Sysfs Functions
 */

static ssize_t tswap_stat_store(struct kobject *kobj,
                                struct kobj_attribute *attr, const char *buf,
                                size_t count)
{
	long input;

	sscanf(buf, "%ld", &input);
	if (input == 0) {
		atomic_long_set(&tswap_stat.nr_async_io, 0);
		atomic_long_set(&tswap_stat.nr_async_end_io, 0);
		atomic_long_set(&tswap_stat.nr_async_io_fail, 0);
		atomic_long_set(&tswap_stat.nr_async_end_io_fail, 0);

		atomic_long_set(&tswap_stat.nr_thp, 0);
		atomic_long_set(&tswap_stat.nr_malloc_fail, 0);
		atomic_long_set(&tswap_stat.nr_kmap_fail, 0);
		atomic_long_set(&tswap_stat.nr_radix_tree_insert_fail, 0);
		atomic_long_set(&tswap_stat.nr_radix_tree_delete_fail, 0);

		atomic_long_set(&tswap_stat.nr_store, 0);
		atomic_long_set(&tswap_stat.nr_load, 0);
		atomic_long_set(&tswap_stat.nr_invalid_load, 0);
		atomic_long_set(&tswap_stat.nr_overwrite_store, 0);

		atomic_long_set(&tswap_stat.nr_invalidate_page, 0);
		atomic_long_set(&tswap_stat.nr_invalid_invalidate_page, 0);
		atomic_long_set(&tswap_stat.nr_invalidate_area, 0);
		atomic_long_set(&tswap_stat.nr_init, 0);

		atomic_long_set(&tswap_stat.nr_quarantine_delete_stale, 0);
		atomic_long_set(&tswap_stat.nr_quarantine_skip_mem_zombie, 0);
	}
	return count;
}

static ssize_t tswap_stat_show(struct kobject *kobj,
                               struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf,
	               "nr_zombie_page:                 %ld\n"
	               "nr_in_memory_page:              %ld\n"
	               "nr_in_memory_zombie_page:       %ld\n"
	               "nr_in_flight_page:              %ld\n"
	               "len_quarantine_list:            %ld\n"
	               "nr_async_io:                    %ld\n"
	               "nr_async_end_io:                %ld\n"
	               "nr_async_io_fail:               %ld\n"
	               "nr_async_end_io_fail:           %ld\n"
	               "nr_thp:                         %ld\n"
	               "nr_malloc_fail:                 %ld\n"
	               "nr_kmap_fail:                   %ld\n"
	               "nr_radix_tree_insert_fail:      %ld\n"
	               "nr_radix_tree_delete_fail:      %ld\n"
	               "nr_store:                       %ld\n"
	               "nr_load:                        %ld\n"
	               "nr_invalid_load:                %ld\n"
	               "nr_overwrite_store:             %ld\n"
	               "nr_invalidate_page:             %ld\n"
	               "nr_invalid_invalidate_page:     %ld\n"
	               "nr_invalidate_area:             %ld\n"
	               "nr_init:                        %ld\n"
	               "nr_quarantine_delete_stale:     %ld\n"
	               "nr_quarantine_skip_mem_zombie:  %ld\n",
	               atomic_long_read(&tswap_stat.nr_zombie_page),
	               atomic_long_read(&tswap_stat.nr_in_memory_page),
	               atomic_long_read(&tswap_stat.nr_in_memory_zombie_page),
	               atomic_long_read(&tswap_stat.nr_in_flight_page),
	               atomic_long_read(&tswap_stat.len_quarantine_list),
	               atomic_long_read(&tswap_stat.nr_async_io),
	               atomic_long_read(&tswap_stat.nr_async_end_io),
	               atomic_long_read(&tswap_stat.nr_async_io_fail),
	               atomic_long_read(&tswap_stat.nr_async_end_io_fail),
	               atomic_long_read(&tswap_stat.nr_thp),
	               atomic_long_read(&tswap_stat.nr_malloc_fail),
	               atomic_long_read(&tswap_stat.nr_kmap_fail),
	               atomic_long_read(&tswap_stat.nr_radix_tree_insert_fail),
	               atomic_long_read(&tswap_stat.nr_radix_tree_delete_fail),
	               atomic_long_read(&tswap_stat.nr_store),
	               atomic_long_read(&tswap_stat.nr_load),
	               atomic_long_read(&tswap_stat.nr_invalid_load),
	               atomic_long_read(&tswap_stat.nr_overwrite_store),
	               atomic_long_read(&tswap_stat.nr_invalidate_page),
	               atomic_long_read(&tswap_stat.nr_invalid_invalidate_page),
	               atomic_long_read(&tswap_stat.nr_invalidate_area),
	               atomic_long_read(&tswap_stat.nr_init),
	               atomic_long_read(&tswap_stat.nr_quarantine_delete_stale),
	               atomic_long_read(&tswap_stat.nr_quarantine_skip_mem_zombie));
}

struct kobj_attribute tswap_stat_attribute = __ATTR_RW(tswap_stat);

static ssize_t tswap_quarantine_time_store(struct kobject *kobj,
                                           struct kobj_attribute *attr, const char *buf,
                                           size_t count)
{
	long input;

	sscanf(buf, "%ld", &input);
	if (input >= 0) {
		atomic_set(&quarantine_time, input);
	}
	return count;
}

static ssize_t tswap_quarantine_time_show(struct kobject *kobj,
                                          struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", atomic_read(&quarantine_time));
}

struct kobj_attribute tswap_quarantine_time_attribute = __ATTR_RW(tswap_quarantine_time);

static ssize_t tswap_nr_promoted_page_show(struct kobject *kobj,
                                          struct kobj_attribute *attr, char *buf)
{
	/* nr_promoted_page will be reset after each read operation */
	long nr_promoted_page = atomic_long_xchg(&tswap_stat.nr_promoted_page, 0);
	return sprintf(buf, "%ld\n", nr_promoted_page);
}

struct kobj_attribute tswap_nr_promoted_page_attribute = __ATTR_RO(tswap_nr_promoted_page);

static ssize_t tswap_nr_disk_promoted_page_show(struct kobject *kobj,
                                                struct kobj_attribute *attr, char *buf)
{
	/* nr_disk_promoted_page will be reset after each read operation */
	long nr_disk_promoted_page = atomic_long_xchg(&tswap_stat.nr_disk_promoted_page, 0);
	return sprintf(buf, "%ld\n", nr_disk_promoted_page);
}

struct kobj_attribute tswap_nr_disk_promoted_page_attribute = __ATTR_RO(tswap_nr_disk_promoted_page);

static ssize_t tswap_prefetch_store(struct kobject *kobj,
                                    struct kobj_attribute *attr, const char *buf,
                                    size_t count)
{
	long nr_pages;
	int type;
	unsigned long offset;
	struct swap_info_struct *sis;
	struct tswap_entry *entry;
	unsigned long flags;
	int ret;

	sscanf(buf, "%ld", &nr_pages);
	while (nr_pages > 0) {
		spin_lock_irqsave(&tswap_prefetch_buffer_lock, flags);
		if (tswap_prefetch_buffer_tail == tswap_prefetch_buffer_head) {
			spin_unlock_irqrestore(&tswap_prefetch_buffer_lock, flags);
			break;
		}
		tswap_prefetch_buffer_tail = (tswap_prefetch_buffer_tail + PREFETCH_BUFFER_SIZE - 1) % PREFETCH_BUFFER_SIZE;
		type = tswap_prefetch_buffer[tswap_prefetch_buffer_tail].type;
		offset = tswap_prefetch_buffer[tswap_prefetch_buffer_tail].offset;
		--nr_pages;
		spin_unlock_irqrestore(&tswap_prefetch_buffer_lock, flags);

		sis = (*swap_info)[type];
		if (!test_bit(offset, sis->frontswap_map))
			continue;

		entry = atomic_entry_lookup_lock(type, offset, &flags);
		if (entry) {
			spin_unlock_irqrestore(&entry->lock, flags);
			continue;
		}

		entry = kmalloc(sizeof(struct tswap_entry), GFP_NOIO);
		if (!entry) {
			pr_err("tswap: cannot allocate memory for prefetch tswap entry\n");
			atomic_long_inc(&tswap_stat.nr_malloc_fail);
			break;
		}

		entry->data = kmalloc(PAGE_SIZE, GFP_NOIO);
		if (!entry->data) {
			pr_err("tswap: failed to allocate memory for prefetch tswap page\n");
			atomic_long_inc(&tswap_stat.nr_malloc_fail);
			kfree(entry);
			break;
		}
		atomic_long_inc(&tswap_stat.nr_in_memory_page);
		spin_lock_init(&entry->lock);
		entry->state = IN_MEMORY;
		entry->type = type;
		entry->offset = offset;
		INIT_LIST_HEAD(&entry->list);

		ret = prefetch_async_io(sis->bdev, entry->offset, virt_to_page(entry->data), 0, entry);
		if (ret < 0) {
			pr_err("tswap: failed to send async io to prefetch page\n");
			atomic_long_dec(&tswap_stat.nr_in_memory_page);
			kfree(entry->data);
			kfree(entry);
		}
	}
	return count;
}

struct kobj_attribute tswap_prefetch_attribute = __ATTR_WO(tswap_prefetch);

/*
 * Module Functions
 */

static void init_stat(void)
{
	atomic_long_set(&tswap_stat.nr_zombie_page, 0);
	atomic_long_set(&tswap_stat.nr_in_memory_page, 0);
	atomic_long_set(&tswap_stat.nr_in_memory_zombie_page, 0);
	atomic_long_set(&tswap_stat.nr_in_flight_page, 0);

	atomic_long_set(&tswap_stat.len_quarantine_list, 0);

	atomic_long_set(&tswap_stat.nr_async_io, 0);
	atomic_long_set(&tswap_stat.nr_async_end_io, 0);
	atomic_long_set(&tswap_stat.nr_async_io_fail, 0);
	atomic_long_set(&tswap_stat.nr_async_end_io_fail, 0);

	atomic_long_set(&tswap_stat.nr_thp, 0);
	atomic_long_set(&tswap_stat.nr_malloc_fail, 0);
	atomic_long_set(&tswap_stat.nr_kmap_fail, 0);
	atomic_long_set(&tswap_stat.nr_radix_tree_insert_fail, 0);
	atomic_long_set(&tswap_stat.nr_radix_tree_delete_fail, 0);

	atomic_long_set(&tswap_stat.nr_store, 0);
	atomic_long_set(&tswap_stat.nr_load, 0);
	atomic_long_set(&tswap_stat.nr_invalid_load, 0);
	atomic_long_set(&tswap_stat.nr_overwrite_store, 0);

	atomic_long_set(&tswap_stat.nr_invalidate_page, 0);
	atomic_long_set(&tswap_stat.nr_invalid_invalidate_page, 0);
	atomic_long_set(&tswap_stat.nr_invalidate_area, 0);
	atomic_long_set(&tswap_stat.nr_init, 0);

	atomic_long_set(&tswap_stat.nr_quarantine_delete_stale, 0);
	atomic_long_set(&tswap_stat.nr_quarantine_skip_mem_zombie, 0);

	atomic_long_set(&tswap_stat.nr_promoted_page, 0);
	atomic_long_set(&tswap_stat.nr_disk_promoted_page, 0);
}

static int __init tswap_init(void)
{
	int i, ret, err;

	/*
	 * since we need to access swap_info which stores the pointers
	 * to the corresponding struct block_device, but swap_info is
	 * not exported to loadable modules. we can only look for its
	 * address via kallsyms subsystem and use this address then.
	 * this is an "ugly hack" and will definitely be frowned upon
	 * by kernel developers. hopefully there will be a graceful
	 * approach in the future.
	 *
	 * refer to https://github.com/euspectre/kernel-strider/blob/master/sources/core/module_ms_alloc.c
	 */
	swap_info = NULL;
	kallsyms_on_each_symbol(symbol_walk_callback, NULL);
	if (!swap_info) {
		pr_err("tswap: cannot find symbol 'swap_info'\n");
		ret = -EINVAL;
		goto err;
	}

	/* assume sizeof(unsigned long) == sizeof(pgoff_t) */
	BUG_ON(sizeof(unsigned long) != sizeof(pgoff_t));

	atomic_set(&quarantine_time, DEFAULT_QUARANTINE_TIME);

	spin_lock_init(&quarantine_lock);
	INIT_LIST_HEAD(&quarantine_list);

	init_stat();

	for (i = 0; i < MAX_SWAPFILES; ++i)
		tswap_tree_roots[i] = NULL;

	tswap_prefetch_buffer_head = 0;
	tswap_prefetch_buffer_tail = 0;
	spin_lock_init(&tswap_prefetch_buffer_lock);

	kobject = kobject_create_and_add("tswap", kernel_kobj);
	if (!kobject) {
		pr_err("tswap: fail to create kobject\n");
		ret = -ENOMEM;
		goto err;
	}
	err = sysfs_create_file(kobject, &tswap_stat_attribute.attr);
	if (err) {
		pr_err("tswap: fail to create sysfs file for stat\n");
		ret = -ENOMEM;
		goto free_kobject;
	}
	err = sysfs_create_file(kobject, &tswap_quarantine_time_attribute.attr);
	if (err) {
		pr_err("tswap: fail to create sysfs file for quarantine time\n");
		ret = -ENOMEM;
		goto free_kobject;
	}
	err = sysfs_create_file(kobject, &tswap_nr_promoted_page_attribute.attr);
	if (err) {
		pr_err("tswap: fail to create sysfs file for nr_promoted_page\n");
		ret = -ENOMEM;
		goto free_kobject;
	}
	err = sysfs_create_file(kobject, &tswap_nr_disk_promoted_page_attribute.attr);
	if (err) {
		pr_err("tswap: fail to create sysfs file for nr_disk_promoted_page\n");
		ret = -ENOMEM;
		goto free_kobject;
	}
	err = sysfs_create_file(kobject, &tswap_prefetch_attribute.attr);
	if (err) {
		pr_err("tswap: fail to create sysfs file for prefetch\n");
		ret = -ENOMEM;
		goto free_kobject;
	}

	discharge_thread = kthread_create(tswap_discharge_threadfn, NULL, "tswap_discharge");
	if (IS_ERR(discharge_thread)) {
		pr_err("tswap: fail to create discharge thread\n");
		ret = -EINVAL;
		goto free_kobject;
	}
	wake_up_process(discharge_thread);

	frontswap_tmem_exclusive_gets(true);
	frontswap_register_ops(&tswap_frontswap_ops);
	return 0;

free_kobject:
	kobject_put(kobject);
err:
	return ret;
}

/* frontswap module cannot unregister */

module_init(tswap_init)
