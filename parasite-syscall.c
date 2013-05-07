#include <unistd.h>
#include <inttypes.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "protobuf.h"
#include "protobuf/sa.pb-c.h"
#include "protobuf/itimer.pb-c.h"
#include "protobuf/creds.pb-c.h"
#include "protobuf/core.pb-c.h"
#include "protobuf/pagemap.pb-c.h"

#include "syscall.h"
#include "ptrace.h"
#include "asm/processor-flags.h"
#include "parasite-syscall.h"
#include "parasite-blob.h"
#include "parasite.h"
#include "crtools.h"
#include "namespaces.h"
#include "pstree.h"
#include "net.h"
#include "mem.h"
#include "restorer.h"

#include <string.h>
#include <stdlib.h>

#include "asm/parasite-syscall.h"
#include "asm/dump.h"
#include "asm/restorer.h"

#define parasite_size		(round_up(sizeof(parasite_blob), PAGE_SIZE))

static int can_run_syscall(unsigned long ip, unsigned long start, unsigned long end)
{
	return ip >= start && ip < (end - code_syscall_size);
}

static int syscall_fits_vma_area(struct vma_area *vma_area)
{
	return can_run_syscall((unsigned long)vma_area->vma.start,
			       (unsigned long)vma_area->vma.start,
			       (unsigned long)vma_area->vma.end);
}

static struct vma_area *get_vma_by_ip(struct list_head *vma_area_list, unsigned long ip)
{
	struct vma_area *vma_area;

	list_for_each_entry(vma_area, vma_area_list, list) {
		if (vma_area->vma.start >= TASK_SIZE)
			continue;
		if (!(vma_area->vma.prot & PROT_EXEC))
			continue;
		if (syscall_fits_vma_area(vma_area))
			return vma_area;
	}

	return NULL;
}

/* we run at @regs->ip */
int __parasite_execute_trap(struct parasite_ctl *ctl, pid_t pid, user_regs_struct_t *regs)
{
	siginfo_t siginfo;
	int status;
	int ret = -1;

	if (ptrace(PTRACE_SETREGS, pid, NULL, regs)) {
		pr_perror("Can't set registers (pid: %d)", pid);
		goto err;
	}

	/*
	 * Most ideas are taken from Tejun Heo's parasite thread
	 * https://code.google.com/p/ptrace-parasite/
	 */

	if (ptrace(PTRACE_CONT, pid, NULL, NULL)) {
		pr_perror("Can't continue (pid: %d)", pid);
		goto err;
	}

	if (wait4(pid, &status, __WALL, NULL) != pid) {
		pr_perror("Waited pid mismatch (pid: %d)", pid);
		goto err;
	}

	if (!WIFSTOPPED(status)) {
		pr_err("Task is still running (pid: %d)\n", pid);
		goto err;
	}

	if (ptrace(PTRACE_GETSIGINFO, pid, NULL, &siginfo)) {
		pr_perror("Can't get siginfo (pid: %d)", pid);
		goto err;
	}

	if (ptrace(PTRACE_GETREGS, pid, NULL, regs)) {
		pr_perror("Can't obtain registers (pid: %d)", pid);
			goto err;
	}

	if (WSTOPSIG(status) != SIGTRAP || siginfo.si_code != ARCH_SI_TRAP) {
		pr_debug("** delivering signal %d si_code=%d\n",
			 siginfo.si_signo, siginfo.si_code);

		pr_err("Unexpected %d task interruption, aborting\n", pid);
		goto err;
	}

	/*
	 * We've reached this point if int3 is triggered inside our
	 * parasite code. So we're done.
	 */
	ret = 0;
err:
	return ret;
}

void *parasite_args_s(struct parasite_ctl *ctl, int args_size)
{
	BUG_ON(args_size > ctl->args_size);
	return ctl->addr_args;
}

#define parasite_args(ctl, type) ({				\
		BUILD_BUG_ON(sizeof(type) > PARASITE_ARG_SIZE_MIN);\
		ctl->addr_args;					\
	})

static int parasite_execute_trap_by_pid(unsigned int cmd, struct parasite_ctl *ctl, int id)
{
	struct parasite_thread_ctl *thread = &ctl->threads[id];
	pid_t pid = thread->tid;
	int ret;
	user_regs_struct_t regs = thread->regs_orig;

	*ctl->addr_cmd = cmd;

	parasite_setup_regs(ctl->parasite_ip, &regs);

	ret = __parasite_execute_trap(ctl, pid, &regs);
	if (ret == 0)
		ret = (int)REG_RES(regs);

	if (ret)
		pr_err("Parasite exited with %d\n", ret);

	if (ctl->pid.real != pid)
		if (ptrace(PTRACE_SETREGS, pid, NULL, &thread->regs_orig)) {
			pr_perror("Can't restore registers (pid: %d)", pid);
			return -1;
		}

	return ret;
}

static int parasite_execute_trap(unsigned int cmd, struct parasite_ctl *ctl)
{
	return parasite_execute_trap_by_pid(cmd, ctl, 0);
}

static int __parasite_send_cmd(int sockfd, struct ctl_msg *m)
{
	int ret;

	ret = send(sockfd, m, sizeof(*m), 0);
	if (ret == -1) {
		pr_perror("Failed to send command %d to daemon %d\n", m->cmd, m->id);
		return -1;
	} else if (ret != sizeof(*m)) {
		pr_err("Message to daemon is trimmed (%d/%d)\n",
		       (int)sizeof(*m), ret);
		return -1;
	}

	pr_debug("Sent msg to daemon %d %d %d %d\n", m->id, m->cmd, m->ack, m->err);
	return 0;
}

static int parasite_wait_ack(int sockfd, pid_t pid, unsigned int cmd, struct ctl_msg *m)
{
	int ret;

	pr_debug("Wait for ack %d-%d on daemon socket\n", pid, cmd);

	while (1) {
		memzero(m, sizeof(*m));

		ret = recv(sockfd, m, sizeof(*m), MSG_WAITALL);
		if (ret == -1) {
			pr_perror("Failed to read ack from %d", pid);
			return -1;
		} else if (ret != sizeof(*m)) {
			pr_err("Message reply from daemon is trimmed (%d/%d)\n",
			       (int)sizeof(*m), ret);
			return -1;
		}
		pr_debug("Fetched ack: %d %d %d %d\n",
			 m->id, m->cmd, m->ack, m->err);

		if (m->id != pid || m->cmd != cmd || m->ack != cmd) {
			pr_err("Communication error, this is not "
			       "the ack we expected\n");
			return -1;
		}
		return 0;
	}

	return -1;
}

int __parasite_execute_daemon_wait_ack(unsigned int cmd,
					      struct parasite_ctl *ctl,
					      pid_t pid)
{
	struct ctl_msg m;

	if (parasite_wait_ack(ctl->tsock, pid, cmd, &m))
		return -1;

	if (m.err != 0) {
		pr_err("Command %d for daemon %d failed with %d\n",
		       cmd, pid, m.err);
		return -1;
	}

	return 0;
}

int __parasite_execute_daemon_by_pid(unsigned int cmd,
					    struct parasite_ctl *ctl,
					    pid_t pid, bool wait_ack)
{
	struct ctl_msg m;

	m = ctl_msg_cmd(pid, cmd);
	if (__parasite_send_cmd(ctl->tsock, &m))
		return -1;

	if (wait_ack)
		return __parasite_execute_daemon_wait_ack(cmd, ctl, pid);

	return 0;
}

static int parasite_execute_daemon_by_pid(unsigned int cmd, struct parasite_ctl *ctl, pid_t pid)
{
	return __parasite_execute_daemon_by_pid(cmd, ctl, pid, true);
}

int parasite_execute_daemon(unsigned int cmd, struct parasite_ctl *ctl)
{
	return parasite_execute_daemon_by_pid(cmd, ctl, ctl->pid.real);
}

static int munmap_seized(struct parasite_ctl *ctl, void *addr, size_t length)
{
	unsigned long x;

	return syscall_seized(ctl, __NR_munmap, &x,
			(unsigned long)addr, length, 0, 0, 0, 0);
}

static int gen_parasite_saddr(struct sockaddr_un *saddr, int key)
{
	int sun_len;

	saddr->sun_family = AF_UNIX;
	snprintf(saddr->sun_path, UNIX_PATH_MAX,
			"X/crtools-pr-%d", key);

	sun_len = SUN_LEN(saddr);
	*saddr->sun_path = '\0';

	return sun_len;
}

int parasite_send_fd(struct parasite_ctl *ctl, int fd)
{
	if (send_fd(ctl->tsock, NULL, 0, fd) < 0) {
		pr_perror("Can't send file descriptor");
		return -1;
	}
	return 0;
}

static int parasite_set_logfd(struct parasite_ctl *ctl, pid_t pid)
{
	int ret;
	struct parasite_log_args *a;

	ret = parasite_send_fd(ctl, log_get_fd());
	if (ret)
		return ret;

	a = parasite_args(ctl, struct parasite_log_args);
	a->log_level = log_get_loglevel();

	ret = parasite_execute_trap(PARASITE_CMD_CFG_LOG, ctl);
	if (ret < 0)
		return ret;

	return 0;
}

static int parasite_init(struct parasite_ctl *ctl, pid_t pid, int nr_threads)
{
	struct parasite_init_args *args;
	static int sock = -1;

	args = parasite_args(ctl, struct parasite_init_args);

	pr_info("Putting tsock into pid %d\n", pid);
	args->h_addr_len = gen_parasite_saddr(&args->h_addr, getpid());
	args->p_addr_len = gen_parasite_saddr(&args->p_addr, pid);
	args->nr_threads = nr_threads;
	args->real = pid;

	if (sock == -1) {
		int rst = -1;

		if (current_ns_mask & CLONE_NEWNET) {
			pr_info("Switching to %d's net for tsock creation\n", pid);

			if (switch_ns(pid, &net_ns_desc, &rst))
				return -1;
		}

		sock = socket(PF_UNIX, SOCK_DGRAM, 0);
		if (sock < 0)
			pr_perror("Can't create socket");

		if (rst > 0 && restore_ns(rst, &net_ns_desc) < 0)
			return -1;
		if (sock < 0)
			return -1;

		if (bind(sock, (struct sockaddr *)&args->h_addr, args->h_addr_len) < 0) {
			pr_perror("Can't bind socket");
			goto err;
		}

	} else {
		struct sockaddr addr = { .sa_family = AF_UNSPEC, };

		/*
		 * When the peer of a dgram socket dies the original socket
		 * remains in connected state, thus denying any connections
		 * from "other" sources. Unconnect the socket by hands thus
		 * allowing for parasite to connect back.
		 */

		if (connect(sock, &addr, sizeof(addr)) < 0) {
			pr_perror("Can't unconnect");
			goto err;
		}
	}

	if (parasite_execute_trap(PARASITE_CMD_INIT, ctl) < 0) {
		pr_err("Can't init parasite\n");
		goto err;
	}

	if (connect(sock, (struct sockaddr *)&args->p_addr, args->p_addr_len) < 0) {
		pr_perror("Can't connect a transport socket");
		goto err;
	}

	ctl->tsock = sock;
	return 0;
err:
	close_safe(&sock);
	return -1;
}

static int parasite_daemonize(struct parasite_ctl *ctl, int id)
{
	struct parasite_thread_ctl *thread = &ctl->threads[id];
	pid_t pid = thread->tid;
	user_regs_struct_t regs;
	struct ctl_msg m = { };

	regs = thread->regs_orig;

	*ctl->addr_cmd = PARASITE_CMD_DAEMONIZE;
	parasite_setup_regs(ctl->parasite_ip, &regs);

	if (ptrace(PTRACE_SETREGS, pid, NULL, &regs)) {
		pr_perror("Can't set registers (pid: %d)", pid);
		goto err;
	}

	if (ptrace(PTRACE_CONT, pid, NULL, NULL)) {
		pr_perror("Can't continue (pid: %d)\n", pid);
		ptrace(PTRACE_SETREGS, pid, NULL, thread->regs_orig);
		goto err;
	}

	pr_info("Wait for parasite being daemonized...\n");

	if (parasite_wait_ack(ctl->tsock, pid, PARASITE_CMD_DAEMONIZE, &m)) {
		pr_err("Can't switch parasite %d to daemon mode %d\n",
		       pid, m.err);
		goto err;
	}

	thread->daemonized = true;
	pr_info("Parasite %d has been switched to daemon mode\n", pid);
	return 0;

err:
	return -1;
}

int parasite_dump_thread_seized(struct parasite_ctl *ctl, struct pid *tid,
		CoreEntry *core)
{
	struct parasite_dump_thread *args;
	int ret;

	args = parasite_args(ctl, struct parasite_dump_thread);
	args->real = tid->real;

	ret = parasite_execute_daemon_by_pid(PARASITE_CMD_DUMP_THREAD, ctl, tid->real);

	CORE_THREAD_ARCH_INFO(core)->clear_tid_addr = encode_pointer(args->tid_addr);
	tid->virt = args->tid;
	core_put_tls(core, args->tls);

	return ret;
}

int parasite_dump_sigacts_seized(struct parasite_ctl *ctl, struct cr_fdset *cr_fdset)
{
	struct parasite_dump_sa_args *args;
	int ret, sig, fd;
	SaEntry se = SA_ENTRY__INIT;

	args = parasite_args(ctl, struct parasite_dump_sa_args);

	ret = parasite_execute_daemon(PARASITE_CMD_DUMP_SIGACTS, ctl);
	if (ret < 0)
		return ret;

	fd = fdset_fd(cr_fdset, CR_FD_SIGACT);

	for (sig = 1; sig <= SIGMAX; sig++) {
		int i = sig - 1;

		if (sig == SIGSTOP || sig == SIGKILL)
			continue;

		ASSIGN_TYPED(se.sigaction, encode_pointer(args->sas[i].rt_sa_handler));
		ASSIGN_TYPED(se.flags, args->sas[i].rt_sa_flags);
		ASSIGN_TYPED(se.restorer, encode_pointer(args->sas[i].rt_sa_restorer));
		ASSIGN_TYPED(se.mask, args->sas[i].rt_sa_mask.sig[0]);

		if (pb_write_one(fd, &se, PB_SIGACT) < 0)
			return -1;
	}

	return 0;
}

static int dump_one_timer(struct itimerval *v, int fd)
{
	ItimerEntry ie = ITIMER_ENTRY__INIT;

	ie.isec = v->it_interval.tv_sec;
	ie.iusec = v->it_interval.tv_usec;
	ie.vsec = v->it_value.tv_sec;
	ie.vusec = v->it_value.tv_sec;

	return pb_write_one(fd, &ie, PB_ITIMERS);
}

int parasite_dump_itimers_seized(struct parasite_ctl *ctl, struct cr_fdset *cr_fdset)
{
	struct parasite_dump_itimers_args *args;
	int ret, fd;

	args = parasite_args(ctl, struct parasite_dump_itimers_args);

	ret = parasite_execute_daemon(PARASITE_CMD_DUMP_ITIMERS, ctl);
	if (ret < 0)
		return ret;

	fd = fdset_fd(cr_fdset, CR_FD_ITIMERS);

	ret = dump_one_timer(&args->real, fd);
	if (!ret)
		ret = dump_one_timer(&args->virt, fd);
	if (!ret)
		ret = dump_one_timer(&args->prof, fd);

	return ret;
}

int parasite_dump_misc_seized(struct parasite_ctl *ctl, struct parasite_dump_misc *misc)
{
	struct parasite_dump_misc *ma;

	ma = parasite_args(ctl, struct parasite_dump_misc);
	if (parasite_execute_daemon(PARASITE_CMD_DUMP_MISC, ctl) < 0)
		return -1;

	*misc = *ma;
	return 0;
}

struct parasite_tty_args *parasite_dump_tty(struct parasite_ctl *ctl, int fd)
{
	struct parasite_tty_args *p;

	p = parasite_args(ctl, struct parasite_tty_args);
	p->fd = fd;

	if (parasite_execute_daemon(PARASITE_CMD_DUMP_TTY, ctl) < 0)
		return NULL;

	return p;
}

int parasite_dump_creds(struct parasite_ctl *ctl, CredsEntry *ce)
{
	struct parasite_dump_creds *pc;

	pc = parasite_args(ctl, struct parasite_dump_creds);
	if (parasite_execute_daemon(PARASITE_CMD_DUMP_CREDS, ctl) < 0)
		return -1;

	ce->secbits = pc->secbits;
	ce->n_groups = pc->ngroups;

	/*
	 * Achtung! We leak the parasite args pointer to the caller.
	 * It's not safe in general, but in our case is OK, since the
	 * latter doesn't go to parasite before using the data in it.
	 */

	BUILD_BUG_ON(sizeof(ce->groups[0]) != sizeof(pc->groups[0]));
	ce->groups = pc->groups;
	return 0;
}

static unsigned int vmas_mprotect_size(struct vm_area_list *vmas)
{
	return sizeof(struct parasite_mprotect_args) +
		(vmas->nr * sizeof(struct parasite_vma_entry));
}

int parasite_drain_fds_seized(struct parasite_ctl *ctl,
		struct parasite_drain_fd *dfds, int *lfds, struct fd_opts *opts)
{
	int ret = -1, size;
	struct parasite_drain_fd *args;

	size = drain_fds_size(dfds);
	args = parasite_args_s(ctl, size);
	memcpy(args, dfds, size);

	ret = __parasite_execute_daemon_by_pid(PARASITE_CMD_DRAIN_FDS, ctl,
					       ctl->pid.real, false);
	if (ret) {
		pr_err("Parasite failed to drain descriptors\n");
		goto err;
	}

	ret = recv_fds(ctl->tsock, lfds, dfds->nr_fds, opts);
	if (ret)
		pr_err("Can't retrieve FDs from socket\n");

	ret |= __parasite_execute_daemon_wait_ack(PARASITE_CMD_DRAIN_FDS, ctl,
						  ctl->pid.real);
err:
	return ret;
}

int parasite_get_proc_fd_seized(struct parasite_ctl *ctl)
{
	int ret = -1, fd;

	ret = __parasite_execute_daemon_by_pid(PARASITE_CMD_GET_PROC_FD, ctl,
					       ctl->pid.real, false);
	if (ret) {
		pr_err("Parasite failed to get proc fd\n");
		return ret;
	}

	fd = recv_fd(ctl->tsock);
	if (fd < 0)
		pr_err("Can't retrieve FD from socket\n");
	if (__parasite_execute_daemon_wait_ack(PARASITE_CMD_GET_PROC_FD, ctl, ctl->pid.real)) {
		close(fd);
		return -1;
	}

	return fd;
}

int parasite_init_threads_seized(struct parasite_ctl *ctl, struct pstree_item *item)
{
	struct parasite_init_args *args;
	int ret = 0, i;

	args = parasite_args(ctl, struct parasite_init_args);

	for (i = 1; i < item->nr_threads; i++) {
		pid_t tid = item->threads[i].real;

		ctl->threads[i].tid = tid;
		ctl->nr_threads++;

		args->real = tid;

		ret = ptrace(PTRACE_GETREGS, tid, NULL, &ctl->threads[i].regs_orig);
		if (ret) {
			pr_perror("Can't obtain registers (pid: %d)", tid);
			goto err;
		}

		ret = parasite_execute_trap_by_pid(PARASITE_CMD_INIT_THREAD, ctl, i);
		if (ret) {
			pr_err("Can't init thread in parasite %d\n",
			       item->threads[i].real);
			goto err;
		}

		if (parasite_daemonize(ctl, i))
			goto err;
	}

	return 0;
err:
	return -1 ;
}

int parasite_fini_threads_seized(struct parasite_ctl *ctl)
{
	struct parasite_init_args *args;
	int ret = 0, i, status;

	args = parasite_args(ctl, struct parasite_init_args);

	for (i = 1; i < ctl->nr_threads; i++) {
		pid_t tid = ctl->threads[i].tid;

		if (!ctl->threads[i].daemonized)
			break;

		args->real = tid;
		ret = parasite_execute_daemon_by_pid(PARASITE_CMD_FINI_THREAD, ctl, tid);
		/*
		 * Note the thread's fini() can be called even when not
		 * all threads were init()'ed, say we're rolling back from
		 * error happened while we were init()'ing some thread, thus
		 * -ENOENT will be returned but we should continie for the
		 * rest of threads set.
		 *
		 * Strictly speaking we always init() threads in sequence thus
		 * we could simply break the loop once first -ENOENT returned
		 * but I prefer to be on a safe side even if some future changes
		 * would change the code logic.
		 */
		if (ret && ret != -ENOENT) {
			pr_err("Can't fini thread in parasite %d\n", tid);
			break;
		} else if (ret == -ENOENT)
			continue;

		pr_debug("Waiting for %d to trap\n", tid);
		if (wait4(tid, &status, __WALL, NULL) != tid) {
			pr_perror("Waited pid mismatch (pid: %d)", tid);
			break;
		}

		pr_debug("Daemon %d exited trapping\n", tid);
		if (!WIFSTOPPED(status)) {
			pr_err("Task is still running (pid: %d)\n", tid);
			break;
		}

		if (ptrace(PTRACE_SETREGS, tid, NULL, &ctl->threads[i].regs_orig)) {
			pr_perror("Can't restore registers (pid: %d)", tid);
			return -1;
		}
	}

	return ret;
}

static int block_signals(struct pstree_item *item, struct parasite_ctl *ctl)
{
	int ret = 0, i;
	k_rtsigset_t blockall;

	ksigfillset(&blockall);

	for (i = 0; i < item->nr_threads; i++) {
		CoreEntry *core = item->core[i];
		k_rtsigset_t *mask = &ctl->threads[i].sig_blocked;
		pid_t tid = item->threads[i].real;

		ret = ptrace(PTRACE_GETSIGMASK, tid, sizeof(*mask), mask);
		if (ret) {
			pr_perror("Can't get sigblockmask of %d\n", tid);
			break;
		}
		ret = ptrace(PTRACE_SETSIGMASK, tid, sizeof(blockall), &blockall);
		if (ret) {
			pr_perror("Can't block all signals of %d\n", tid);
			break;
		}
		ctl->threads[i].use_sig_blocked = true;

		if (core->tc) {
			BUILD_BUG_ON(sizeof(core->tc->blk_sigset) != sizeof(k_rtsigset_t));
			memcpy(&core->tc->blk_sigset, mask, sizeof(k_rtsigset_t));
		}

		core->thread_core->has_blk_sigset = true;
		memcpy(&core->thread_core->blk_sigset, mask, sizeof(k_rtsigset_t));

	}

	return ret;
}

static int unblock_signals(struct parasite_ctl *ctl)
{
	int ret = 0, i;

	for (i = 0; i < ctl->nr_threads; i++) {
		k_rtsigset_t *mask = &ctl->threads[i].sig_blocked;
		pid_t tid = ctl->threads[i].tid;

		ret = ptrace(PTRACE_SETSIGMASK, tid, sizeof(*mask), mask);
		if (ret) {
			pr_perror("Can't restore sigblockmask of %d\n", tid);
			break;
		}
	}

	return ret;
}

static int parasite_fini_seized(struct parasite_ctl *ctl)
{
	struct parasite_init_args *args;
	int status, ret = 0;

	args = parasite_args(ctl, struct parasite_init_args);
	args->real = ctl->pid.real;

	args->real = ctl->pid.real;
	__parasite_execute_daemon_by_pid(PARASITE_CMD_FINI, ctl, ctl->pid.real, false);

	if (wait4(ctl->pid.real, &status, __WALL, NULL) != ctl->pid.real) {
		pr_perror("Waited pid mismatch (pid: %d)", ctl->pid.real);
		ret = -1;
	}

	if (!WIFSTOPPED(status)) {
		pr_err("Task is still running (pid: %d)\n", ctl->pid.real);
		ret = -1;
	}

	return ret;
}

int parasite_cure_seized(struct parasite_ctl *ctl)
{
	int ret = 0;

	if (ctl->parasite_ip) {
		ret = parasite_fini_threads_seized(ctl);
		parasite_fini_seized(ctl);
	}

	ctl->tsock = -1;

	if (ctl->remote_map) {
		if (munmap_seized(ctl, (void *)ctl->remote_map, ctl->map_length)) {
			pr_err("munmap_seized failed (pid: %d)\n", ctl->pid.real);
			ret = -1;
		}
	}

	if (ctl->local_map) {
		if (munmap(ctl->local_map, ctl->map_length)) {
			pr_err("munmap failed (pid: %d)\n", ctl->pid.real);
			ret = -1;
		}
	}

	if (ptrace_poke_area(ctl->pid.real, (void *)ctl->code_orig,
			     (void *)ctl->syscall_ip, sizeof(ctl->code_orig))) {
		pr_err("Can't restore syscall blob (pid: %d)\n", ctl->pid.real);
		ret = -1;
	}

	if (ptrace(PTRACE_SETREGS, ctl->pid.real, NULL, &ctl->threads[0].regs_orig)) {
		pr_err("Can't restore registers (pid: %d)\n", ctl->pid.real);
		ret = -1;
	}

	if (unblock_signals(ctl))
		ret = -1;

	xfree(ctl);
	return ret;
}

struct parasite_ctl *parasite_prep_ctl(pid_t pid, struct vm_area_list *vma_area_list, unsigned int nr_threads)
{
	struct parasite_ctl *ctl = NULL;
	struct vma_area *vma_area;

	BUG_ON(nr_threads == 0);

	if (!arch_can_dump_task(pid))
		goto err;

	/*
	 * Control block early setup.
	 */
	ctl = xzalloc(sizeof(*ctl) + nr_threads * sizeof(ctl->threads[0]));
	if (!ctl) {
		pr_err("Parasite control block allocation failed (pid: %d)\n", pid);
		goto err;
	}

	ctl->tsock = -1;
	ctl->nr_threads = 1;
	ctl->threads[0].tid = pid;

	if (ptrace(PTRACE_GETREGS, pid, NULL, &ctl->threads[0].regs_orig)) {
		pr_err("Can't obtain registers (pid: %d)\n", pid);
		goto err;
	}

	vma_area = get_vma_by_ip(&vma_area_list->h, REG_IP(ctl->threads[0].regs_orig));
	if (!vma_area) {
		pr_err("No suitable VMA found to run parasite "
		       "bootstrap code (pid: %d)\n", pid);
		goto err;
	}

	ctl->pid.real	= pid;
	ctl->pid.virt	= 0;
	ctl->syscall_ip	= vma_area->vma.start;

	/*
	 * Inject syscall instruction and remember original code,
	 * we will need it to restore original program content.
	 */
	memcpy(ctl->code_orig, code_syscall, sizeof(ctl->code_orig));
	if (ptrace_swap_area(pid, (void *)ctl->syscall_ip,
			     (void *)ctl->code_orig, sizeof(ctl->code_orig))) {
		pr_err("Can't inject syscall blob (pid: %d)\n", pid);
		goto err;
	}

	return ctl;

err:
	xfree(ctl);
	return NULL;
}

int parasite_map_exchange(struct parasite_ctl *ctl, unsigned long size)
{
	int fd;

	ctl->remote_map = mmap_seized(ctl, NULL, size,
				      PROT_READ | PROT_WRITE | PROT_EXEC,
				      MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	if (!ctl->remote_map) {
		pr_err("Can't allocate memory for parasite blob (pid: %d)\n", ctl->pid.real);
		return -1;
	}

	ctl->map_length = round_up(size, PAGE_SIZE);

	fd = open_proc_rw(ctl->pid.real, "map_files/%p-%p",
		 ctl->remote_map, ctl->remote_map + ctl->map_length);
	if (fd < 0)
		return -1;

	ctl->local_map = mmap(NULL, size, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_FILE, fd, 0);
	close(fd);

	if (ctl->local_map == MAP_FAILED) {
		ctl->local_map = NULL;
		pr_perror("Can't map remote parasite map");
		return -1;
	}

	return 0;
}

static unsigned long parasite_args_size(struct vm_area_list *vmas, struct parasite_drain_fd *dfds)
{
	unsigned long size = PARASITE_ARG_SIZE_MIN;

	size = max(size, (unsigned long)drain_fds_size(dfds));
	size = max(size, (unsigned long)vmas_pagemap_size(vmas));
	size = max(size, (unsigned long)vmas_mprotect_size(vmas));

	return round_up(size, PAGE_SIZE);
}

struct parasite_ctl *parasite_infect_seized(pid_t pid, struct pstree_item *item,
		struct vm_area_list *vma_area_list, struct parasite_drain_fd *dfds)
{
	int ret;
	struct parasite_ctl *ctl;

	BUG_ON(item->threads[0].real != pid);

	ctl = parasite_prep_ctl(pid, vma_area_list, item->nr_threads);
	if (!ctl)
		return NULL;

	if (block_signals(item, ctl))
		goto err_restore;

	/*
	 * Inject a parasite engine. Ie allocate memory inside alien
	 * space and copy engine code there. Then re-map the engine
	 * locally, so we will get an easy way to access engine memory
	 * without using ptrace at all.
	 */

	ctl->args_size = parasite_args_size(vma_area_list, dfds);
	ret = parasite_map_exchange(ctl, parasite_size +
				ctl->args_size +
				item->nr_threads * RESTORE_STACK_SIGFRAME);
	if (ret)
		goto err_restore;

	for (i = 0; i < item->nr_threads; i++) {
		struct parasite_thread_ctl *thread = &ctl->threads[i];

		thread->sigframe = ctl->local_map + parasite_size + ctl->args_size + i * RESTORE_STACK_SIGFRAME;
		thread->rsigframe = ctl->remote_map + parasite_size + ctl->args_size + i * RESTORE_STACK_SIGFRAME;

		if (construct_sigframe(thread->sigframe, thread->rsigframe, item->core[i]))
			goto err_restore;
	}

	pr_info("Putting parasite blob into %p->%p\n", ctl->local_map, ctl->remote_map);
	memcpy(ctl->local_map, parasite_blob, sizeof(parasite_blob));

	/* Setup the rest of a control block */
	ctl->parasite_ip	= (unsigned long)parasite_sym(ctl->remote_map, __export_parasite_head_start);
	ctl->addr_cmd		= parasite_sym(ctl->local_map, __export_parasite_cmd);
	ctl->addr_args		= parasite_sym(ctl->local_map, __export_parasite_args);

	ret = parasite_init(ctl, pid, item->nr_threads);
	if (ret) {
		pr_err("%d: Can't create a transport socket\n", pid);
		goto err_restore;
	}

	ret = parasite_set_logfd(ctl, pid);
	if (ret) {
		pr_err("%d: Can't set a logging descriptor\n", pid);
		goto err_restore;
	}

	if (parasite_daemonize(ctl, 0))
		goto err_restore;

	ret = parasite_init_threads_seized(ctl, item);
	if (ret)
		goto err_restore;

	return ctl;

err_restore:
	parasite_cure_seized(ctl);
	return NULL;
}

