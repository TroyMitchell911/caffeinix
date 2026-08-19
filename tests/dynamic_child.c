#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	int fd;

	if (argc != 2)
		return 1;
	if (!strcmp(argv[1], "pressure"))
		return 0;
	fd = atoi(argv[1]);
	errno = 0;
	if (fcntl(fd, F_GETFD) != -1 || errno != EBADF)
		return 1;
	puts("DYNAMIC_EXEC_OK");
	return 0;
}
