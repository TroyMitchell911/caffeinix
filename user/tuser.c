/*
 * @Author: TroyMitchell
 * @Date: 2024-05-19
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-22
 * @FilePath: /caffeinix/user/tuser.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */

#include "user.h"
#include "fcntl.h"

int main(void)
{
        int ret;
        const char *oldfile = "oldfile.txt";
        const char *newfile = "newfile.txt";
        printf("test user program!\n");

        int fd = open(oldfile, O_CREAT);
        if (fd < 0) {  
                printf("open oldfile.txt failed\n");  
                return 0;
        } else {
                printf("open oldfile.txt successfully\n");
        }

        ret = link(oldfile, newfile);
        if (ret != 0) {
                printf("link fail \n");
        } else {
                printf("link successful \n");
        }

        int fd1 = open(oldfile, 0x002);
        if (fd1 < 0) {  
                printf("open newfile.txt failed\n");  
                return 0;
        } else {
                printf("open newfile.txt successfully\n");
        }

        /*Test for unlink*/
        printf("\n");
        int fd2 = unlink(newfile);
        if (fd2 != 0) {
                printf("unlink %s faild\n", newfile);
        } else {
                printf("unlink %s successfully\n", newfile);
        }

        fd1 = open(newfile, 0x002);
        if (fd1 < 0) {  
                printf("After unlink, open newfile.txt failed\n");  
                return 0;
        } else {
                printf("After unlink, open newfile.txt successfully\n");
        }

        return 0;
}
