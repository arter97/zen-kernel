// SPDX-License-Identifier: GPL-2.0
#include <linux/pagewalk.h>
#include <linux/highmem.h>
#include <linux/sched.h>
#include <linux/hugetlb.h>

/*
 * We want to know the real level where a entry is located ignoring any
 * folding of levels which may be happening. For example if p4d is folded then
 * a missing entry found at level 1 (p4d) is actually at level 0 (pgd).
 */
static int real_depth(int depth)
{
	if (depth == 3 && PTRS_PER_PMD == 1)
		depth = 2;
	if (depth == 2 && PTRS_PER_PUD == 1)
		depth = 1;
	if (depth == 1 && PTRS_PER_P4D == 1)
		depth = 0;
	return depth;
}

static int walk_pte_range_inner(pte_t *pte, unsigned long addr,
				unsigned long end, struct mm_walk *walk)
{
	const struct mm_walk_ops *ops = walk->ops;
	int err = 0;

	for (;;) {
		err = ops->pte_entry(pte, addr, addr + PAGE_SIZE, walk);
		if (err)
		       break;
		if (addr >= end - PAGE_SIZE)
			break;
		addr += PAGE_SIZE;
		pte++;
	}
	return err;
}

static int walk_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pte_t *pte;
	int err = 0;
	spinlock_t *ptl;

	if (walk->no_vma) {
		pte = pte_offset_map(pmd, addr);
		err = walk_pte_range_inner(pte, addr, end, walk);
		pte_unmap(pte);
	} else {
		pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
		err = walk_pte_range_inner(pte, addr, end, walk);
		pte_unmap_unlock(pte, ptl);
	}

	return err;
}

static int walk_pmd_range(pud_t *pud, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pmd_t *pmd;
	unsigned long next;
	const struct mm_walk_ops *ops = walk->ops;
	int err = 0;
	int depth = real_depth(3);

	pmd = pmd_offset(pud, addr);
	do {
again:
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd)) {
			if (ops->pte_hole)
				err = ops->pte_hole(addr, next, depth, walk);
			if (err)
				break;
			continue;
		}

		walk->action = ACTION_SUBTREE;

		/*
		 * This implies that each ->pmd_entry() handler
		 * needs to know about pmd_trans_huge() pmds
		 */
		if (ops->pmd_entry)
			err = ops->pmd_entry(pmd, addr, next, walk);
		if (err)
			break;

		if (walk->action == ACTION_AGAIN)
			goto again;

		/*
		 * Check this here so we only break down trans_huge
		 * pages when we _need_ to
		 */
		if ((!walk->vma && (pmd_leaf(*pmd) || !pmd_present(*pmd))) ||
		    walk->action == ACTION_CONTINUE ||
		    !(ops->pte_entry))
			continue;

		if (walk->vma) {
			split_huge_pmd(walk->vma, pmd, addr);
			if (pmd_trans_unstable(pmd))
				goto again;
		}

		err = walk_pte_range(pmd, addr, next, walk);
		if (err)
			break;
	} while (pmd++, addr = next, addr != end);

	return err;
}

static int walk_pud_range(p4d_t *p4d, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pud_t *pud;
	unsigned long next;
	const struct mm_walk_ops *ops = walk->ops;
	int err = 0;
	int depth = real_depth(2);

	pud = pud_offset(p4d, addr);
	do {
 again:
		next = pud_addr_end(addr, end);
		if (pud_none(*pud)) {
			if (ops->pte_hole)
				err = ops->pte_hole(addr, next, depth, walk);
			if (err)
				break;
			continue;
		}

		walk->action = ACTION_SUBTREE;

		if (ops->pud_entry)
			err = ops->pud_entry(pud, addr, next, walk);
		if (err)
			break;

		if (walk->action == ACTION_AGAIN)
			goto again;

		if ((!walk->vma && (pud_leaf(*pud) || !pud_present(*pud))) ||
		    walk->action == ACTION_CONTINUE ||
		    !(ops->pmd_entry || ops->pte_entry))
			continue;

		if (walk->vma)
			split_huge_pud(walk->vma, pud, addr);
		if (pud_none(*pud))
			goto again;

		err = walk_pmd_range(pud, addr, next, walk);
		if (err)
			break;
	} while (pud++, addr = next, addr != end);

	return err;
}

static int walk_p4d_range(pgd_t *pgd, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	p4d_t *p4d;
	unsigned long next;
	const struct mm_walk_ops *ops = walk->ops;
	int err = 0;
	int depth = real_depth(1);

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(p4d)) {
			if (ops->pte_hole)
				err = ops->pte_hole(addr, next, depth, walk);
			if (err)
				break;
			continue;
		}
		if (ops->p4d_entry) {
			err = ops->p4d_entry(p4d, addr, next, walk);
			if (err)
				break;
		}
		if (ops->pud_entry || ops->pmd_entry || ops->pte_entry)
			err = walk_pud_range(p4d, addr, next, walk);
		if (err)
			break;
	} while (p4d++, addr = next, addr != end);

	return err;
}

static int walk_pgd_range(unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pgd_t *pgd;
	unsigned long next;
	const struct mm_walk_ops *ops = walk->ops;
	int err = 0;

	if (walk->pgd)
		pgd = walk->pgd + pgd_index(addr);
	else
		pgd = pgd_offset(walk->mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd)) {
			if (ops->pte_hole)
				err = ops->pte_hole(addr, next, 0, walk);
			if (err)
				break;
			continue;
		}
		if (ops->pgd_entry) {
			err = ops->pgd_entry(pgd, addr, next, walk);
			if (err)
				break;
		}
		if (ops->p4d_entry || ops->pud_entry || ops->pmd_entry ||
		    ops->pte_entry)
			err = walk_p4d_range(pgd, addr, next, walk);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);

	return err;
}

#ifdef CONFIG_HUGETLB_PAGE
static unsigned long hugetlb_entry_end(struct hstate *h, unsigned long addr,
				       unsigned long end)
{
	unsigned long boundary = (addr & huge_page_mask(h)) + huge_page_size(h);
	return boundary < end ? boundary : end;
}

static int walk_hugetlb_range(unsigned long addr, unsigned long end,
			      struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;
	struct hstate *h = hstate_vma(vma);
	unsigned long next;
	unsigned long hmask = huge_page_mask(h);
	unsigned long sz = huge_page_size(h);
	pte_t *pte;
	const struct mm_walk_ops *ops = walk->ops;
	int err = 0;

	do {
		next = hugetlb_entry_end(h, addr, end);
		pte = huge_pte_offset(walk->mm, addr & hmask, sz);

		if (pte)
			err = ops->hugetlb_entry(pte, hmask, addr, next, walk);
		else if (ops->pte_hole)
			err = ops->pte_hole(addr, next, -1, walk);

		if (err)
			break;
	} while (addr = next, addr != end);

	return err;
}

#else /* CONFIG_HUGETLB_PAGE */
static int walk_hugetlb_range(unsigned long addr, unsigned long end,
			      struct mm_walk *walk)
{
	return 0;
}

#endif /* CONFIG_HUGETLB_PAGE */

/*
 * Decide whether we really walk over the current vma on [@start, @end)
 * or skip it via the returned value. Return 0 if we do walk over the
 * current vma, and return 1 if we skip the vma. Negative values means
 * error, where we abort the current walk.
 */
static int walk_page_test(unsigned long start, unsigned long end,
			struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;
	const struct mm_walk_ops *ops = walk->ops;

	if (ops->test_walk)
		return ops->test_walk(start, end, walk);

	/*
	 * vma(VM_PFNMAP) doesn't have any valid struct pages behind VM_PFNMAP
	 * range, so we don't walk over it as we do for normal vmas. However,
	 * Some callers are interested in handling hole range and they don't
	 * want to just ignore any single address range. Such users certainly
	 * define their ->pte_hole() callbacks, so let's delegate them to handle
	 * vma(VM_PFNMAP).
	 */
	if (vma->vm_flags & VM_PFNMAP) {
		int err = 1;
		if (ops->pte_hole)
			err = ops->pte_hole(start, end, -1, walk);
		return err ? err : 1;
	}
	return 0;
}

static int __walk_page_range(unsigned long start, unsigned long end,
			struct mm_walk *walk)
{
	int err = 0;
	struct vm_area_struct *vma = walk->vma;

	if (is_vm_hugetlb_page(vma)) {
		if (walk->ops->hugetlb_entry)
			err = walk_hugetlb_range(start, end, walk);
	} else
		err = walk_pgd_range(start, end, walk);

	return err;
}

/**
 * walk_page_range - walk page table with caller specific callbacks
 * @mm:		mm_struct representing the target process of page table walk
 * @start:	start address of the virtual address range
 * @end:	end address of the virtual address range
 * @ops:	operation to call during the walk
 * @private:	private data for callbacks' usage
 *
 * Recursively walk the page table tree of the process represented by @mm
 * within the virtual address range [@start, @end). During walking, we can do
 * some caller-specific works for each entry, by setting up pmd_entry(),
 * pte_entry(), and/or hugetlb_entry(). If you don't set up for some of these
 * callbacks, the associated entries/pages are just ignored.
 * The return values of these callbacks are commonly defined like below:
 *
 *  - 0  : succeeded to handle the current entry, and if you don't reach the
 *         end address yet, continue to walk.
 *  - >0 : succeeded to handle the current entry, and return to the caller
 *         with caller specific value.
 *  - <0 : failed to handle the current entry, and return to the caller
 *         with error code.
 *
 * Before starting to walk page table, some callers want to check whether
 * they really want to walk over the current vma, typically by checking
 * its vm_flags. walk_page_test() and @ops->test_walk() are used for this
 * purpose.
 *
 * struct mm_walk keeps current values of some common data like vma and pmd,
 * which are useful for the access from callbacks. If you want to pass some
 * caller-specific data to callbacks, @private should be helpful.
 *
 * Locking:
 *   Callers of walk_page_range() and walk_page_vma() should hold @mm->mmap_sem,
 *   because these function traverse vma list and/or access to vma's data.
 */
int walk_page_range(struct mm_struct *mm, unsigned long start,
		unsigned long end, const struct mm_walk_ops *ops,
		void *private)
{
	int err = 0;
	unsigned long next;
	struct vm_area_struct *vma;
	struct mm_walk walk = {
		.ops		= ops,
		.mm		= mm,
		.private	= private,
	};

	if (start >= end)
		return -EINVAL;

	if (!walk.mm)
		return -EINVAL;

	lockdep_assert_held(&walk.mm->mmap_sem);

	vma = find_vma(walk.mm, start);
	do {
		if (!vma) { /* after the last vma */
			walk.vma = NULL;
			next = end;
			if (ops->pte_hole)
				err = ops->pte_hole(start, next, -1, &walk);
		} else if (start < vma->vm_start) { /* outside vma */
			walk.vma = NULL;
			next = min(end, vma->vm_start);
			if (ops->pte_hole)
				err = ops->pte_hole(start, next, -1, &walk);
		} else { /* inside vma */
			walk.vma = vma;
			next = min(end, vma->vm_end);
			vma = vma->vm_next;

			err = walk_page_test(start, next, &walk);
			if (err > 0) {
				/*
				 * positive return values are purely for
				 * controlling the pagewalk, so should never
				 * be passed to the callers.
				 */
				err = 0;
				continue;
			}
			if (err < 0)
				break;
			err = __walk_page_range(start, next, &walk);
		}
		if (err)
			break;
	} while (start = next, start < end);
	return err;
}

/*
 * Similar to walk_page_range() but can walk any page tables even if they are
 * not backed by VMAs. Because 'unusual' entries may be walked this function
 * will also not lock the PTEs for the pte_entry() callback. This is useful for
 * walking the kernel pages tables or page tables for firmware.
 */
int walk_page_range_novma(struct mm_struct *mm, unsigned long start,
			  unsigned long end, const struct mm_walk_ops *ops,
			  pgd_t *pgd,
			  void *private)
{
	struct mm_walk walk = {
		.ops		= ops,
		.mm		= mm,
		.pgd		= pgd,
		.private	= private,
		.no_vma		= true
	};

	if (start >= end || !walk.mm)
		return -EINVAL;

	lockdep_assert_held(&walk.mm->mmap_sem);

	return walk_pgd_range(start, end, &walk);
}

int walk_page_vma(struct vm_area_struct *vma, const struct mm_walk_ops *ops,
		void *private)
{
	struct mm_walk walk = {
		.ops		= ops,
		.mm		= vma->vm_mm,
		.vma		= vma,
		.private	= private,
	};
	int err;

	if (!walk.mm)
		return -EINVAL;

	lockdep_assert_held(&walk.mm->mmap_sem);

	err = walk_page_test(vma->vm_start, vma->vm_end, &walk);
	if (err > 0)
		return 0;
	if (err < 0)
		return err;
	return __walk_page_range(vma->vm_start, vma->vm_end, &walk);
}
