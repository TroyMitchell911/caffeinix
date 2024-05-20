/*
 * @Author: TroyMitchell
 * @Date: 2024-05-19
 * @LastEditors: GoKo-Son626
 * @LastEditTime: 2024-05-20
 * @FilePath: /caffeinix/user/tuser.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */

#include "user.h"

int main(void)
{
        int ret;
        printf("test user program!\n");
rerun:
        ret = chdir("test_dir");
        if(ret != 0) {
                printf("change directory to 'test_dir' failed\n");
                ret = mkdir("test_dir");
                if(ret != 0) {
                        printf("create directory 'test_dir' failed\n");
                } else {
                        printf("create directory 'test_dir' successful\n");
                        goto rerun;
                }
        } else {
                printf("change directory to 'test_dir' successful\n");
        }
        return 0;
}
