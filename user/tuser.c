/*
 * @Author: TroyMitchell
 * @Date: 2024-05-19
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-06-01
 * @FilePath: /caffeinix/user/tuser.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */

#include "user.h"
#include "fcntl.h"
#include "environ.h"

int main(void)
{
        printf("test user program!\n");

        init_env();
        
        setenv("test2", "test2");
        setenv("test1", "test1");

        char *test1 = getenv("test1");
        char *test2 = getenv("test2");

        printf("test1: %s test2:%s\n", test1, test2);
        return 0;
}
