/* Compatibility shims for building galcore 6.4.8 on Linux >= 6.x (fnOS 6.18) */
#ifndef GC_COMPAT618_H
#define GC_COMPAT618_H
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#ifndef nth_page
#define nth_page(page, n) ((page) + (n))
#endif
#if !defined(MAX_ORDER) && defined(MAX_PAGE_ORDER)
#define MAX_ORDER (MAX_PAGE_ORDER + 1)
#endif

/* __pte_offset_map_lock is not exported to modules on 6.x:
 * resolve a user address to a pfn with exported APIs instead. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/mmap_lock.h>
static inline int gc_user_addr_to_pfn(struct mm_struct *mm, unsigned long addr, unsigned long *pfn)
{
    struct vm_area_struct *vma;
    int ret = -EFAULT;

    if (!mm)
        return -EFAULT;
    mmap_read_lock(mm);
    vma = vma_lookup(mm, addr);
    if (vma && (vma->vm_flags & (VM_IO | VM_PFNMAP))) {
        struct follow_pfnmap_args a = { .vma = vma, .address = addr };
        if (!follow_pfnmap_start(&a)) {
            *pfn = a.pfn;
            follow_pfnmap_end(&a);
            ret = 0;
        }
    } else if (vma) {
        struct page *pg = NULL;
        if (get_user_pages(addr & PAGE_MASK, 1, 0, &pg) == 1) {
            *pfn = page_to_pfn(pg);
            put_page(pg);
            ret = 0;
        }
    }
    mmap_read_unlock(mm);
    return ret;
}
#define GC_HAVE_USER_ADDR_TO_PFN 1
#endif
#endif
