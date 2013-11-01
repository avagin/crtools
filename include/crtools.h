#ifndef __CR_CRTOOLS_H__
#define __CR_CRTOOLS_H__

#include <sys/types.h>

#include "list.h"
#include "asm/types.h"
#include "list.h"
#include "util.h"
#include "lock.h"
#include "cr-show.h"

#include "protobuf/vma.pb-c.h"

int check_img_inventory(void);
int write_img_inventory(void);
void kill_inventory(void);

extern int open_image_dir(void);
extern void close_image_dir(void);

#define LAST_PID_PATH		"/proc/sys/kernel/ns_last_pid"

int cr_dump_tasks(pid_t pid);
int cr_pre_dump_tasks(pid_t pid);
int cr_restore_tasks(void);
int cr_show(int pid);
int convert_to_elf(char *elf_path, int fd_core);
int cr_check(void);
int cr_exec(int pid, char **opts);

void restrict_uid(unsigned int uid, unsigned int gid);
struct proc_status_creds;
bool may_dump(struct proc_status_creds *);
struct _CredsEntry;
bool may_restore(struct _CredsEntry *);

#endif /* __CR_CRTOOLS_H__ */
