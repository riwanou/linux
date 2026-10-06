// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "numa_repl: " fmt

#include <linux/numa_replication.h>
#include <linux/mempolicy.h>
#include <linux/mman.h>
#include <linux/seq_file.h>

#include <asm/pgalloc.h>
#include <asm/tlb.h>

#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/numa_replication.h>

struct repl_stat {
	unsigned long w, r, s, local, shared;
};

struct repl_stats {
	struct repl_stat node[REPL_MAX_NODES];
	unsigned long main_on[REPL_MAX_NODES], unique[REPL_MAX_NODES];
};

static struct mempolicy repl_preferred_policy[REPL_MAX_NODES];
static struct mempolicy repl_bind_policy[REPL_MAX_NODES];

enum { REPL_MAIN_BOUND, REPL_MAIN_FIRST_TOUCH, REPL_MAIN_INTERLEAVE };
static const char *const repl_main_names[] = { "bound", "first_touch",
					       "interleave" };
static int repl_main_placement = REPL_MAIN_BOUND;
static unsigned int repl_free_percent = 10;

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
	int main_nid;
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

	main_nid = numa_node_id();
	mm->repl->main_nid = main_nid;

	return mm->repl->pgds[main_nid];
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

	if (addr_is_replicated(mm, address)) {
		p4d_populate(mm, p4d, pud);
		return;
	}

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

	if (addr_is_replicated(mm, address)) {
		p4d_clear(p4d);
		return;
	}

	for_each_repl_pgd(mm, nid, repl_pgd) {
		repl_p4d =
			p4d_offset(pgd_offset_pgd(repl_pgd, address), address);
		p4d_clear(repl_p4d);
	}
}

void repl_free_p4d_range(struct mmu_gather *tlb, unsigned long addr,
			 unsigned long end, unsigned long floor,
			 unsigned long ceiling)
{
	int nid;
	pgd_t *repl_pgd;

	for_each_repl_pgd(tlb->mm, nid, repl_pgd)
		free_p4d_range(tlb, pgd_offset_pgd(repl_pgd, addr), addr, end,
			       floor, ceiling);
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

bool repl_mmap_eligible(struct mm_struct *mm, struct file *file,
			unsigned long flags)
{
	bool shared = flags & MAP_SHARED; /* also MAP_SHARED_VALIDATE */

	return repl_has_pgds(mm) && (flags & MAP_REPL) &&
	       !(flags & (MAP_FIXED | MAP_HUGETLB)) &&
	       (file ? shared : (flags & MAP_TYPE) == MAP_PRIVATE);
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

static inline bool repl_pte_rdprotected(pte_t pte)
{
	return pte_protnone(pte);
}

static inline bool repl_pte_uptodate(pte_t pte)
{
	return pte_present(pte) && !repl_pte_rdprotected(pte);
}

static inline bool repl_on_main_node(struct mm_struct *mm, pgd_t *pgd)
{
	return pgd == mm->pgd;
}

static bool repl_needs_cow(struct vm_area_struct *vma, unsigned long addr,
			   pte_t main)
{
	struct page *page = vm_normal_page(vma, addr, main);

	return !(vma->vm_flags & VM_SHARED) &&
	       (!page || !PageAnonExclusive(page));
}

static inline pte_t repl_pte_rdprotect(pte_t pte)
{
	return pte_modify(pte, PAGE_NONE);
}

static inline pte_t repl_pte_rdunprotect(pte_t pte, struct vm_area_struct *vma)
{
	return pte_modify(pte, vma->vm_page_prot);
}

static pmd_t *repl_pmd_find(struct mm_struct *mm, pgd_t *pgd,
			    unsigned long addr)
{
	p4d_t *p4d;
	pud_t *pud;

	p4d = p4d_offset(pgd_offset_pgd(pgd, addr), addr);
	if (p4d_none_or_clear_bad(p4d))
		return NULL;
	pud = pud_offset(p4d, addr);
	if (pud_none_or_clear_bad(pud))
		return NULL;
	return pmd_offset(pud, addr);
}

static pte_t *repl_pte_map(struct mm_struct *mm, pgd_t *pgd, unsigned long addr)
{
	pmd_t *pmd = repl_pmd_find(mm, pgd, addr);

	return pmd ? pte_offset_map(pmd, addr) : NULL;
}

static pte_t *repl_pte_map_lock(struct mm_struct *mm, pgd_t *pgd,
				unsigned long addr, spinlock_t **ptl)
{
	pmd_t *pmd = repl_pmd_find(mm, pgd, addr);

	return pmd ? pte_offset_map_lock(mm, pmd, addr, ptl) : NULL;
}

static pte_t repl_pte_read(struct mm_struct *mm, pgd_t *pgd, unsigned long addr)
{
	pte_t *ptep = repl_pte_map(mm, pgd, addr), pte = __pte(0);

	if (ptep) {
		pte = ptep_get_lockless(ptep);
		pte_unmap(ptep);
	}
	return pte;
}

static inline struct vm_fault repl_vmf(struct vm_area_struct *vma,
				       unsigned long addr, unsigned int flags)
{
	return (struct vm_fault){
		.vma = vma,
		.address = addr & PAGE_MASK,
		.real_address = addr,
		.flags = flags,
		.pgoff = linear_page_index(vma, addr),
		.gfp_mask = __get_fault_gfp_mask(vma),
	};
}

static void repl_copy(struct vm_area_struct *vma, unsigned long addr,
		      struct page *dst, struct page *src)
{
	copy_user_highpage(dst, src, addr, vma);
	if (!folio_test_anon(page_folio(dst)))
		folio_mark_dirty(page_folio(dst));
	trace_repl_copy(addr, page_to_pfn(src), page_to_pfn(dst));
}

static bool repl_node_has_room(int nid)
{
	unsigned long free = sum_zone_node_page_state(nid, NR_FREE_PAGES);

	return free >
	       node_present_pages(nid) * READ_ONCE(repl_free_percent) / 100;
}

#ifdef CONFIG_MEMCG
static bool repl_memcg_has_room(struct mm_struct *mm)
{
	struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
	unsigned long limit;
	bool room = true;

	if (!memcg)
		return true;
	if (mem_cgroup_is_root(memcg))
		goto out;
	limit = READ_ONCE(memcg->memory.high);
	if (limit == PAGE_COUNTER_MAX)
		limit = READ_ONCE(memcg->memory.max);
	if (limit != PAGE_COUNTER_MAX)
		room = page_counter_read(&memcg->memory) +
			       limit * READ_ONCE(repl_free_percent) / 100 <=
		       limit;
out:
	mem_cgroup_put(memcg);
	return room;
}
#else
static bool repl_memcg_has_room(struct mm_struct *mm)
{
	return true;
}
#endif

static bool repl_has_room(struct mm_struct *mm, int nid)
{
	return repl_node_has_room(nid) && repl_memcg_has_room(mm);
}

static struct folio *repl_prealloc_folio(struct vm_area_struct *vma, int nid)
{
	gfp_t gfp = (GFP_HIGHUSER_MOVABLE & ~__GFP_RECLAIM) | __GFP_THISNODE |
		    __GFP_NOWARN;
	struct folio *folio;

	if (!repl_has_room(vma->vm_mm, nid))
		return NULL;
	folio = __folio_alloc_node(gfp, 0, nid);
	if (folio && mem_cgroup_charge(folio, vma->vm_mm, gfp)) {
		folio_put(folio);
		return NULL;
	}
	return folio;
}

static pmd_t *repl_alloc_pgtables(struct mm_struct *mm, pgd_t *pgd,
				  unsigned long addr)
{
	p4d_t *p4d = p4d_alloc(mm, pgd_offset_pgd(pgd, addr), addr);
	pud_t *pud;
	pmd_t *pmd;

	if (!p4d)
		return NULL;
	pud = pud_alloc(mm, p4d, addr);
	if (!pud)
		return NULL;
	pmd = pmd_alloc(mm, pud, addr);
	if (!pmd || pte_alloc(mm, pmd))
		return NULL;
	return pmd;
}

static int repl_main_nid(struct vm_area_struct *vma, unsigned long addr)
{
	int nid = first_online_node, n;

	switch (READ_ONCE(repl_main_placement)) {
	case REPL_MAIN_FIRST_TOUCH:
		return numa_node_id();
	case REPL_MAIN_INTERLEAVE:
		for (n = linear_page_index(vma, addr) % num_online_nodes(); n;
		     n--)
			nid = next_online_node(nid);
		return nid;
	}
	return vma->vm_mm->repl->main_nid;
}

static vm_fault_t repl_install_main(struct vm_area_struct *vma,
				    unsigned long addr, unsigned int flags,
				    pmd_t *pmd, pte_t main)
{
	struct mm_struct *mm = vma->vm_mm;
	struct vm_fault vmf =
		repl_vmf(vma, addr, flags | FAULT_FLAG_ORIG_PTE_VALID);
	int nid = repl_main_nid(vma, addr);
	unsigned short mode = repl_has_room(mm, nid) ? MPOL_BIND :
						       MPOL_PREFERRED;
	struct mempolicy *old;
	pmd_t dummy;
	int ret = 0;

	vmf.pmd = pmd;
	vmf.orig_pte = main;

	if (pte_none(main)) {
		if (vma_is_anonymous(vma))
			vmf.flags |= FAULT_FLAG_WRITE; /* no zero page */
		old = repl_push_alloc_node(nid, mode);
		ret = do_pte_missing(&vmf);
		repl_pop_alloc_node(old);
		trace_repl_main_fill(mm->repl->main_nid, addr,
				     pte_pfn(repl_pte_read(mm, mm->pgd, addr)));
		return ret;
	}
	/* swap, migration: do_swap_page expects the pte mapped */
	vmf.pte = pte_offset_map_rw_nolock(vma->vm_mm, pmd, addr, &dummy,
					   &vmf.ptl);
	if (!vmf.pte)
		return 0;
	old = repl_push_alloc_node(nid, mode);
	ret = do_swap_page(&vmf);
	repl_pop_alloc_node(old);
	trace_repl_main_swapin(mm->repl->main_nid, addr,
			       pte_pfn(repl_pte_read(mm, mm->pgd, addr)));
	return ret;
}

static void repl_wrprotect_all(struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm = vma->vm_mm;
	bool flush = false;
	pgd_t *repl_pgd;
	pte_t *ptep;
	int nid;

	for_each_repl_pgd(mm, nid, repl_pgd) {
		ptep = repl_pte_map(mm, repl_pgd, addr);
		if (!ptep)
			continue;
		if (pte_write(ptep_get(ptep))) {
			ptep_set_wrprotect(mm, addr, ptep);
			flush = true;
		}
		pte_unmap(ptep);
	}
	if (flush)
		flush_tlb_page(vma, addr);
}

static void repl_install_replica(struct vm_area_struct *vma, unsigned long addr,
				 pte_t *mainp, pte_t *ptep,
				 struct folio **prealloc_folio)
{
	struct folio *folio = *prealloc_folio;
	pte_t main = ptep_get(mainp), pte;
	struct page *main_page = vm_normal_page(vma, addr, main);

	VM_WARN_ON_ONCE(ptep == mainp);

	/* no local copy possible or useful: share main's page */
	if (!folio || !main_page ||
	    page_to_nid(main_page) == folio_nid(folio)) {
		trace_repl_share(numa_node_id(), addr, pte_pfn(main));
		set_pte_at(vma->vm_mm, addr, ptep, main);
		return;
	}
	*prealloc_folio = NULL;

	/* stale main: skip the copy, repl_handle_rdprotect refreshes it next */
	if (!repl_pte_rdprotected(main)) {
		repl_wrprotect_all(vma, addr);
		repl_copy(vma, addr, folio_page(folio, 0), main_page);
	}
	__folio_mark_uptodate(folio);
	folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE);
	folio_clear_swapbacked(folio);
	folio_add_lru_vma(folio, vma);
	inc_mm_counter(vma->vm_mm, MM_ANONPAGES);

	pte = pte_wrprotect(folio_mk_pte(folio, vma->vm_page_prot));
	if (repl_pte_rdprotected(main))
		pte = repl_pte_rdprotect(pte);
	trace_repl_install(numa_node_id(), addr, folio_pfn(folio));
	set_pte_at(vma->vm_mm, addr, ptep, pte);
	update_mmu_cache(vma, addr, ptep);
}

static bool repl_folio_lock_dirty(struct folio *folio)
{
	if (!folio_trylock(folio))
		return false;
	if (folio_test_dirty(folio))
		return true;
	folio_unlock(folio);
	return false;
}

static vm_fault_t repl_page_mkwrite(struct vm_area_struct *vma,
				    unsigned long addr, unsigned int flags,
				    struct page *page)
{
	struct vm_fault vmf = repl_vmf(vma, addr, flags);
	struct folio *folio = page_folio(page);
	vm_fault_t ret;

	vmf.page = page;
	if (vma->vm_ops->page_mkwrite) {
		ret = vmf_can_call_fault(&vmf);
		if (ret)
			return ret;
		ret = do_page_mkwrite(&vmf, folio);
		if (!ret || (ret & (VM_FAULT_ERROR | VM_FAULT_NOPAGE)))
			return ret;
	} else {
		folio_lock(folio);
	}
	return fault_dirty_shared_page(&vmf);
}

static pte_t repl_find_uptodate(struct mm_struct *mm, pgd_t *pgd,
				unsigned long addr, unsigned long pfn)
{
	pgd_t *repl_pgd;
	pte_t pte, found = __pte(0);
	int nid;

	for_each_repl_pgd(mm, nid, repl_pgd) {
		if (repl_pgd == pgd)
			continue;
		pte = repl_pte_read(mm, repl_pgd, addr);
		if (!repl_pte_uptodate(pte))
			continue;
		if (pte_pfn(pte) == pfn)
			return pte;
		found = pte;
	}
	return found;
}

static void repl_handle_rdprotect(struct vm_area_struct *vma, pgd_t *pgd,
				  unsigned long addr, pte_t *ptep,
				  spinlock_t *ptl)
{
	pte_t pte = ptep_get(ptep), src;

	lockdep_assert_held(ptl);
	src = repl_find_uptodate(vma->vm_mm, pgd, addr, pte_pfn(pte));
	if (WARN_ON_ONCE(!pte_present(src)))
		goto unprotect; /* no up-to-date copy: keep what we have */
	if (pte_pfn(src) != pte_pfn(pte)) {
		repl_wrprotect_all(vma, addr);
		repl_copy(vma, addr, vm_normal_page(vma, addr, pte),
			  vm_normal_page(vma, addr, src));
	}
unprotect:
	set_pte_at(vma->vm_mm, addr, ptep,
		   pte_wrprotect(repl_pte_rdunprotect(pte, vma)));
	update_mmu_cache(vma, addr, ptep);
	trace_repl_refresh(numa_node_id(), addr, pte_pfn(pte));
}

static void repl_handle_write(struct vm_area_struct *vma, pgd_t *pgd,
			      unsigned long addr, pte_t *ptep, spinlock_t *ptl)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long pfn = pte_pfn(ptep_get(ptep));
	bool flush = false;
	pgd_t *repl_pgd;
	pte_t *other, pte;
	int nid;

	lockdep_assert_held(ptl);
	/* copies of another page go stale; ptes sharing my page stay valid */
	for_each_repl_pgd(mm, nid, repl_pgd) {
		if (repl_pgd == pgd)
			continue;
		other = repl_pte_map(mm, repl_pgd, addr);
		if (!other)
			continue;
		pte = ptep_get(other);
		if (repl_pte_uptodate(pte) && pte_pfn(pte) != pfn) {
			trace_repl_invalidate(nid, addr, pte_pfn(pte));
			pte = ptep_modify_prot_start(vma, addr, other);
			ptep_modify_prot_commit(vma, addr, other, pte,
						repl_pte_rdprotect(pte));
			flush = true;
		}
		pte_unmap(other);
	}
	if (flush)
		flush_tlb_page(vma, addr);

	/* only now: a stale TLB entry elsewhere could still write otherwise */
	pte = ptep_get(ptep);
	pte = pte_mkyoung(pte);
	pte = pte_mkdirty(pte);
	pte = maybe_mkwrite(pte, vma);
	trace_repl_write(numa_node_id(), addr, pfn);
	if (ptep_set_access_flags(vma, addr, ptep, pte, 1))
		update_mmu_cache(vma, addr, ptep);
}

bool repl_zap_replicas(struct mmu_gather *tlb, struct vm_area_struct *vma,
		       unsigned long addr, pte_t *mainp)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t main = ptep_get(mainp), *ptep, pte, src;
	struct folio *folio;
	bool full = false;
	struct page *page;
	pgd_t *repl_pgd;
	int nid;

	trace_repl_main_zap(tlb->mm->repl->main_nid, addr, pte_pfn(main));

	if ((vma->vm_flags & VM_SHARED) && repl_pte_rdprotected(main)) {
		src = repl_find_uptodate(mm, mm->pgd, addr, pte_pfn(main));
		if (!WARN_ON_ONCE(!pte_present(src)))
			repl_copy(vma, addr, vm_normal_page(vma, addr, main),
				  vm_normal_page(vma, addr, src));
	}

	for_each_repl_pgd(mm, nid, repl_pgd) {
		if (repl_pgd == mm->pgd)
			continue;

		ptep = repl_pte_map(mm, repl_pgd, addr);
		if (!ptep)
			continue;

		pte = ptep_get_and_clear(mm, addr, ptep);
		tlb_remove_tlb_entry(tlb, ptep, addr);
		pte_unmap(ptep);
		page = pte_present(pte) ? vm_normal_page(vma, addr, pte) : NULL;
		if (!page || pte_pfn(pte) == pte_pfn(main))
			continue; /* none, or main's page: main's zap frees it */

		folio = page_folio(page);
		folio_remove_rmap_pte(folio, page, vma);
		dec_mm_counter(mm, MM_ANONPAGES);
		trace_repl_free(nid, addr, folio_pfn(folio));
		if (!full) {
			full = __tlb_remove_folio_pages(tlb, page, 1, false);
			continue;
		}
		/* @tlb takes no more pages: flush this one ourselves */
		flush_tlb_page(vma, addr);
		folio_put(folio);
	}

	return full;
}

static vm_fault_t repl_handle_pte_fault(struct vm_area_struct *vma, pgd_t *pgd,
					unsigned long addr, unsigned int flags,
					struct folio **prealloc_folio)
{
	bool write = flags & FAULT_FLAG_WRITE;
	struct mm_struct *mm = vma->vm_mm;
	struct folio *wp_folio = NULL;
	pte_t *mainp, *ptep, main;
	struct page *page;
	spinlock_t *ptl;
	vm_fault_t ret = 0;

	mainp = repl_pte_map_lock(mm, mm->pgd, addr, &ptl);
	if (!mainp)
		return 0; /* freed meanwhile: refault */
	main = ptep_get(mainp);
	if (!pte_present(main))
		goto unlock; /* backed out or zapped meanwhile: refault */

	page = vm_normal_page(vma, addr, main);
	if (write && (vma->vm_flags & VM_SHARED) && page) {
		wp_folio = page_folio(page);
		if (!repl_folio_lock_dirty(wp_folio)) {
			VM_WARN_ON_ONCE(repl_pte_rdprotected(main));
			folio_get(wp_folio);
			pte_unmap_unlock(mainp, ptl);
			ret = repl_page_mkwrite(vma, addr, flags, page);
			folio_put(wp_folio);
			return ret;
		}
	}
	VM_WARN_ON_ONCE(write && repl_needs_cow(vma, addr, main));

	ptep = repl_on_main_node(mm, pgd) ? mainp : repl_pte_map(mm, pgd, addr);
	if (!ptep)
		goto unlock; /* freed meanwhile: refault */

	if (pte_none(ptep_get(ptep)))
		repl_install_replica(vma, addr, mainp, ptep, prealloc_folio);
	if (repl_pte_rdprotected(ptep_get(ptep)))
		repl_handle_rdprotect(vma, pgd, addr, ptep, ptl);
	if (write)
		repl_handle_write(vma, pgd, addr, ptep, ptl);

	if (ptep != mainp)
		pte_unmap(ptep);
unlock:
	if (wp_folio)
		folio_unlock(wp_folio);
	pte_unmap_unlock(mainp, ptl);
	return ret;
}

vm_fault_t repl_handle_mm_fault(struct vm_area_struct *vma, unsigned long addr,
				unsigned int flags)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd = repl_get_numa_pgd(mm);
	int nid = numa_node_id();
	struct vm_fault vmf = { .vma = vma, .flags = flags };
	struct folio *prealloc_folio = NULL;
	struct mempolicy *old;
	pmd_t *main_pmd, *pmd;
	pte_t main;
	vm_fault_t ret;

	main_pmd = repl_alloc_pgtables(mm, mm->pgd, addr);
	if (!main_pmd)
		return VM_FAULT_OOM;
	main = repl_pte_read(mm, mm->pgd, addr);
	if (!pte_present(main)) {
		ret = repl_install_main(vma, addr, flags, main_pmd, main);
		if (ret &
		    (VM_FAULT_ERROR | VM_FAULT_RETRY | VM_FAULT_COMPLETED))
			return ret;
	}

	if (!repl_on_main_node(mm, pgd)) {
		old = repl_push_alloc_node(nid, MPOL_PREFERRED);
		pmd = repl_alloc_pgtables(mm, pgd, addr);
		repl_pop_alloc_node(old);
		if (!pmd)
			return VM_FAULT_OOM;
		ret = vmf_anon_prepare(&vmf); /* replicas are anon folios */
		if (ret)
			return ret;
		prealloc_folio = repl_prealloc_folio(vma, nid);
	}

	ret = repl_handle_pte_fault(vma, pgd, addr & PAGE_MASK, flags,
				    &prealloc_folio);
	if (prealloc_folio)
		folio_put(prealloc_folio);
	return ret;
}

/* one pass per pmd: each node's pte table is mapped once, read in order */
static void repl_stat_range(struct mm_struct *mm, unsigned long addr,
			    unsigned long end, struct repl_stats *st)
{
	int nid, main_nid = mm->repl->main_nid;
	pte_t *ptes[REPL_MAX_NODES], pte, main;
	struct repl_stat *s;
	unsigned long next, i;
	pmd_t *pmd;
	pgd_t *pgd;

	for (; addr < end; addr = next) {
		next = pmd_addr_end(addr, end);
		for_each_repl_pgd(mm, nid, pgd) {
			pmd = repl_pmd_find(mm, pgd, addr);
			ptes[nid] = pmd ? pte_offset_map(pmd, addr) : NULL;
		}
		for (i = 0; i < (next - addr) >> PAGE_SHIFT; i++) {
			int sharers = 0;

			main = ptes[main_nid] ? ptep_get(ptes[main_nid] + i) :
						__pte(0);
			for_each_repl_pgd(mm, nid, pgd) {
				pte = ptes[nid] ? ptep_get(ptes[nid] + i) :
						  __pte(0);
				if (!pte_present(pte) ||
				    !pfn_valid(pte_pfn(pte)))
					continue;
				s = &st->node[nid];
				s->local += pfn_to_nid(pte_pfn(pte)) == nid;
				if (repl_pte_rdprotected(pte))
					s->s++;
				else if (pte_write(pte))
					s->w++;
				else
					s->r++;
				if (nid == main_nid) {
					st->main_on[pfn_to_nid(pte_pfn(pte))]++;
				} else if (pte_pfn(pte) == pte_pfn(main)) {
					s->shared++;
					sharers++;
				} else {
					st->unique[pfn_to_nid(pte_pfn(pte))]++;
				}
			}
			if (pte_present(main) && pfn_valid(pte_pfn(main)) &&
			    !sharers)
				st->unique[pfn_to_nid(pte_pfn(main))]++;
		}
		for_each_repl_pgd(mm, nid, pgd)
			if (ptes[nid])
				pte_unmap(ptes[nid]);
		cond_resched();
	}
}

static void repl_stat_print(struct seq_file *m, struct mm_struct *mm,
			    struct repl_stats *st)
{
	int nid, main_nid = mm->repl->main_nid;
	unsigned long mapped;
	struct repl_stat *s;
	char copies[16];
	pgd_t *pgd;

	seq_printf(m, "%-9s %7s %7s %7s %7s %7s %7s | %7s %7s\n", "node", "W",
		   "R", "S", "local", "remote", "copies", "main", "unique");
	for_each_repl_pgd(mm, nid, pgd) {
		s = &st->node[nid];
		mapped = s->w + s->r + s->s;
		snprintf(copies, sizeof(copies), "%lu", mapped - s->shared);
		seq_printf(m,
			   "n%d%-7s %7lu %7lu %7lu %7lu %7lu %7s | %7lu %7lu\n",
			   nid, nid == main_nid ? " (main)" : "", s->w, s->r,
			   s->s, s->local, mapped - s->local,
			   nid == main_nid ? "-" : copies, st->main_on[nid],
			   st->unique[nid]);
	}
}

void repl_stat_show(struct seq_file *m, struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	struct vma_iterator vmi;
	struct repl_stats *st;

	if (!(repl_has_pgds(mm) && mm->repl->window))
		return;
	st = kzalloc_obj(*st, GFP_KERNEL);
	if (!st)
		return;
	mmap_read_lock(mm);
	vma_iter_init(&vmi, mm, mm->repl->window);
	for_each_vma_range(vmi, vma, mm->repl->window + PGDIR_SIZE)
		if (vma->vm_ops != &repl_window_ops)
			repl_stat_range(mm, vma->vm_start, vma->vm_end, st);
	mmap_read_unlock(mm);
	repl_stat_print(m, mm, st);
	kfree(st);
}

static ssize_t main_placement_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	int i, len = 0;

	for (i = 0; i < ARRAY_SIZE(repl_main_names); i++)
		len += sysfs_emit_at(
			buf, len,
			i == READ_ONCE(repl_main_placement) ? "[%s] " : "%s ",
			repl_main_names[i]);
	buf[len - 1] = '\n';
	return len;
}

static ssize_t main_placement_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	int i = sysfs_match_string(repl_main_names, buf);

	if (i < 0)
		return i;
	WRITE_ONCE(repl_main_placement, i);
	return count;
}

static struct kobj_attribute main_placement_attr = __ATTR_RW(main_placement);
static struct attribute *repl_attrs[] = { &main_placement_attr.attr, NULL };
static const struct attribute_group repl_attr_group = {
	.name = "numa_replication",
	.attrs = repl_attrs,
};

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

	if (sysfs_create_group(mm_kobj, &repl_attr_group))
		pr_err("sysfs registration failed\n");

	return 0;
}
subsys_initcall(repl_init);
