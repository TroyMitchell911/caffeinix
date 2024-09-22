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
#include <getopt.h>

int main(int argc, char *argv[])
{
        int opt;

        printf("test user program!\n");

        while ((opt = getopt(argc, argv, "rf")) != -1) {
        switch (opt) {
            case 'r':
                printf("Option -a selected\n");
                break;
            case 'f':
                printf("Option -b selected\n");
                break;
            default:
                printf("Unknown option: %c\n", opt);
        }
    }
        return 0;
}
