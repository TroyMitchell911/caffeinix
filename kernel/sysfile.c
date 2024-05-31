/*
 * @Author: TroyMitchell
 * @Date: 2024-05-07
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-31
 * @FilePath: /caffeinix/kernel/sysfile.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <fs.h>
#include <inode.h>
#include <log.h>
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
#include <vm.h>
#include <myfcntl.h>

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
        inode_t dp, ip;
        char name[DIRSIZ];
        dp = nameiparent(path, name);
        if(!dp) {
                return 0;
        }

        ilock(dp);
        ip = dirlookup(dp, name, 0);
        /* If we have found this name in the dirent */
        if(ip) {
                iunlockput(dp);
                ilock(ip); 
                /* If we wanna create a file and this existing name is file or device so we can return this inode */
                if(type == T_FILE && (ip->d.type == T_FILE || ip->d.type == T_DEVICE))
                        return ip;
                iunlockput(ip);
                return 0;
        }

        ip = ialloc(dp->dev, type);
        if(!ip) {
                iunlockput(dp);
                return 0;
        }

        ilock(ip);
        ip->d.nlink = 1;
        if(type == T_DEVICE) {
                ip->d.major = major;
                ip->d.minor = minor;
        }
        iupdate(ip);

        /* Create the local reference and the previous reference */
        if(type == T_DIR) {
                if(dirlink(ip, ".", ip->inum) ||
                   dirlink(ip, "..", dp->inum)) {
                        goto fail;
                }
        }

        if(dirlink(dp, name, ip->inum) != 0)
                goto fail;

        iunlockput(dp);
        return ip;

fail:
        ip->d.nlink = 0;
        iupdate(ip);
        iunlockput(dp);
        iunlockput(ip);
        return 0;
}

uint64 sys_dup(void)
{
        file_t f;
        int fd;

        argint(0, &fd);

        f = cur_proc()->ofile[fd];
        if(f) {
                /* Alloc a fd number */
                fd = fdalloc(f);
                if (fd < 0) {
                        return -1;
                }
                /* Execute this function 'file_dup' to increase reference of file descriptor*/
                file_dup(f);
                return fd;
        }
        return -1;
}

uint64 sys_getpid(void)
{
        return cur_proc()->pid;
}

uint64 sys_open(void)
{
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
        if(flag & O_CREATE) {
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
                if(ip->d.type == T_DIR && flag != O_RDONLY)
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

        if(ip->d.type == T_DEVICE) {
                f->type = FD_DEVICE;
                f->major = ip->d.major;
        } else {
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

uint64 sys_mknod(void)
{
        int ret = -1;
        char path[MAXPATH];
        int major, minor;
        inode_t ip;

        log_begin();

        /* Get major and minor from user */
        argint(1, &major);
        argint(2, &minor);
        if(major < 0 || minor < 0)
                goto fail;
        /* Get the path from user */
        if(argstr(0, path, MAXPATH) == -1)
                goto fail;
        /* Create a device file that be filled major and minor */
        ip = create(path, T_DEVICE, major, minor);
        if(!ip)
                goto fail;
        /* Bcs the function 'create' will return a inode that be locked and got */
        iunlockput(ip);
        ret = 0;
fail:
        log_end();
        return ret;
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

uint64 sys_write(void)
{
        file_t f;
        int fd, n;
        uint64 src;

        argint(0, &fd);
        argint(2, &n);
        argaddr(1, &src);

        f = cur_proc()->ofile[fd];
        if(f)
                return file_write(f, src, n); 
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
        cur_proc()->ofile[fd] = 0;
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

        for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
                pfree(argv[i]);

        return ret;
fail:
        for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
                pfree(argv[i]);
        return -1;
}
/*
Added a system call function sys_mkdir
test in user/init.c, but it's not succeed

2024-05-15 create by GoKo-Son626 
2024-05-15 fix by TroyMitchell
*/
uint64 sys_mkdir(void)
{
        int ret;
        char path[MAXPATH];
        struct inode *ip;

        log_begin();
        ret = argstr(0, path, MAXPATH);
        if(ret < 0) 
                goto fail;
        ip = create(path, T_DIR, 0, 0);
        if(ip == 0)
                goto fail;
        iunlockput(ip);
        log_end();
        return 0;
fail:
        log_end();
        return -1;
}

/**
 * @description: Added a system call function "file_stat"
 * @return {0}
 */
uint64 sys_fstat(void)
{
        file_t f;
        int fd;
        uint64 st;

        argint(0, &fd);
        argaddr(1, &st);

        f = cur_proc()->ofile[fd];
        if (f) {
                return file_stat(f, st);
        }
        return -1;
}

uint64 sys_sbrk(void)
{
        uint64 addr;
        int n, ret;
        
        argint(0, &n);
        addr = cur_proc()->sz;
        ret = process_grow(n);
        if(ret != 0)
                return -1;
        return addr;
}

/**
 * @description: Changing the working directory of current process
 * @return {int}: Returns 0 if the working directory was changed successfully, or 1 if not
 */
uint64 sys_chdir(void)
{
        char path[MAXPATH];
        inode_t ip;
        process_t p = cur_proc();

        log_begin();
        if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
                log_end();
                return -1;
        }
        ilock(ip);
        if (ip->d.type != T_DIR) {
                iunlock(ip);
                log_end();
                return -1;
        }
        iunlock(ip);
        iput(p->cwd);
        log_end();
        p->cwd = ip;
        if(path[0] == '/') {
                safe_strncpy(p->cwd_name, path, MAXPATH);
        }
        else {
                if(p->cwd_name[0] != '/')
                        strcat(p->cwd_name, "/");
                strcat(p->cwd_name, path);
        }
                
        return 0;
}

/**
 * @description: Create the path new as a link to the same inode as old_path
 * @return {int}: Returns 0 on successful creation and -1 on failure
 */
uint64 sys_link(void)
{
        char name[DIRSIZ], new_path[MAXPATH], old_path[MAXPATH];
        inode_t dp, ip;
        int ret;

        ret = argstr(0, old_path, MAXPATH);
        if(ret < 0)
                return -1;
        ret = argstr(1, new_path, MAXPATH);
        if(ret < 0)
                return -1;

        log_begin();

        ip = namei(old_path);
        if (!ip)
                goto r0;

        ilock(ip);

        if (ip->d.type == T_DIR)
                goto r1;

        ip->d.nlink++;
        iupdate(ip);
        iunlock(ip);

        dp = nameiparent(new_path, name);

        if (!dp) {
              goto bad;  
        }

        ilock(dp);

        if (dp->dev != ip->dev || dirlink(dp, name, ip->inum)) {
                iunlockput(dp);
                goto bad;
        }
        iunlockput(dp);
        iput(ip);
        log_end();

        return 0;

bad:
        ilock(ip);
        ip->d.nlink--;
        iupdate(ip);
r1:
        iunlockput(ip);
r0:
        log_end();
        return -1;
}

static int isdirempty(struct inode *dp)
{
        int off;
        struct dirent de;

        for(off=2*sizeof(de); off<dp->d.size; off+=sizeof(de)) {
                if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
                        panic("isdirempty: readi");
                if(de.inum != 0)
                        return 0;
        }
        
        return 1;
}

uint64 sys_unlink(void)
{
        int ret1,ret2;
        inode_t ip, dp;
        struct dirent de;
        char name[DIRSIZ], path[MAXPATH];
        uint off;

        ret1 = argstr(0, path, MAXPATH);
        if(ret1 < 0)
                return -1;

        log_begin();
        dp = nameiparent(path, name);
        if(dp == 0) {
                log_end();
                return -1;
        }

        ilock(dp);

        ret1 = strncmp(name, ".", DIRSIZ);
        ret2 = strncmp(name, "..", DIRSIZ);
        if(ret1 == 0 || ret2 == 0)
                goto bad;

        ip = dirlookup(dp, name, &off);
        if(ip == 0)
                goto bad;
        ilock(ip);

        if(ip->d.nlink < 1)
                panic("unlink: nlink < 1");
        if(ip->d.type == T_DIR && !isdirempty(ip)) {
                iunlockput(ip);
                goto bad;
        }

        memset(&de, 0, sizeof(de));
        ret1 = writei(dp, 0, (uint64) & de, off, sizeof(de));
        if(ret1 != sizeof(de))
                panic("unlink: writei");
        if(ip->d.type == T_DIR) {
                dp->d.nlink--;
                iupdate(dp);
        }
        iunlockput(dp);

        ip->d.nlink--;
        iupdate(ip);
        iunlockput(ip);
        log_end();
        return 0;
bad:
        iunlockput(dp);
        log_end();

        return -1;
}
