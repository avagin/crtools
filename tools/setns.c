#define _GNU_SOURCE
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define __NR_set_ns                             308

int main (int argc, char *argv[])
{
	int fd = open(argv[1], O_RDONLY);
	if (fd == -1)
		return 1;
	if (syscall(__NR_set_ns, fd, CLONE_NEWNET) == -1)
		return 1;
	close(fd);

	execvp(argv[2], argv + 2);
	return 1;
}

