// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/memblock.h>
#include <linux/stacktrace.h>
#include <linux/page_owner.h>
#include <linux/jump_label.h>
#include <linux/migrate.h>
#include <linux/stackdepot.h>
#include <linux/seq_file.h>
#include <linux/memcontrol.h>
#include <linux/sched/clock.h>

#include "internal.h"

struct page_owner {
	unsigned short order;
	short last_migrate_reason;
	gfp_t gfp_mask;
	depot_stack_handle_t handle;
	depot_stack_handle_t free_handle;
	u64 ts_nsec;
	u64 free_ts_nsec;
	char comm[TASK_COMM_LEN];
	pid_t pid;
	pid_t tgid;
};

static bool page_owner_enabled __initdata;
DEFINE_STATIC_KEY_FALSE(page_owner_inited);

static depot_stack_handle_t early_handle;

static void init_early_allocated_pages(void);

static int __init early_page_owner_param(char *buf)
{
	int ret = kstrtobool(buf, &page_owner_enabled);

	if (page_owner_enabled)
		stack_depot_want_early_init();

	return ret;
}
early_param("page_owner", early_page_owner_param);

static __init bool need_page_owner(void)
{
	return page_owner_enabled;
}

static __always_inline depot_stack_handle_t create_dummy_stack(void)
{
	unsigned long entries[4];
	unsigned int nr_entries;

	nr_entries = stack_trace_save(entries, ARRAY_SIZE(entries), 0);
	return stack_depot_save(entries, nr_entries, GFP_KERNEL);
}

static noinline void register_early_stack(void)
{
	early_handle = create_dummy_stack();
}

static __init void init_page_owner(void)
{
	if (!page_owner_enabled)
		return;

	stack_depot_capture_init();
	register_early_stack();
	static_branch_enable(&page_owner_inited);
	init_early_allocated_pages();
}

struct page_ext_operations page_owner_ops = {
	.size = sizeof(struct page_owner),
	.need = need_page_owner,
	.init = init_page_owner,
};

static inline struct page_owner *get_page_owner(struct page_ext *page_ext)
{
	return (void *)page_ext + page_owner_ops.offset;
}

void __reset_page_owner(struct page *page, unsigned short order)
{
	int i;
	struct page_ext *page_ext;
	depot_stack_handle_t handle;
	struct page_owner *page_owner;
	u64 free_ts_nsec = local_clock();

	page_ext = page_ext_get(page);
	if (unlikely(!page_ext))
		return;

	handle = stack_depot_capture_stack(GFP_NOWAIT | __GFP_NOWARN);
	for (i = 0; i < (1 << order); i++) {
		__clear_bit(PAGE_EXT_OWNER_ALLOCATED, &page_ext->flags);
		page_owner = get_page_owner(page_ext);
		page_owner->free_handle = handle;
		page_owner->free_ts_nsec = free_ts_nsec;
		page_ext = page_ext_next(page_ext);
	}
	page_ext_put(page_ext);
}

static inline void __set_page_owner_handle(struct page_ext *page_ext,
					depot_stack_handle_t handle,
					unsigned short order, gfp_t gfp_mask)
{
	struct page_owner *page_owner;
	int i;

	for (i = 0; i < (1 << order); i++) {
		page_owner = get_page_owner(page_ext);
		page_owner->handle = handle;
		page_owner->order = order;
		page_owner->gfp_mask = gfp_mask;
		page_owner->last_migrate_reason = -1;
		page_owner->pid = current->pid;
		page_owner->tgid = current->tgid;
		page_owner->ts_nsec = local_clock();
		strscpy(page_owner->comm, current->comm,
			sizeof(page_owner->comm));
		__set_bit(PAGE_EXT_OWNER, &page_ext->flags);
		__set_bit(PAGE_EXT_OWNER_ALLOCATED, &page_ext->flags);

		page_ext = page_ext_next(page_ext);
	}
}

noinline void __set_page_owner(struct page *page, unsigned short order,
					gfp_t gfp_mask)
{
	struct page_ext *page_ext;
	depot_stack_handle_t handle;

	handle = stack_depot_capture_stack(gfp_mask);

	page_ext = page_ext_get(page);
	if (unlikely(!page_ext))
		return;
	__set_page_owner_handle(page_ext, handle, order, gfp_mask);
	page_ext_put(page_ext);
}

void __set_page_owner_migrate_reason(struct page *page, int reason)
{
	struct page_ext *page_ext = page_ext_get(page);
	struct page_owner *page_owner;

	if (unlikely(!page_ext))
		return;

	page_owner = get_page_owner(page_ext);
	page_owner->last_migrate_reason = reason;
	page_ext_put(page_ext);
}

void __split_page_owner(struct page *page, unsigned int nr)
{
	int i;
	struct page_ext *page_ext = page_ext_get(page);
	struct page_owner *page_owner;

	if (unlikely(!page_ext))
		return;

	for (i = 0; i < nr; i++) {
		page_owner = get_page_owner(page_ext);
		page_owner->order = 0;
		page_ext = page_ext_next(page_ext);
	}
	page_ext_put(page_ext);
}

void __folio_copy_owner(struct folio *newfolio, struct folio *old)
{
	struct page_ext *old_ext;
	struct page_ext *new_ext;
	struct page_owner *old_page_owner, *new_page_owner;

	old_ext = page_ext_get(&old->page);
	if (unlikely(!old_ext))
		return;

	new_ext = page_ext_get(&newfolio->page);
	if (unlikely(!new_ext)) {
		page_ext_put(old_ext);
		return;
	}

	old_page_owner = get_page_owner(old_ext);
	new_page_owner = get_page_owner(new_ext);
	new_page_owner->order = old_page_owner->order;
	new_page_owner->gfp_mask = old_page_owner->gfp_mask;
	new_page_owner->last_migrate_reason =
		old_page_owner->last_migrate_reason;
	new_page_owner->handle = old_page_owner->handle;
	new_page_owner->pid = old_page_owner->pid;
	new_page_owner->tgid = old_page_owner->tgid;
	new_page_owner->ts_nsec = old_page_owner->ts_nsec;
	new_page_owner->free_ts_nsec = old_page_owner->ts_nsec;
	strcpy(new_page_owner->comm, old_page_owner->comm);

	/*
	 * We don't clear the bit on the old folio as it's going to be freed
	 * after migration. Until then, the info can be useful in case of
	 * a bug, and the overall stats will be off a bit only temporarily.
	 * Also, migrate_misplaced_transhuge_page() can still fail the
	 * migration and then we want the old folio to retain the info. But
	 * in that case we also don't need to explicitly clear the info from
	 * the new page, which will be freed.
	 */
	__set_bit(PAGE_EXT_OWNER, &new_ext->flags);
	__set_bit(PAGE_EXT_OWNER_ALLOCATED, &new_ext->flags);
	page_ext_put(new_ext);
	page_ext_put(old_ext);
}

void pagetypeinfo_showmixedcount_print(struct seq_file *m,
				       pg_data_t *pgdat, struct zone *zone)
{
	struct page *page;
	struct page_ext *page_ext;
	struct page_owner *page_owner;
	unsigned long pfn, block_end_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long count[MIGRATE_TYPES] = { 0, };
	int pageblock_mt, page_mt;
	int i;

	/* Scan block by block. First and last block may be incomplete */
	pfn = zone->zone_start_pfn;

	/*
	 * Walk the zone in pageblock_nr_pages steps. If a page block spans
	 * a zone boundary, it will be double counted between zones. This does
	 * not matter as the mixed block count will still be correct
	 */
	for (; pfn < end_pfn; ) {
		page = pfn_to_online_page(pfn);
		if (!page) {
			pfn = ALIGN(pfn + 1, MAX_ORDER_NR_PAGES);
			continue;
		}

		block_end_pfn = pageblock_end_pfn(pfn);
		block_end_pfn = min(block_end_pfn, end_pfn);

		pageblock_mt = get_pageblock_migratetype(page);

		for (; pfn < block_end_pfn; pfn++) {
			/* The pageblock is online, no need to recheck. */
			page = pfn_to_page(pfn);

			if (page_zone(page) != zone)
				continue;

			if (PageBuddy(page)) {
				unsigned long freepage_order;

				freepage_order = buddy_order_unsafe(page);
				if (freepage_order < MAX_ORDER)
					pfn += (1UL << freepage_order) - 1;
				continue;
			}

			if (PageReserved(page))
				continue;

			page_ext = page_ext_get(page);
			if (unlikely(!page_ext))
				continue;

			if (!test_bit(PAGE_EXT_OWNER_ALLOCATED, &page_ext->flags))
				goto ext_put_continue;

			page_owner = get_page_owner(page_ext);
			page_mt = gfp_migratetype(page_owner->gfp_mask);
			if (pageblock_mt != page_mt) {
				if (is_migrate_cma(pageblock_mt))
					count[MIGRATE_MOVABLE]++;
				else
					count[pageblock_mt]++;

				pfn = block_end_pfn;
				page_ext_put(page_ext);
				break;
			}
			pfn += (1UL << page_owner->order) - 1;
ext_put_continue:
			page_ext_put(page_ext);
		}
	}

	/* Print counts */
	seq_printf(m, "Node %d, zone %8s ", pgdat->node_id, zone->name);
	for (i = 0; i < MIGRATE_TYPES; i++)
		seq_printf(m, "%12lu ", count[i]);
	seq_putc(m, '\n');
}

/*
 * Looking for memcg information and print it out
 */
static inline int print_page_owner_memcg(char *kbuf, size_t count, int ret,
					 struct page *page)
{
#ifdef CONFIG_MEMCG
	unsigned long memcg_data;
	struct mem_cgroup *memcg;
	bool online;
	char name[80];

	rcu_read_lock();
	memcg_data = READ_ONCE(page->memcg_data);
	if (!memcg_data)
		goto out_unlock;

	if (memcg_data & MEMCG_DATA_OBJEXTS)
		ret += scnprintf(kbuf + ret, count - ret,
				"Slab cache page\n");

	memcg = page_memcg_check(page);
	if (!memcg)
		goto out_unlock;

	online = (memcg->css.flags & CSS_ONLINE);
	cgroup_name(memcg->css.cgroup, name, sizeof(name));
	ret += scnprintf(kbuf + ret, count - ret,
			"Charged %sto %smemcg %s\n",
			PageMemcgKmem(page) ? "(via objcg) " : "",
			online ? "" : "offline ",
			name);
out_unlock:
	rcu_read_unlock();
#endif /* CONFIG_MEMCG */

	return ret;
}

static ssize_t
print_page_owner(char __user *buf, size_t count, unsigned long pfn,
		struct page *page, struct page_owner *page_owner,
		depot_stack_handle_t handle)
{
	int ret, pageblock_mt, page_mt;
	char *kbuf;

	count = min_t(size_t, count, PAGE_SIZE);
	kbuf = kmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	ret = scnprintf(kbuf, count,
			"Page allocated via order %u, mask %#x(%pGg), pid %d, tgid %d (%s), ts %llu ns, free_ts %llu ns\n",
			page_owner->order, page_owner->gfp_mask,
			&page_owner->gfp_mask, page_owner->pid,
			page_owner->tgid, page_owner->comm,
			page_owner->ts_nsec, page_owner->free_ts_nsec);

	/* Print information relevant to grouping pages by mobility */
	pageblock_mt = get_pageblock_migratetype(page);
	page_mt  = gfp_migratetype(page_owner->gfp_mask);
	ret += scnprintf(kbuf + ret, count - ret,
			"PFN %lu type %s Block %lu type %s Flags %pGp\n",
			pfn,
			migratetype_names[page_mt],
			pfn >> pageblock_order,
			migratetype_names[pageblock_mt],
			&page->flags);

	ret += stack_depot_snprint(handle, kbuf + ret, count - ret, 0);
	if (ret >= count)
		goto err;

	if (page_owner->last_migrate_reason != -1) {
		ret += scnprintf(kbuf + ret, count - ret,
			"Page has been migrated, last migrate reason: %s\n",
			migrate_reason_names[page_owner->last_migrate_reason]);
	}

	ret = print_page_owner_memcg(kbuf, count, ret, page);

	ret += snprintf(kbuf + ret, count - ret, "\n");
	if (ret >= count)
		goto err;

	if (copy_to_user(buf, kbuf, ret))
		ret = -EFAULT;

	kfree(kbuf);
	return ret;

err:
	kfree(kbuf);
	return -ENOMEM;
}

void __dump_page_owner(const struct page *page)
{
	struct page_ext *page_ext = page_ext_get((void *)page);
	struct page_owner *page_owner;
	depot_stack_handle_t handle;
	gfp_t gfp_mask;
	int mt;

	if (unlikely(!page_ext)) {
		pr_alert("There is not page extension available.\n");
		return;
	}

	page_owner = get_page_owner(page_ext);
	gfp_mask = page_owner->gfp_mask;
	mt = gfp_migratetype(gfp_mask);

	if (!test_bit(PAGE_EXT_OWNER, &page_ext->flags)) {
		pr_alert("page_owner info is not present (never set?)\n");
		page_ext_put(page_ext);
		return;
	}

	if (test_bit(PAGE_EXT_OWNER_ALLOCATED, &page_ext->flags))
		pr_alert("page_owner tracks the page as allocated\n");
	else
		pr_alert("page_owner tracks the page as freed\n");

	pr_alert("page last allocated via order %u, migratetype %s, gfp_mask %#x(%pGg), pid %d, tgid %d (%s), ts %llu, free_ts %llu\n",
		 page_owner->order, migratetype_names[mt], gfp_mask, &gfp_mask,
		 page_owner->pid, page_owner->tgid, page_owner->comm,
		 page_owner->ts_nsec, page_owner->free_ts_nsec);

	handle = READ_ONCE(page_owner->handle);
	if (!handle)
		pr_alert("page_owner allocation stack trace missing\n");
	else
		stack_depot_print(handle);

	handle = READ_ONCE(page_owner->free_handle);
	if (!handle) {
		pr_alert("page_owner free stack trace missing\n");
	} else {
		pr_alert("page last free stack trace:\n");
		stack_depot_print(handle);
	}

	if (page_owner->last_migrate_reason != -1)
		pr_alert("page has been migrated, last migrate reason: %s\n",
			migrate_reason_names[page_owner->last_migrate_reason]);
	page_ext_put(page_ext);
}

static ssize_t
read_page_owner(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	unsigned long pfn;
	struct page *page;
	struct page_ext *page_ext;
	struct page_owner *page_owner;
	depot_stack_handle_t handle;

	if (!static_branch_unlikely(&page_owner_inited))
		return -EINVAL;

	page = NULL;
	if (*ppos == 0)
		pfn = min_low_pfn;
	else
		pfn = *ppos;
	/* Find a valid PFN or the start of a MAX_ORDER_NR_PAGES area */
	while (!pfn_valid(pfn) && (pfn & (MAX_ORDER_NR_PAGES - 1)) != 0)
		pfn++;

	/* Find an allocated page */
	for (; pfn < max_pfn; pfn++) {
		/*
		 * This temporary page_owner is required so
		 * that we can avoid the context switches while holding
		 * the rcu lock and copying the page owner information to
		 * user through copy_to_user() or GFP_KERNEL allocations.
		 */
		struct page_owner page_owner_tmp;

		/*
		 * If the new page is in a new MAX_ORDER_NR_PAGES area,
		 * validate the area as existing, skip it if not
		 */
		if ((pfn & (MAX_ORDER_NR_PAGES - 1)) == 0 && !pfn_valid(pfn)) {
			pfn += MAX_ORDER_NR_PAGES - 1;
			continue;
		}

		page = pfn_to_page(pfn);
		if (PageBuddy(page)) {
			unsigned long freepage_order = buddy_order_unsafe(page);

			if (freepage_order < MAX_ORDER)
				pfn += (1UL << freepage_order) - 1;
			continue;
		}

		page_ext = page_ext_get(page);
		if (unlikely(!page_ext))
			continue;

		/*
		 * Some pages could be missed by concurrent allocation or free,
		 * because we don't hold the zone lock.
		 */
		if (!test_bit(PAGE_EXT_OWNER, &page_ext->flags))
			goto ext_put_continue;

		/*
		 * Although we do have the info about past allocation of free
		 * pages, it's not relevant for current memory usage.
		 */
		if (!test_bit(PAGE_EXT_OWNER_ALLOCATED, &page_ext->flags))
			goto ext_put_continue;

		page_owner = get_page_owner(page_ext);

		/*
		 * Don't print "tail" pages of high-order allocations as that
		 * would inflate the stats.
		 */
		if (!IS_ALIGNED(pfn, 1 << page_owner->order))
			goto ext_put_continue;

		/*
		 * Access to page_ext->handle isn't synchronous so we should
		 * be careful to access it.
		 */
		handle = READ_ONCE(page_owner->handle);
		if (!handle)
			goto ext_put_continue;

		/* Record the next PFN to read in the file offset */
		*ppos = pfn + 1;

		page_owner_tmp = *page_owner;
		page_ext_put(page_ext);
		return print_page_owner(buf, count, pfn, page,
				&page_owner_tmp, handle);
ext_put_continue:
		page_ext_put(page_ext);
	}

	return 0;
}

static loff_t lseek_page_owner(struct file *file, loff_t offset, int orig)
{
	switch (orig) {
	case SEEK_SET:
		file->f_pos = offset;
		break;
	case SEEK_CUR:
		file->f_pos += offset;
		break;
	default:
		return -EINVAL;
	}
	return file->f_pos;
}

static void init_pages_in_zone(pg_data_t *pgdat, struct zone *zone)
{
	unsigned long pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long count = 0;

	/*
	 * Walk the zone in pageblock_nr_pages steps. If a page block spans
	 * a zone boundary, it will be double counted between zones. This does
	 * not matter as the mixed block count will still be correct
	 */
	for (; pfn < end_pfn; ) {
		unsigned long block_end_pfn;

		if (!pfn_valid(pfn)) {
			pfn = ALIGN(pfn + 1, MAX_ORDER_NR_PAGES);
			continue;
		}

		block_end_pfn = pageblock_end_pfn(pfn);
		block_end_pfn = min(block_end_pfn, end_pfn);

		for (; pfn < block_end_pfn; pfn++) {
			struct page *page = pfn_to_page(pfn);
			struct page_ext *page_ext;

			if (page_zone(page) != zone)
				continue;

			/*
			 * To avoid having to grab zone->lock, be a little
			 * careful when reading buddy page order. The only
			 * danger is that we skip too much and potentially miss
			 * some early allocated pages, which is better than
			 * heavy lock contention.
			 */
			if (PageBuddy(page)) {
				unsigned long order = buddy_order_unsafe(page);

				if (order > 0 && order < MAX_ORDER)
					pfn += (1UL << order) - 1;
				continue;
			}

			if (PageReserved(page))
				continue;

			page_ext = page_ext_get(page);
			if (unlikely(!page_ext))
				continue;

			/* Maybe overlapping zone */
			if (test_bit(PAGE_EXT_OWNER, &page_ext->flags))
				goto ext_put_continue;

			/* Found early allocated page */
			__set_page_owner_handle(page_ext, early_handle,
						0, 0);
			count++;
ext_put_continue:
			page_ext_put(page_ext);
		}
		cond_resched();
	}

	pr_info("Node %d, zone %8s: page owner found early allocated %lu pages\n",
		pgdat->node_id, zone->name, count);
}

static void init_zones_in_node(pg_data_t *pgdat)
{
	struct zone *zone;
	struct zone *node_zones = pgdat->node_zones;

	for (zone = node_zones; zone - node_zones < MAX_NR_ZONES; ++zone) {
		if (!populated_zone(zone))
			continue;

		init_pages_in_zone(pgdat, zone);
	}
}

static void init_early_allocated_pages(void)
{
	pg_data_t *pgdat;

	for_each_online_pgdat(pgdat)
		init_zones_in_node(pgdat);
}

static const struct file_operations proc_page_owner_operations = {
	.read		= read_page_owner,
	.llseek		= lseek_page_owner,
};

static int __init pageowner_init(void)
{
	if (!static_branch_unlikely(&page_owner_inited)) {
		pr_info("page_owner is disabled\n");
		return 0;
	}

	debugfs_create_file("page_owner", 0400, NULL, NULL,
			    &proc_page_owner_operations);

	return 0;
}
late_initcall(pageowner_init)
