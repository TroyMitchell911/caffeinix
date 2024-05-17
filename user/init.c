/*
 * @Author: TroyMitchell
 * @Date: 2024-05-08
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-17
 * @FilePath: /caffeinix/user/init.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include "user.h"
#include "fcntl.h"
#include "stat.h"

#define CONSOLE                 1  

int _test_thread(void* arg);

int main(void){
        int ret, fd;

        if((fd = open("console", O_RDWR)) == -1) {
                if((mknod("console", 1, 0)) == 0) {
                        fd = open("console", O_RDWR);       
                }
        }
        if(fd != -1)
                fd = dup(fd);

        clone(_test_thread, 0, 0, "_test_thread1");
        clone(_test_thread, 0, 0, "_test_thread2");
        clone(_test_thread, 0, 0, "_test_thread3");
        clone(_test_thread, 0, 0, "_test_thread4");
        clone(_test_thread, 0, 0, "_test_thread5");

        ret = fork();
        if(ret == 0)
                printf("child\n");
        else
                printf("parent\n");

        for(;;);
        // for(;;) {
        //         if(fd != -1) {
        //                 ret = read(fd, buf, 128);
        //                 buf[ret] = '\0';
        //                 if(ret != 0) {
        //                         fprintf(fd, "From user: %s", buf);
        //                 }
        //         }  
        // }

        return 0;
}

int _test_thread(void* arg)
{
        printf("%s\n", (char*)arg);
        while(1);
        return 0;
}