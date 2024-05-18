#include "user.h"
#include "fcntl.h"

#define SH_BUF_MAX                      128

static int getcmd(char* buf, int max)
{
        printf("$ ");
        gets(buf, max);
        if(buf[0] == 0)
                return -1;
        return 0;
}

int main(void)
{
        static char buf[SH_BUF_MAX];
        int fd;

        while((fd = open("console", O_RDWR)) >= 0) {
                if(fd == 3) {
                        close(fd);
                        break;
                }
        }
        if(fd != 3) {
                printf("sh failed: fd->%d\n", fd);
                exit(-1);
        }

        while(getcmd(buf, SH_BUF_MAX) >= 0) {

        }

        exit(-1);
        return 0;
}