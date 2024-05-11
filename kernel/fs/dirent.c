/*
 * @Author: TroyMitchell
 * @Date: 2024-05-07
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-07
 * @FilePath: /caffeinix/kernel/fs/dirent.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <dirent.h>
#include <debug.h>
#include <mystring.h>
#include <scheduler.h>

int namecmp(const char *s, const char *t)
{
        return strncmp(s, t, DIRSIZ);
}

inode_t dirlookup(inode_t ip, char *name, uint32 *poff)
{
        uint32 off, inum;
        struct dirent de;

        if(ip->d.type != T_DIR)
                PANIC("dirlookup not DIR");

        for(off = 0; off < ip->d.size; off += sizeof(de)){
  	        if(readi(ip, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
                        PANIC("dirlookup read");
                if(de.inum == 0)
                        continue;
                if(namecmp(name, de.name) == 0){
                        if(poff)
                                *poff = off;
                        inum = de.inum;
                        return iget(ip->dev, inum);
                }
        }

        return 0;
}

int dirlink(struct inode *dp, char *name, uint32 inum)
{
        int off;
        struct dirent de;
        struct inode *ip;

        // Check that name is not present.
        if((ip = dirlookup(dp, name, 0)) != 0) {
                iput(ip);
                return -1;
        }

        // Look for an empty dirent.
        for(off = 0; off < dp->d.size; off += sizeof(de)) {
                if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
                        panic("dirlink read");
                if(de.inum == 0)
                        break;
        }
        strncpy(de.name, name, DIRSIZ);
        
        de.inum = inum;
        if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
                return -1;
        return 0;
}

static char* skipelem(char *path, char *name)
{
        char *s;
        int len;

        while(*path == '/')
                path++;
        if(*path == 0)
                return 0;
        s = path;
        while(*path != '/' && *path != 0)
                path++;
        len = path - s;
        if(len >= DIRSIZ)
                memmove(name, s, DIRSIZ);
        else {
                memmove(name, s, len);
                name[len] = 0;
        }
        while(*path == '/')
                path++;
        return path;
}

static inode_t namex(char *path, int nameiparent, char *name)
{
        struct inode *ip, *next;
        
        if(*path == '/')
                ip = iget(ROOTDEV, ROOTINO);
        else
                ip = idup(cur_proc()->cwd);

        while((path = skipelem(path, name)) != 0){
                ilock(ip);
                if(ip->d.type != T_DIR){
                        iunlockput(ip);
                        return 0;
                }
                if(nameiparent && *path == '\0'){
                        // Stop one level early.
                        iunlock(ip);
                        return ip;
                }
                if((next = dirlookup(ip, name, 0)) == 0){
                        iunlockput(ip);
                        return 0;
                }
                iunlockput(ip);
                ip = next;
        }
        if(nameiparent){
                iput(ip);
                return 0;
        }
        return ip;
}

inode_t namei(char *path)
{
        char name[DIRSIZ];
        return namex(path, 0, name);
}

inode_t nameiparent(char *path, char *name)
{
        return namex(path, 1, name);
}