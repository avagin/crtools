#include <sys/mman.h>
#include <errno.h>
#include <signal.h>
#include <linux/limits.h>
#include <sys/mount.h>
#include <stdarg.h>
#include <sys/ioctl.h>

#include "syscall.h"
#include "parasite.h"
#include "lock.h"
#include "log.h"

#include <string.h>

#include "asm/parasite.h"

static int tsock = -1;

static struct tid_state_s {
	pid_t		real;
	pid_t		tid;

	futex_t		cmd;
	futex_t		ack;
	int		ret;

	struct rt_sigframe *sigframe;

	void		*next;
	unsigned char	stack[PARASITE_STACK_SIZE] __aligned(8);
} *tid_state;

static unsigned int nr_tid_state;
static unsigned int next_tid_state;

#define TID_STATE_SIZE(n)	\
	(ALIGN(sizeof(struct tid_state_s) * n, PAGE_SIZE))

#define thread_leader	(&tid_state[0])

#ifndef SPLICE_F_GIFT
#define SPLICE_F_GIFT	0x08
#endif

static struct tid_state_s *tid_table[512];

static int mprotect_vmas(struct parasite_mprotect_args *args)
{
	struct parasite_vma_entry *vma;
	int ret = 0, i;

	for (i = 0; i < args->nr; i++) {
		vma = args->vmas + i;
		ret = sys_mprotect((void *)vma->start, vma->len, vma->prot);
		if (ret) {
			pr_err("mprotect(%08lx, %lu) failed with code %d\n",
						vma->start, vma->len, ret);
			break;
		}
	}

	return ret;
}

static int dump_pages(struct parasite_dump_pages_args *args)
{
	int p, ret;

	p = recv_fd(tsock);
	if (p < 0)
		return -1;

	ret = sys_vmsplice(p, &args->iovs[args->off], args->nr,
				SPLICE_F_GIFT | SPLICE_F_NONBLOCK);
	if (ret != PAGE_SIZE * args->nr_pages) {
		sys_close(p);
		pr_err("Can't splice pages ti pipe (%d/%d)", ret, args->nr_pages);
		return -1;
	}

	sys_close(p);
	return 0;
}

static int dump_sigact(struct parasite_dump_sa_args *da)
{
	int sig, ret = 0;

	for (sig = 1; sig <= SIGMAX; sig++) {
		int i = sig - 1;

		if (sig == SIGKILL || sig == SIGSTOP)
			continue;

		ret = sys_sigaction(sig, NULL, &da->sas[i], sizeof(k_rtsigset_t));
		if (ret < 0) {
			pr_err("sys_sigaction failed\n");
			break;
		}
	}

	return ret;
}

static int dump_itimers(struct parasite_dump_itimers_args *args)
{
	int ret;

	ret = sys_getitimer(ITIMER_REAL, &args->real);
	if (!ret)
		ret = sys_getitimer(ITIMER_VIRTUAL, &args->virt);
	if (!ret)
		ret = sys_getitimer(ITIMER_PROF, &args->prof);

	if (ret)
		pr_err("getitimer failed\n");

	return ret;
}

static int dump_misc(struct parasite_dump_misc *args)
{
	args->brk = sys_brk(0);

	args->pid = sys_getpid();
	args->sid = sys_getsid();
	args->pgid = sys_getpgid(0);
	args->tls = arch_get_tls();
	args->umask = sys_umask(0);
	sys_umask(args->umask); /* never fails */

	return 0;
}

static int dump_creds(struct parasite_dump_creds *args)
{
	int ret;

	args->secbits = sys_prctl(PR_GET_SECUREBITS, 0, 0, 0, 0);

	ret = sys_getgroups(0, NULL);
	if (ret < 0)
		goto grps_err;

	args->ngroups = ret;
	if (args->ngroups >= PARASITE_MAX_GROUPS) {
		pr_err("Too many groups in task %d\n", (int)args->ngroups);
		return -1;
	}

	ret = sys_getgroups(args->ngroups, args->groups);
	if (ret < 0)
		goto grps_err;

	if (ret != args->ngroups) {
		pr_err("Groups changed on the fly %d -> %d\n",
				args->ngroups, ret);
		return -1;
	}

	return 0;

grps_err:
	pr_err("Error calling getgroups (%d)\n", ret);
	return -1;
}

static int drain_fds(struct parasite_drain_fd *args)
{
	int ret;

	ret = send_fds(tsock, NULL, 0,
		       args->fds, args->nr_fds, true);
	if (ret)
		pr_err("send_fds failed\n");

	return ret;
}

static void hash_thread_state(struct tid_state_s *s)
{
	unsigned int pos = s->real % ARRAY_SIZE(tid_table);
	struct tid_state_s *next = tid_table[pos];

	tid_table[pos] = s;
	s->next = next;
}

static struct tid_state_s *find_thread_state(pid_t real)
{
	unsigned int pos = real % ARRAY_SIZE(tid_table);
	struct tid_state_s *s;

	for (s = tid_table[pos]; s; s = s->next) {
		if (s->real == real)
			return s;
	}

	return NULL;
}

static int dump_thread(struct parasite_dump_thread *args)
{
	pid_t tid = sys_gettid();
	struct tid_state_s *s;
	int ret;

	s = find_thread_state(args->real);
	if (!s)
		return -ENOENT;

	ret = sys_prctl(PR_GET_TID_ADDRESS, (unsigned long) &args->tid_addr, 0, 0, 0);
	if (ret)
		return ret;

	args->tid = tid;
	args->tls = arch_get_tls();

	return 0;
}

static int init_thread(struct parasite_init_args *args)
{
	if (next_tid_state >= nr_tid_state)
		return -ENOMEM;

	tid_state[next_tid_state].tid = sys_gettid();
	tid_state[next_tid_state].real = args->real;
	tid_state[next_tid_state].sigframe = args->sigframe;

	futex_set(&tid_state[next_tid_state].cmd, PARASITE_CMD_IDLE);
	futex_set(&tid_state[next_tid_state].ack, PARASITE_CMD_IDLE);

	hash_thread_state(&tid_state[next_tid_state]);

	next_tid_state++;

	return 0;
}

static int fini_thread(struct tid_state_s *s)
{
	return 0;
}

static int init(struct parasite_init_args *args)
{
	int ret;

	if (!args->nr_threads)
		return -EINVAL;

	tid_state = (void *)sys_mmap(NULL, TID_STATE_SIZE(args->nr_threads),
				     PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS,
				     -1, 0);
	if ((unsigned long)tid_state > TASK_SIZE)
		return -ENOMEM;

	nr_tid_state = args->nr_threads;

	ret = init_thread(args);
	if (ret < 0)
		return ret;

	tsock = sys_socket(PF_UNIX, SOCK_DGRAM, 0);
	if (tsock < 0)
		return tsock;

	ret = sys_bind(tsock, (struct sockaddr *) &args->p_addr, args->p_addr_len);
	if (ret < 0)
		return ret;

	ret = sys_connect(tsock, (struct sockaddr *)&args->h_addr, args->h_addr_len);
	if (ret < 0)
		return ret;

	return 0;
}

static char proc_mountpoint[] = "proc.crtools";
static int parasite_get_proc_fd()
{
	int ret, fd = -1;
	char buf[2];

	ret = sys_readlink("/proc/self", buf, sizeof(buf));
	if (ret < 0 && ret != -ENOENT) {
		pr_err("Can't readlink /proc/self\n");
		return ret;
	}

	/* Fast path -- if /proc belongs to this pidns */
	if (ret == 1 && buf[0] == '1') {
		fd = sys_open("/proc", O_RDONLY, 0);
		goto out_send_fd;
	}

	if (sys_mkdir(proc_mountpoint, 0700)) {
		pr_err("Can't create a directory\n");
		return ret;
	}

	if (sys_mount("proc", proc_mountpoint, "proc", MS_MGC_VAL, NULL)) {
		pr_err("mount failed\n");
		goto out_rmdir;
	}

	fd = sys_open(proc_mountpoint, O_RDONLY, 0);

	if (sys_umount2(proc_mountpoint, MNT_DETACH)) {
		pr_err("Can't umount procfs\n");
		return -1;
	}

out_rmdir:
	if (sys_rmdir(proc_mountpoint)) {
		pr_err("Can't remove directory\n");
		return -1;
	}

out_send_fd:
	if (fd < 0)
		return fd;
	ret = send_fd(tsock, NULL, 0, fd);
	sys_close(fd);
	return ret;
}

static inline int tty_ioctl(int fd, int cmd, int *arg)
{
	int ret;

	ret = sys_ioctl(fd, cmd, (unsigned long)arg);
	if (ret < 0) {
		if (ret != -ENOTTY)
			return ret;
		*arg = 0;
	}
	return 0;
}

static int parasite_dump_tty(struct parasite_tty_args *args)
{
	int ret;

#ifndef TIOCGPKT
# define TIOCGPKT	_IOR('T', 0x38, int)
#endif

#ifndef TIOCGPTLCK
# define TIOCGPTLCK	_IOR('T', 0x39, int)
#endif

#ifndef TIOCGEXCL
# define TIOCGEXCL	_IOR('T', 0x40, int)
#endif

	ret = tty_ioctl(args->fd, TIOCGSID, &args->sid);
	if (ret < 0)
		goto err;

	ret = tty_ioctl(args->fd, TIOCGPGRP, &args->pgrp);
	if (ret < 0)
		goto err;

	ret = tty_ioctl(args->fd, TIOCGPKT, &args->st_pckt);
	if (ret < 0)
		goto err;

	ret = tty_ioctl(args->fd, TIOCGPTLCK, &args->st_lock);
	if (ret < 0)
		goto err;

	ret = tty_ioctl(args->fd, TIOCGEXCL, &args->st_excl);
	if (ret < 0)
		goto err;

	args->hangup = false;
	return 0;

err:
	if (ret != -EIO) {
		pr_err("TTY: Can't get sid/pgrp: %d\n", ret);
		return -1;
	}

	/* kernel reports EIO for get ioctls on pair-less ptys */
	args->sid = 0;
	args->pgrp = 0;
	args->st_pckt = 0;
	args->st_lock = 0;
	args->st_excl = 0;
	args->hangup = true;

	return 0;
}

static int parasite_cfg_log(struct parasite_log_args *args)
{
	int ret;

	ret = recv_fd(tsock);
	if (ret >= 0) {
		log_set_fd(ret);
		log_set_loglevel(args->log_level);
		ret = 0;
	}

	return ret;
}

static int fini(struct tid_state_s *s)
{
	log_set_fd(-1);

	return fini_thread(s);
}

static int __parasite_daemon_reply_ack(unsigned int id, unsigned int cmd, int err)
{
	struct ctl_msg m;
	int ret;

	m = ctl_msg_ack(id, cmd, err);
	ret = sys_sendto(tsock, &m, sizeof(m), 0, NULL, 0);
	if (ret != sizeof(m)) {
		pr_err("Sent only %d bytes while %d expected\n",
			ret, (int)sizeof(m));
		return -1;
	}

	pr_debug("__sent ack msg: %d %d %d %d\n",
		 m.id, m.cmd, m.ack, m.err);

	return 0;
}

static int __parasite_daemon_wait_msg(struct ctl_msg *m)
{
	int ret;

	pr_debug("Daemon wais for command\n");

	while (1) {
		*m = (struct ctl_msg){ };
		ret = sys_recvfrom(tsock, m, sizeof(*m), MSG_WAITALL, NULL, 0);
		if (ret != sizeof(*m)) {
			pr_err("Trimmed message received (%d/%d)\n",
			       (int)sizeof(*m), ret);
			return 0;
		}

		pr_debug("__fetched msg: %d %d %d %d\n",
			 m->id, m->cmd, m->ack, m->err);
		return 0;
	}

	return -1;
}

static int __parasite_daemon_thread_wait_cmd(struct tid_state_s *s)
{
	futex_wait_while_lt(&s->cmd, PARASITE_CMD_DAEMONIZED);
	return futex_get(&s->cmd);
}

static void __parasite_daemon_thread_ack(struct tid_state_s *s, int ret)
{
	s->ret = ret;
	futex_set(&s->ack, PARASITE_CMD_IDLE);
	futex_set_and_wake(&s->cmd, PARASITE_CMD_IDLE);
}

static unsigned long noinline __used
__parasite_daemon_thread(void *args, struct tid_state_s *s, unsigned long oldstack)
{
	pr_debug("Running daemon thread %d\n", s->real);

	/* Reply we're alive */
	if (__parasite_daemon_reply_ack(s->real, PARASITE_CMD_DAEMONIZE, 0))
		return oldstack;

	while (1) {
		int ret, cmd;

		cmd = __parasite_daemon_thread_wait_cmd(s);

		pr_debug("Command %d in daemon thread %d\n", cmd, s->real);

		switch (cmd) {
		case PARASITE_CMD_DUMP_THREAD:
			ret = dump_thread(args);
			break;
		case PARASITE_CMD_FINI_THREAD:
			__parasite_daemon_thread_ack(s, 0);
			fini_thread(s);
			return oldstack;
		default:
			pr_err("Unknown command in parasite daemon thread: %d\n", cmd);
			ret = -1;
			break;
		}
		__parasite_daemon_thread_ack(s, ret);
	}

	return oldstack;
}

static int __parasite_execute_thread(struct ctl_msg *m)
{
	struct tid_state_s *s = find_thread_state(m->id);
	if (!s)
		return -ENOENT;

	pr_debug("Wake thread %d daemon with command %d\n", s->real, m->cmd);
	futex_set_and_wake(&s->cmd, m->cmd);

	pr_debug("Wait thread %d for PARASITE_CMD_IDLE\n", s->real);
	futex_wait_until(&s->cmd, PARASITE_CMD_IDLE);

	return s->ret;
}

static unsigned long noinline __used
__parasite_daemon_thread_leader(void *args, struct tid_state_s *s, unsigned long oldstack)
{
	struct ctl_msg m = { };
	int ret = -1;

	pr_debug("Running daemon thread leader %d\n", s->real);

	/* Reply we're alive */
	if (__parasite_daemon_reply_ack(s->real, PARASITE_CMD_DAEMONIZE, 0))
		return oldstack;

	while (1) {
		if (__parasite_daemon_wait_msg(&m))
			break;

		switch (m.cmd) {
		case PARASITE_CMD_FINI:
			ret = fini(s);
			sys_close(tsock);
			/*
			 * No ACK here since we're getting out.
			 */
			break;
		case PARASITE_CMD_FINI_THREAD:
			ret = __parasite_execute_thread(&m);
			break;
		case PARASITE_CMD_DUMP_THREAD:
			ret = __parasite_execute_thread(&m);
			break;
		case PARASITE_CMD_DUMPPAGES:
			ret = dump_pages(args);
			break;
		case PARASITE_CMD_MPROTECT_VMAS:
			ret = mprotect_vmas(args);
			break;
		case PARASITE_CMD_DUMP_SIGACTS:
			ret = dump_sigact(args);
			break;
		case PARASITE_CMD_DUMP_ITIMERS:
			ret = dump_itimers(args);
			break;
		case PARASITE_CMD_DUMP_MISC:
			ret = dump_misc(args);
			break;
		case PARASITE_CMD_DUMP_CREDS:
			ret = dump_creds(args);
			break;
		case PARASITE_CMD_DRAIN_FDS:
			ret = drain_fds(args);
			break;
		case PARASITE_CMD_GET_PROC_FD:
			ret = parasite_get_proc_fd();
			break;
		case PARASITE_CMD_DUMP_TTY:
			ret = parasite_dump_tty(args);
			break;
		default:
			pr_err("Unknown command in parasite daemon thread leader: %d\n", m.cmd);
			ret = -1;
			break;
		}

		if (__parasite_daemon_reply_ack(m.id, m.cmd, ret))
			break;
	}

	return oldstack;
}

static int noinline parasite_daemon(struct parasite_init_args *args)
{
	struct tid_state_s *s;
	unsigned long new_sp = 0;
	bool is_leader = false;

	s = find_thread_state(args->real);
	if (!s)
		return -ENOENT;

	if (s->real == thread_leader->real)
		is_leader = true;

	pr_info("Parasite entering daemon mode for %d\n", s->real);
	new_sp = (unsigned long)(void *)&s->stack[PARASITE_STACK_SIZE - 8];

	if (is_leader)
		call_daemon_thread(new_sp, args, s, __parasite_daemon_thread_leader);
	else
		call_daemon_thread(new_sp, args, s, __parasite_daemon_thread);

	pr_info("Parasite leaving daemon mode for %d\n", s->real);

	if (is_leader)
		sys_munmap(tid_state, TID_STATE_SIZE(nr_tid_state));

	asm_trap();
	return 0;
}

int __used parasite_service(unsigned int cmd, void *args)
{
	pr_info("Parasite cmd %d/%x process\n", cmd, cmd);

	switch (cmd) {
	case PARASITE_CMD_INIT:
		return init(args);
	case PARASITE_CMD_INIT_THREAD:
		return init_thread(args);
	case PARASITE_CMD_CFG_LOG:
		return parasite_cfg_log(args);
	case PARASITE_CMD_DAEMONIZE:
		return parasite_daemon(args);
	}

	pr_err("Unknown command to parasite: %d\n", cmd);
	return -EINVAL;
}
