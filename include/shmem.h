#ifndef __CR_SHMEM_H__
#define __CR_SHMEM_H__

#include "lock.h"

#include "protobuf/vma.pb-c.h"

#define SHMEMS_SIZE	4096

/*
 * pid is a pid of a creater
 * start, end are used for open mapping
 * fd is a file discriptor, which is valid for creater,
 * it's opened in cr-restor, because pgoff may be non zero
 */
struct shmem_info {
	unsigned long	shmid;
	unsigned long	start;
	unsigned long	end;
	unsigned long	size;
	int		pid;
	int		fd;
	futex_t		lock;
};

struct shmems {
	int			nr_shmems;
	struct shmem_info	entries[0];
};

int prepare_shmem_pid(int pid);
int prepare_shmem_restore(void);
void show_saved_shmems(void);
int get_shmem_fd(int pid, VmaEntry *vi);

struct shmems;
extern struct shmems *rst_shmems;

int cr_dump_shmem(void);
int add_shmem_area(pid_t pid, VmaEntry *vma);

static always_inline struct shmem_info *
find_shmem(struct shmems *shmems, unsigned long shmid)
{
	struct shmem_info *si;
	int i;

	for (i = 0; i < shmems->nr_shmems; i++) {
		si = &shmems->entries[i];
		if (si->shmid == shmid)
			return si;
	}

	return NULL;
}

#endif /* __CR_SHMEM_H__ */
