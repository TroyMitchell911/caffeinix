#include "user.h"
#include "fcntl.h"

#define TAG                     "cp: "

static int cp(char *src, char *dst)
{
        int sfd, dfd, n;
        char buf[1024];
        sfd = open(src, O_RDONLY);
        if(!sfd) {
                printf(TAG"cannot stat '%s': No such file or directory\n", src);
                goto r0;
        }

        dfd = open(dst, O_WRONLY | O_CREATE | O_TRUNC);
        if(!dfd) {
                printf(TAG"cannot create '%s'\n", dst);
                goto r1;
        }

        while((n = read(sfd, buf, 1024)) > 0) {
                if((n = write(dfd, buf, 1024)) < 0) {
                        printf(TAG"write '%s' failed\n", dst);
                        goto r2;
                }
        }
        
        close(sfd);
        close(dfd);
        return 0;

r2:
        close(dfd);
r1:    
        close(sfd);
r0:
        return -1;
}

int main(int argc, char **argv)
{
        if(argc < 3) {
                printf(TAG"missing operand\n");
                return -1;
        }

        if(cp(argv[1], argv[2]) < 0)
                return -1;
        
        return 0;
}