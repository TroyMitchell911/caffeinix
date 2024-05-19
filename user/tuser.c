/*
 * @Author: TroyMitchell
 * @Date: 2024-05-19
 * @LastEditors: GoKo-Son626
 * @LastEditTime: 2024-05-19
 * @FilePath: /caffeinix/user/tuser.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */

#include "user.h"

int main(void)
{
        int result;
        printf("test user program!\n");

        /*Test for chdir*/
        char path[128] = "/path/directory";
        result = chdir(path);
        if (result == 0) {
                printf("Test successfully\n");
        } else {
                printf("Test faild\n");
        }

        return 0;
}