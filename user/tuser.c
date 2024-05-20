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
        const char *oldfile = "oldfile.txt";
        const char *newfile = "newfile.txt";
        printf("test user program!\n");

        int fd = open(oldfile, 0x200 | 0x001);
        if (fd < 0) {  
                printf("open oldfile.txt failed\n");  
                return 0;
        }else {
                printf("open oldfile.txt successfully\n");
        }

        int fd1 = open(newfile, 0x200 | 0x001);
        if (fd1 < 0) {  
                printf("open newfile.txt failed\n");  
                return 0;
        }else {
                printf("open newfile.txt successfully\n");
        }

        ret = link(oldfile, newfile);
        if (ret != 0) {
                printf("link fail \n");
        } else {
                printf("link successful \n");
        }

        return 0;
}
