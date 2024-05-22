/*
 * @Author: TroyMitchell
 * @Date: 2024-05-22
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-22
 * @FilePath: /caffeinix/user/dirent.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#define DIRSIZ 14

struct dirent {
        unsigned short inum;
        char name[DIRSIZ];
};