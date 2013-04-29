/*
 * A simple testee program with threads
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <syscall.h>

#include "zdtmtst.h"

const char *test_doc	= "Create a few pthreads/forks and compare TLS and mmap data on restore\n";
const char *test_author	= "Cyrill Gorcunov <gorcunov@openvz.org";

static task_waiter_t waiter;

static void *thread_func_1(void *map)
{
	task_waiter_complete(&waiter, 1);
	task_waiter_wait4(&waiter, 2);

	return NULL;
}


int main(int argc, char *argv[])
{
	pthread_t th;
	int ret;

	test_init(argc, argv);

	task_waiter_init(&waiter);

	ret = pthread_create(&th, NULL, &thread_func_1, NULL);

	if (ret) {
		fail("Can't pthread_create");
		exit(1);
	}

	test_msg("Waiting until all threads are ready\n");

	task_waiter_wait4(&waiter, 1);

	test_daemon();
	test_waitsig();

	task_waiter_complete(&waiter, 2);

	pthread_join(th, NULL);

	pass();

	return 0;
}
