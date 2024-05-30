/*
 * @Author: TroyMitchell
 * @Date: 2024-05-19
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-30
 * @FilePath: /caffeinix/user/tuser.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */

#include "user.h"
#include "fcntl.h"

int main(void)
{
        char buf[1024];
        int ret;
        printf("test user program!\n");
        mkdir("test");
        chdir("/test");
        ret = getcwd(buf, 1024);
        if(ret) {
                printf("getcwd failed!\n");
        } else {
                printf("getcwd successful: %s\n", buf);
        }
        return 0;
}
