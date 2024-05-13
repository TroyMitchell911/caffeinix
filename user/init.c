/*
 * @Author: TroyMitchell
 * @Date: 2024-05-08
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-13
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
        char buf1[128];
        char buf2[256];
        buf2[0] = 'F';
        buf2[1] = 'r';
        buf2[2] = 'o';
        buf2[3] = 'm';
        buf2[4] = ' ';
        buf2[5] = 'u';
        buf2[6] = 's';
        buf2[7] = 'e';
        buf2[8] = 'r';
        buf2[9] = ':';
        buf2[10] = ' ';
        buf2[11] = '\0';
        buf2len = 11;
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
        buf1[0] = 'p';
        buf1[1] = 'i';
        buf1[2] = 'd';
        buf1[3] = ':';
        buf1[4] = pid + '0';
        buf1[5] = '\n';
        write(fd, buf1, 6);
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