#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>

#define STACK_SIZE	(8 * 4096)

static int ac;
static char **av;
static int ns_exec(void *_arg)
{
	execvp(av[1], av + 1);
	return 1;
}

int main(int argc, char **argv)
{
	void *stack;
	int ret;
	pid_t pid;

	ac = argc;
	av = argv;

	stack = mmap(NULL, STACK_SIZE, PROT_WRITE | PROT_READ,
			MAP_PRIVATE | MAP_GROWSDOWN | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) {
		fprintf(stderr, "Can't map stack %m\n");
		exit(1);
	}
	pid = clone(ns_exec, stack + STACK_SIZE,
			CLONE_NEWPID | CLONE_NEWIPC | SIGCHLD, NULL);
	if (pid < 0) {
		fprintf(stderr, "clone() failed: %m\n");
		exit(1);
	}
	return 0;
}
