// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "numa_repl: " fmt

#include <linux/numa_replication.h>

#include <asm/pgalloc.h>
#include <linux/mempolicy.h>
#include <vma.h>

static struct mempolicy repl_preferred_policy[REPL_MAX_NODES];
static struct mempolicy repl_bind_policy[REPL_MAX_NODES];

static int __init repl_init(void)
{
	int nid;

	if (!repl_supported()) {
		pr_info("off (5-level paging, PTI or > %d nodes)\n",
			REPL_MAX_NODES);
		return 0;
	}

	for (nid = 0; nid < REPL_MAX_NODES; nid++) {
		repl_preferred_policy[nid] = (struct mempolicy){
			.refcnt = ATOMIC_INIT(1), /* static: never freed */
			.mode = MPOL_PREFERRED,
			.nodes = nodemask_of_node(nid),
			.home_node = NUMA_NO_NODE,
		};
		repl_bind_policy[nid] = repl_preferred_policy[nid];
		repl_bind_policy[nid].mode = MPOL_BIND;
	}

	return 0;
}
subsys_initcall(repl_init);

static struct mempolicy *repl_push_alloc_node(int nid, unsigned short mode)
{
	struct mempolicy *old;

	task_lock(current);
	old = current->mempolicy;
	current->mempolicy = mode == MPOL_BIND ? &repl_bind_policy[nid] :
						 &repl_preferred_policy[nid];
	task_unlock(current);
	return old;
}

static void repl_pop_alloc_node(struct mempolicy *old)
{
	task_lock(current);
	current->mempolicy = old;
	task_unlock(current);
}

void repl_mm_init(struct mm_struct *mm)
{
	mm->repl = NULL;
}

pgd_t *repl_pgds_alloc(struct mm_struct *mm)
{
	unsigned int nid;
	struct mempolicy *mempolicy;

	mm->repl = kzalloc_obj(struct mm_repl, GFP_KERNEL_ACCOUNT);
	if (!mm->repl)
		return NULL;

	for_each_online_node(nid) {
		mempolicy = repl_push_alloc_node(nid, MPOL_PREFERRED);
		mm->repl->pgds[nid] = pgd_alloc(mm);
		repl_pop_alloc_node(mempolicy);
		if (unlikely(!mm->repl->pgds[nid])) {
			repl_pgds_free(mm);
			return NULL;
		}
		node_set(nid, mm->repl->nodes);
	}

	return mm->repl->pgds[numa_node_id()];
}

void repl_pgds_free(struct mm_struct *mm)
{
	int i, nid;
	pgd_t *repl_pgd;

	for_each_repl_pgd(mm, nid, repl_pgd) {
		if (IS_ENABLED(CONFIG_DEBUG_VM))
			for (i = 0; i < KERNEL_PGD_BOUNDARY; i++)
				VM_WARN_ON_ONCE(pgd_val(repl_pgd[i]));
		pgd_free(mm, repl_pgd);
	}
	kfree(mm->repl);
	mm->repl = NULL;
}

void repl_p4ds_populate(struct mm_struct *mm, p4d_t *p4d, pud_t *pud,
			unsigned long address)
{
	int nid;
	pgd_t *repl_pgd;
	p4d_t *repl_p4d;

	for_each_repl_pgd(mm, nid, repl_pgd) {
		repl_p4d =
			p4d_offset(pgd_offset_pgd(repl_pgd, address), address);
		p4d_populate(mm, repl_p4d, pud);
	}
}

void repl_p4ds_clear(struct mm_struct *mm, p4d_t *p4d, unsigned long address)
{
	int nid;
	pgd_t *repl_pgd;
	p4d_t *repl_p4d;

	for_each_repl_pgd(mm, nid, repl_pgd) {
		repl_p4d =
			p4d_offset(pgd_offset_pgd(repl_pgd, address), address);
		p4d_clear(repl_p4d);
	}
}

static const char *repl_window_name(struct vm_area_struct *vma)
{
	return "[repl]";
}

static const struct vm_operations_struct repl_window_ops = {
	.name = repl_window_name,
};

static bool repl_placeholder_insert(struct mm_struct *mm, unsigned long start,
				    unsigned long end)
{
	struct vm_area_struct *vma = vm_area_alloc(mm);

	if (!vma)
		return false;
	vma->vm_start = start;
	vma->vm_end = end;
	vma->vm_ops = &repl_window_ops;
	vm_flags_init(vma, VM_DONTEXPAND | VM_DONTCOPY | VM_NORESERVE);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	if (insert_vm_struct(mm, vma)) {
		vm_area_free(vma);
		return false;
	}
	vm_stat_account(mm, vma->vm_flags, (end - start) >> PAGE_SHIFT);
	return true;
}

void repl_window_init(struct mm_struct *mm)
{
	struct vm_unmapped_area_info info = {
		.flags = VM_UNMAPPED_AREA_TOPDOWN,
		.length = PGDIR_SIZE,
		.low_limit = PAGE_SIZE,
		.high_limit = mm->mmap_base,
		.align_mask = ~PGDIR_MASK,
	};
	unsigned long addr;

	mmap_write_lock(mm);

	addr = vm_unmapped_area(&info);
	if (IS_ERR_VALUE(addr))
		goto out;

	if (!repl_placeholder_insert(mm, addr, addr + PGDIR_SIZE))
		goto out;
	mm->repl->window = addr;

out:
	mmap_write_unlock(mm);
}

void repl_window_refill(struct mm_struct *mm, unsigned long start,
			unsigned long end)
{
	unsigned long w = mm->repl->window;
	struct vm_area_struct *vma;

	VMA_ITERATOR(vmi, mm, 0);
	start = max(start, w);
	end = min(end, w + PGDIR_SIZE);
	if (!w || start >= end)
		return;

	vma = vma_lookup(mm, start - 1);
	if (vma && vma->vm_ops == &repl_window_ops)
		start = vma->vm_start;
	vma = vma_lookup(mm, end);
	if (vma && vma->vm_ops == &repl_window_ops)
		end = vma->vm_end;

	vma_iter_set(&vmi, start);
	WARN_ON_ONCE(do_vmi_munmap(&vmi, mm, start, end - start, NULL, false));
	WARN_ON_ONCE(!repl_placeholder_insert(mm, start, end));
}

unsigned long repl_get_window_area(struct mm_struct *mm, unsigned long len)
{
	unsigned long start = mm->repl->window;
	struct vm_area_struct *vma;

	VMA_ITERATOR(vmi, mm, start);
	if (!start)
		return 0;
	for_each_vma_range(vmi, vma, start + PGDIR_SIZE) {
		if (vma->vm_ops == &repl_window_ops &&
		    vma->vm_end - vma->vm_start >= len)
			return vma->vm_start;
	}
	return 0;
}
