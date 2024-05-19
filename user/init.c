/*
 * @Author: TroyMitchell
 * @Date: 2024-05-08
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-19
 * @FilePath: /caffeinix/user/init.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include "user.h"
#include "fcntl.h"
#include "stat.h"

#define TEST_USER               1

#define CONSOLE                 1 

char *argv[] = {"sh", 0};

int main(void){
        int pid, fd, ret;

        if((fd = open("console", O_RDWR)) == -1) {
                if((mknod("console", 1, 0)) == 0) {
                        fd = open("console", O_RDWR);       
                }
        }
        if(fd != -1) {
                fd = dup(fd);
                fd = dup(fd);
        } else {
                printf("open console failed\n");
                exit(-1);
        }     

#if TEST_USER
        pid = fork();
        if(pid == 0) {
                char *targv[] = {"tuser", 0};
                exec("tuser", targv);
                exit(0);
        } else if(pid != -1) {
                wait(0);
        } else {
                exit(-1);
        }
#endif

        for(;;) {
                pid = fork();

                if(pid == -1) {
                        printf("fork faild\n");
                        exit(-1);
                } else if(pid == 0) {
                        exec("sh", argv);
                        printf("exec sh failed\n");
                        exit(-1);
                }
                        
                for(;;) {
                        ret = wait(0);
                        if(ret == pid) {
                                printf("sh exited\n");
                                sleep(2);
                                break;
                        } else if(ret == -1) {
                                printf("wait failed\n");
                                exit(-1);
                        } else {
                                printf("parentless\n");
                                /* Don't do anything: parentless */
                        }
                }
        }
}