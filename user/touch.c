#include "user.h"
#include "fcntl.h"

int main(int argc, char *argv[])
{
        int fd;

        if(argc < 2 || strcmp(argv[1], "") == 0) {
                fprintf(2, "touch: missing file operand\n");
                return -1;
        }

        fd = open(argv[1], O_CREATE);
        if(fd < 0) {
                fprintf(2, "touch: create %s failed\n", argv[1]);
                return -1;
        } else {
                close(fd);
        }

        return 0;
}