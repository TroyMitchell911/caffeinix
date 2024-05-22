#include "user.h"

void ls(char *path)
{
        int fd;

        fd = open(path, 0);
        if(fd < 0) {
                fprintf(2, "ls: open %s failed\n", path);
                return;
        }
        printf("ls: open %s successful\n", path);
        close(fd);
}

int main(void)
{
        ls(".");
        return 0;
}