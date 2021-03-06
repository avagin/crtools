#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "crtools.h"
#include "cr_options.h"
#include "util.h"
#include "log.h"
#include "pstree.h"
#include "cr-service.h"
#include "cr-service-const.h"
#include "sd-daemon.h"

unsigned int service_sk_ino = -1;

static int recv_criu_msg(int socket_fd, CriuReq **msg)
{
	unsigned char buf[CR_MAX_MSG_SIZE];
	int len;

	len = read(socket_fd, buf, CR_MAX_MSG_SIZE);
	if (len == -1) {
		pr_perror("Can't read request");
		return -1;
	}

	*msg = criu_req__unpack(NULL, len, buf);
	if (!*msg) {
		pr_perror("Failed unpacking request");
		return -1;
	}

	return 0;
}

static int send_criu_msg(int socket_fd, CriuResp *msg)
{
	unsigned char buf[CR_MAX_MSG_SIZE];
	int len;

	len = criu_resp__get_packed_size(msg);

	if (criu_resp__pack(msg, buf) != len) {
		pr_perror("Failed packing response");
		return -1;
	}

	if (write(socket_fd, buf, len)  == -1) {
		pr_perror("Can't send response");
		return -1;
	}

	return 0;
}

int send_criu_dump_resp(int socket_fd, bool success, bool restored)
{
	CriuResp msg = CRIU_RESP__INIT;
	CriuDumpResp resp = CRIU_DUMP_RESP__INIT;

	msg.type = CRIU_REQ_TYPE__DUMP;
	msg.success = success;
	msg.dump = &resp;

	resp.has_restored = true;
	resp.restored = restored;

	return send_criu_msg(socket_fd, &msg);
}

int send_criu_restore_resp(int socket_fd, bool success, int pid)
{
	CriuResp msg = CRIU_RESP__INIT;
	CriuRestoreResp resp = CRIU_RESTORE_RESP__INIT;

	msg.type = CRIU_REQ_TYPE__RESTORE;
	msg.success = success;
	msg.restore = &resp;

	resp.pid = pid;

	return send_criu_msg(socket_fd, &msg);
}

static int setup_opts_from_req(int sk, CriuOpts *req)
{
	struct ucred ids;
	struct stat st;
	socklen_t ids_len = sizeof(struct ucred);
	char images_dir_path[PATH_MAX];

	if (getsockopt(sk, SOL_SOCKET, SO_PEERCRED, &ids, &ids_len)) {
		pr_perror("Can't get socket options");
		return -1;
	}

	restrict_uid(ids.uid, ids.gid);

	if (fstat(sk, &st)) {
		pr_perror("Can't get socket stat");
		return -1;
	}

	BUG_ON(st.st_ino == -1);
	service_sk_ino = st.st_ino;

	/* going to dir, where to place/get images*/
	sprintf(images_dir_path, "/proc/%d/fd/%d", ids.pid, req->images_dir_fd);

	if (chdir(images_dir_path)) {
		pr_perror("Can't chdir to images directory");
		return -1;
	}

	if (open_image_dir(".") < 0)
		return -1;

	/* initiate log file in imgs dir */
	if (req->log_file)
		opts.output = req->log_file;
	else
		opts.output = DEFAULT_LOG_FILENAME;

	log_set_loglevel(req->log_level);
	if (log_init(opts.output) == -1) {
		pr_perror("Can't initiate log");
		return -1;
	}

	/* checking flags from client */
	if (req->has_leave_running && req->leave_running)
		opts.final_state = TASK_ALIVE;

	if (!req->has_pid) {
		req->has_pid = true;
		req->pid = ids.pid;
	}

	if (req->has_ext_unix_sk)
		opts.ext_unix_sk = req->ext_unix_sk;

	if (req->has_tcp_established)
		opts.tcp_established_ok = req->tcp_established;

	if (req->has_evasive_devices)
		opts.evasive_devices = req->evasive_devices;

	if (req->has_shell_job)
		opts.shell_job = req->shell_job;

	if (req->has_file_locks)
		opts.handle_file_locks = req->file_locks;

	return 0;
}

static int dump_using_req(int sk, CriuOpts *req)
{
	bool success = false;
	bool self_dump = !req->pid;

	if (setup_opts_from_req(sk, req) == -1) {
		pr_perror("Arguments treating fail");
		goto exit;
	}

	/*
	 * FIXME -- cr_dump_tasks() may return code from custom
	 * scripts, that can be positive. However, right now we
	 * don't have ability to push scripts via RPC, so psitive
	 * ret values are impossible here.
	 */
	if (cr_dump_tasks(req->pid))
		goto exit;

	success = true;
exit:
	if (req->leave_running  || !self_dump) {
		if (send_criu_dump_resp(sk, success, false) == -1) {
			pr_perror("Can't send response");
			success = false;
		}
	}

	return success ? 0 : 1;
}

static int restore_using_req(int sk, CriuOpts *req)
{
	bool success = false;

	/*
	 * We can't restore processes under arbitrary task yet.
	 * Thus for now we force the detached restore under the
	 * cr service task.
	 */

	opts.restore_detach = true;

	if (setup_opts_from_req(sk, req) == -1) {
		pr_perror("Arguments treating fail");
		goto exit;
	}

	if (cr_restore_tasks())
		goto exit;

	success = true;
exit:
	if (send_criu_restore_resp(sk, success,
				   root_item ? root_item->pid.real : -1) == -1) {
		pr_perror("Can't send response");
		success = false;
	}

	return success ? 0 : 1;
}

static int check(int sk)
{
	CriuResp resp = CRIU_RESP__INIT;

	resp.type = CRIU_REQ_TYPE__CHECK;

	if (!cr_check())
		resp.success = true;

	return send_criu_msg(sk, &resp);
}

static int cr_service_work(int sk)
{
	CriuReq *msg = 0;

	init_opts();

	if (recv_criu_msg(sk, &msg) == -1) {
		pr_perror("Can't recv request");
		goto err;
	}

	switch (msg->type) {
	case CRIU_REQ_TYPE__DUMP:
		return dump_using_req(sk, msg->opts);
	case CRIU_REQ_TYPE__RESTORE:
		return restore_using_req(sk, msg->opts);
	case CRIU_REQ_TYPE__CHECK:
		return check(sk);

	default: {
		CriuResp resp = CRIU_RESP__INIT;

		resp.type = CRIU_REQ_TYPE__EMPTY;
		resp.success = false;
		/* XXX -- add optional error code to CriuResp */

		pr_perror("Invalid request");
		send_criu_msg(sk, &resp);

		goto err;
	}
	}

err:
	return -1;
}

static void reap_worker(int signo)
{
	int saved_errno;
	int status;
	pid_t pid;

	saved_errno = errno;

	/*
	 * As we block SIGCHLD, lets wait for every child that has
	 * already changed state.
	 */
	while (1) {
		pid = waitpid(-1, &status, WNOHANG);

		if (pid <= 0) {
			errno = saved_errno;
			return;
		}

		if (WIFEXITED(status))
			pr_info("Worker(pid %d) exited with %d\n",
				pid, WEXITSTATUS(status));
		else if (WIFSIGNALED(status))
			pr_info("Worker(pid %d) was killed by %d\n",
				pid, WTERMSIG(status));
	}
}

static int setup_sigchld_handler()
{
	struct sigaction action;

	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGCHLD);
	action.sa_handler	= reap_worker;
	action.sa_flags		= SA_RESTART;

	if (sigaction(SIGCHLD, &action, NULL)) {
		pr_perror("Can't setup SIGCHLD handler");
		return -1;
	}

	return 0;
}

static int restore_sigchld_handler()
{
	struct sigaction action;

	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGCHLD);
	action.sa_handler	= SIG_DFL;
	action.sa_flags		= SA_RESTART;

	if (sigaction(SIGCHLD, &action, NULL)) {
		pr_perror("Can't restore SIGCHLD handler");
		return -1;
	}

	return 0;
}

int cr_service(bool daemon_mode)
{
	int server_fd = -1, n;
	int child_pid;

	struct sockaddr_un client_addr;
	socklen_t client_addr_len;

	n = sd_listen_fds(0);
	if (n > 1) {
		pr_err("Too many file descriptors (%d) recieved", n);
		goto err;
	} else if (n == 1)
		server_fd = SD_LISTEN_FDS_START + 0;
	else {
		struct sockaddr_un server_addr;
		socklen_t server_addr_len;

		server_fd = socket(AF_LOCAL, SOCK_SEQPACKET, 0);
		if (server_fd == -1) {
			pr_perror("Can't initialize service socket");
			goto err;
		}

		memset(&server_addr, 0, sizeof(server_addr));
		memset(&client_addr, 0, sizeof(client_addr));
		server_addr.sun_family = AF_LOCAL;

		if (opts.addr == NULL)
			opts.addr = CR_DEFAULT_SERVICE_ADDRESS;

		strcpy(server_addr.sun_path, opts.addr);

		server_addr_len = strlen(server_addr.sun_path)
				+ sizeof(server_addr.sun_family);
		client_addr_len = sizeof(client_addr);

		unlink(server_addr.sun_path);

		if (bind(server_fd, (struct sockaddr *) &server_addr,
						server_addr_len) == -1) {
			pr_perror("Can't bind");
			goto err;
		}

		pr_info("The service socket is bound to %s\n", server_addr.sun_path);

		/* change service socket permissions, so anyone can connect to it */
		if (chmod(server_addr.sun_path, 0666)) {
			pr_perror("Can't change permissions of the service socket");
			goto err;
		}

		if (listen(server_fd, 16) == -1) {
			pr_perror("Can't listen for socket connections");
			goto err;
		}
	}

	if (daemon_mode) {
		if (daemon(1, 0) == -1) {
			pr_perror("Can't run service server in the background");
			goto err;
		}
	}

	if (opts.pidfile) {
		if (write_pidfile(getpid()) == -1) {
			pr_perror("Can't write pidfile");
			goto err;
		}
	}

	if (setup_sigchld_handler())
		goto err;

	while (1) {
		int sk;

		pr_info("Waiting for connection...\n");

		sk = accept(server_fd, &client_addr, &client_addr_len);
		if (sk == -1) {
			pr_perror("Can't accept connection");
			goto err;
		}

		pr_info("Connected.\n");
		child_pid = fork();
		if (child_pid == 0) {
			int ret;

			if (restore_sigchld_handler())
				exit(1);

			close(server_fd);
			ret = cr_service_work(sk);
			close(sk);
			exit(ret != 0);
		}

		if (child_pid < 0)
			pr_perror("Can't fork a child");

		close(sk);
	}

err:
	close_safe(&server_fd);

	return 1;
}
