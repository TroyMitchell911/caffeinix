/*
 * @Author: TroyMitchell
 * @Date: 2024-05-07
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-08
 * @FilePath: /caffeinix/kernel/sysfile.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <sysfile.h>
#include <inode.h>
#include <file.h>
#include <dirent.h>
#include <scheduler.h>
#include <process.h>
#include <syscall.h>
#include <mystring.h>
#include <palloc.h>
#include <debug.h>

extern int exec(char* path, char** argv);

static int fdalloc(file_t f)
{
        int fd;
        process_t p = cur_proc();

        for(fd = 0; fd < NOFILE; fd++) {
                if(p->ofile[fd] == 0) {
                        p->ofile[fd] = f;
                        return fd;
                }
        }
        return -1;
}

static inode_t create(char* path, short type, short major, short minor)
{
        return 0;
}

uint64 sys_open(void)
{
        PANIC("sys_open");
        inode_t ip;
        char path[MAXPATH];
        int fd, flag;
        file_t f;

        argint(1, &flag);
        if(argstr(0, path, MAXPATH) < 0) {
                return -1;
        }

        log_begin();
        /* If the caller need to create and open a file */
        if(flag & O_CREAT) {
                ip = create(path, T_FILE, 0, 0);
                /* Create failed */
                if(ip == 0)
                        goto fail1;      
        } else{
                /* The caller just wants to open the file */
                ip = namei(path);
                if(ip == 0)
                        goto fail1;
                ilock(ip);
                /* Caffeinix doesn't allow user to open a dirent through the syscall 'open' */
                if(ip->d.type == T_DIR)
                        goto fail2;
        }
        /* Check the major of device */
        if(ip->d.type == T_DEVICE && (ip->d.major < 0))
                goto fail2;
        
        if((f = file_alloc()) == 0 || (fd = fdalloc(f)) < 0) {
                if(f) {
                        file_close(f);
                }
                goto fail2;
        }

        if(ip->d.type == T_FILE) {
                f->type = FD_INODE;
                f->off = 0;
        }

        f->ip = ip;
        f->readable = !(flag & O_WRONLY);
        f->writable = (flag & O_WRONLY) || (flag & O_RDWR);
        if((flag & O_TRUNC) && ip->d.type == T_FILE) {
                itrunc(ip);
        }

        iunlock(ip);
        log_end();

        return fd;

fail2:
        iunlockput(ip);
fail1:
        log_end();
        return -1;
}

uint64 sys_read(void)
{
        file_t f;
        int fd, n;
        uint64 dst;

        argint(0, &fd);
        argint(2, &n);
        argaddr(1, &dst);

        f = cur_proc()->ofile[fd];
        if(f)
                return file_read(f, dst, n); 
        return -1;
}

uint64 sys_close(void)
{
        file_t f;
        int fd;

        argint(0, &fd);

        f = cur_proc()->ofile[fd];
        if(!f)
                return -1;
        file_close(f);
        return 0;
}

uint64 sys_exec(void)
{
        char path[MAXPATH], *argv[MAXARG];
        int i, ret = 0;
        uint64 uargv, uarg;
        /* Get parameters */
        argaddr(1, &uargv);
        if(argstr(0, path, MAXPATH) < 0) {
                return -1;
        }
        /* Clear buffer */
        memset(argv, 0, sizeof(argv));

        for(i = 0; ;i++) {
                if(i > NELEM(argv))
                        goto fail;
                if(fetch_addr_from_user(uargv + i * sizeof(uint64), &uarg) < 0)
                        goto fail;
                if(uarg == 0)
                        break;
                argv[i] = palloc();
                if(argv[i] == 0)
                        goto fail;
                if(fetch_str_from_user(uarg, argv[i], PGSIZE) < 0)
                        goto fail;
        }

        ret = exec(path, argv);

        // for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        //         pfree(argv[i]);

        return ret;
fail:
        for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
                pfree(argv[i]);
        return -1;
}