#include "user.h"
#include "stat.h"

void ls(char *path)
{
        int fd, ret;
        struct stat st;

        fd = open(path, 0);
        if(fd < 0) {
                fprintf(2, "ls: open %s failed\n", path);
                return;
        }

        ret = fstat(fd, &st);
        if(ret == -1) {
                fprintf(2, "ls: fstat %s failed\n", path);
                goto r1;
        }

        switch(st.type) {
                case T_DEVICE:
                case T_FILE:
                        printf("%s %d %d %d\n", path, st.type, st.ino, st.size);
                        break;
        }
r1:
        close(fd);
}

int main(void)
{
        ls(".");
        return 0;
}