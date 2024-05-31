/*
 * @Author: TroyMitchell
 * @Date: 2024-05-31
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-31
 * @FilePath: /caffeinix/user/cat.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include "user.h"
#include "fcntl.h"

#define TAG                     "cat: "

int cat(int fd, const char *path)
{
        int n;
        char buf[1024];
        
        while((n = read(fd, buf, 1024)) > 0) {
                if(write(1, buf, n) < 0) {
                        fprintf(2, TAG"%s: Read error\n", path);
                        return -1;
                }
        }
        if(n < 0) {
                fprintf(2, TAG"%s: Read error\n", path);
                return -1;
        }

        return 0;
}

int main(int argc, char **argv)
{
        int i, fd;
        int ret;

        if(argc < 2) {
                printf(TAG"missing operand\n");
                return -1;
        }

        for(i = 1; i < argc; i++) {
                fd = open(argv[i], O_RDONLY);
                if(fd == -1) {
                        printf(TAG"cat: %s: No such file or directory", argv[i]);
                        return -1;
                }
                ret = cat(fd, argv[i]);
                close(fd);
                if(ret)
                        return -1;
        }
        
        return 0;
}