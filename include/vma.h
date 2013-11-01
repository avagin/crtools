#ifndef __CR_VMA_H__
#define __CR_VMA_H__

struct vm_area_list {
	struct list_head	h;
	unsigned		nr;
	unsigned long		priv_size; /* nr of pages in private VMAs */
	unsigned long		longest; /* nr of pages in longest VMA */
};

#define VM_AREA_LIST(name)	struct vm_area_list name = { .h = LIST_HEAD_INIT(name.h), .nr = 0, }

struct vma_area {
	struct list_head	list;
	VmaEntry		vma;

	union {
		int		vm_file_fd;
		int		vm_socket_id;
	};
	unsigned long		*page_bitmap;  /* existent pages */
	unsigned long		*ppage_bitmap; /* parent's existent pages */
};

int collect_mappings(pid_t pid, struct vm_area_list *vma_area_list);
void free_mappings(struct vm_area_list *vma_area_list);
bool privately_dump_vma(struct vma_area *vma);

#define vma_area_is(vma_area, s)	vma_entry_is(&((vma_area)->vma), s)
#define vma_area_len(vma_area)		vma_entry_len(&((vma_area)->vma))
#define vma_premmaped_start(vma)	((vma)->shmid)
#define vma_entry_is(vma, s)		(((vma)->status & (s)) == (s))
#define vma_entry_len(vma)		((vma)->end - (vma)->start)

static inline int in_vma_area(struct vma_area *vma, unsigned long addr)
{
	return addr >= (unsigned long)vma->vma.start &&
		addr < (unsigned long)vma->vma.end;
}

#endif
