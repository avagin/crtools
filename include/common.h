#ifndef __CR_TYPES_H__
#define __CR_TYPES_H__

#include <stdbool.h>

#include "bug.h"
#include "list.h"

struct pid {
	/*
	 * The @real pid is used to fetch tasks during dumping stage,
	 * This is a global pid seen from the context where the dumping
	 * is running.
	 */
	pid_t real;

	/*
	 * The @virt pid is one which used in the image itself and keeps
	 * the pid value to be restored. This pid fetched from the
	 * dumpee context, because the dumpee might have own pid namespace.
	 */
	pid_t virt;
};

/*
 * When we have to restore a shared resource, we mush select which
 * task should do it, and make other(s) wait for it. In order to
 * avoid deadlocks, always make task with lower pid be the restorer.
 */
static inline bool pid_rst_prio(unsigned pid_a, unsigned pid_b)
{
	return pid_a < pid_b;
}

struct cr_fdset {
	int fd_off;
	int fd_nr;
	int *_fds;
};

static inline int fdset_fd(const struct cr_fdset *fdset, int type)
{
	int idx;

	idx = type - fdset->fd_off;
	BUG_ON(idx > fdset->fd_nr);

	return fdset->_fds[idx];
}

extern struct cr_fdset *glob_fdset;

struct cr_fdset *cr_task_fdset_open(int pid, int mode);
struct cr_fdset *cr_fdset_open_range(int pid, int from, int to,
			       unsigned long flags);
#define cr_fdset_open(pid, type, flags) cr_fdset_open_range(pid, \
		_CR_FD_##type##_FROM, _CR_FD_##type##_TO, flags)
struct cr_fdset *cr_glob_fdset_open(int mode);

void close_cr_fdset(struct cr_fdset **cr_fdset);

extern void print_data(unsigned long addr, unsigned char *data, size_t size);
extern void print_image_data(int fd, unsigned int length, int show);

struct cr_options {
	int			final_state;
	char			*show_dump_file;
	bool			check_ms_kernel;
	bool			show_pages_content;
	bool			restore_detach;
	bool			ext_unix_sk;
	bool			shell_job;
	bool			handle_file_locks;
	bool			tcp_established_ok;
	bool			evasive_devices;
	bool			link_remap_ok;
	unsigned int		rst_namespaces_flags;
	bool			log_file_per_pid;
	char			*output;
	char			*root;
	char			*pidfile;
	struct list_head	veth_pairs;
	struct list_head	scripts;
	bool			use_page_server;
	unsigned short		ps_port;
	char			*addr;
	bool			track_mem;
	char			*img_parent;
};

extern struct cr_options opts;

extern void init_opts(void);

struct script {
	struct list_head node;
	char *path;
};

#endif
