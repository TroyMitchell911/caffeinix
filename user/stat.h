/*
 * @Author: TroyMitchell
 * @Date: 2024-05-14
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-14
 * @FilePath: /caffeinix/user/stat.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */

#include "../arch/riscv/include/typedefs.h"

#define T_DIR     1
#define T_FILE    2
#define T_DEVICE  3

struct stat {
        int dev;     
        uint ino;    
        short type;  
        short nlink; 
        uint64 size; 
};
