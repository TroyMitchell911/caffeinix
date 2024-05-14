/*
 * @Author: TroyMitchell
 * @Date: 2024-05-08
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-14
 * @FilePath: /caffeinix/user/init.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include "user.h"
#include "../kernel/include/myfcntl.h"

#define CONSOLE                 1  
int main(void){
        int ret, fd, buf2len, i, pid;
        char buf1[128] = "pid: ";
        char buf2[256] = "From user: ";
        buf2len = strlen(buf2);
        
        fd = open("console", O_RDWR);
        if(fd == -1) {
                ret = mknod("console", 1, 0);
                if(ret == 0) {
                        fd = open("console", O_RDWR);
                }
        }
        if(fd != -1)
                fd = dup(fd);

        /* For pid test */
        pid = getpid();
        i = strlen(buf1);
        buf1[i] = pid + '0';
        strcpy(&buf1[i+1], "\n");
        write(fd, buf1, strlen(buf1));
        for(;;) {
                if(fd != -1) {
                        ret = read(fd, buf1, 128);
                        for(i = buf2len; i < ret + buf2len; i++) {
                                buf2[i] = buf1[i - buf2len];
                        }
                        buf2[i] = '\0';
                        if(ret != 0) {
                                ret = write(fd, buf2, i);
                        }
                }  
        }
        return 0;
}