/*
 * @Author: TroyMitchell
 * @Date: 2024-05-08
 * @LastEditors: GoKo-Son626
 * @LastEditTime: 2024-05-16
 * @FilePath: /caffeinix/user/init.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include "user.h"
#include "../kernel/include/myfcntl.h"
#include "stat.h"

#define CONSOLE                 1  
int main(void){
        int ret, fd;
        char buf[128];

        char* test = malloc(128);
        strcpy(test, "hello,world\n");

        fd = open("console", O_RDWR);
        if(fd == -1) {
                ret = mknod("console", 1, 0);
                if(ret == 0) {
                        fd = open("console", O_RDWR);
                }
        }
        if(fd != -1)
                fd = dup(fd);

        /*For mkdir test*/
        const char *directory_name = "dir_name";
        if (mkdir(directory_name) == -1) {
                printf("mkdir error\n");
        }else {
                printf("mkdir created successfully.\n");
        }

        /*for fstat test*/
        struct stat st;
        if (fstat(fd, &st) == -1) {  
                printf("fstat error\n");
        }else {
        
              printf("fstat successfully\n");
        }
        printf("%s", test);

        for(;;) {
                if(fd != -1) {
                        ret = read(fd, buf, 128);
                        buf[ret] = '\0';
                        if(ret != 0) {
                                fprintf(fd, "From user: %s", buf);
                        }
                }  
        }

        return 0;
}