/*
 * @Author: TroyMitchell
 * @Date: 2024-05-07
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-14
 * @FilePath: /caffeinix/kernel/exec.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <elf.h>
#include <log.h>
#include <inode.h>
#include <dirent.h>
#include <scheduler.h>
#include <vm.h>
#include <debug.h>
#include <mystring.h>
#include <printf.h>

static int flags2perm(int flags)
{
        int perm = 0;
        if(flags & 0x1)
                perm = PTE_X;
        if(flags & 0x2)
                perm |= PTE_W;
        return perm;
}

static int loadseg(pagedir_t pgdir, uint64 va, inode_t ip, uint32 offset, uint32 sz)
{
        uint32 i, n;
        uint64 pa;

        for(i = 0; i < sz; i += PGSIZE) {
                pa = va2pa(pgdir, va + i);
                if(pa == 0)
                        PANIC("load_seg");
                if(sz - i < PGSIZE)
                        n = sz - i;
                else
                        n = PGSIZE;
                if(readi(ip, 0, pa, offset + i, n) != n)
                        return -1;
        }
        return 0;
}

int exec(char* path, char** argv)
{
        int i, off;
        uint64 argc, oldsz, sz = 0, sz1, sp, ustack[MAXARG], stackbase;
        inode_t ip;
        struct elfhdr elf;
        struct proghdr ph;
        pagedir_t oldpgdir, pgdir = 0;
        process_t p = cur_proc();   
        
        log_begin();
        ip = namei(path);
        if(!ip) {
                log_end();
                return -1;
        }
        ilock(ip);
        /* Read elf head */
        if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) {
                goto fail;
        }
        /* Check magic number */
        if(elf.magic != ELF_MAGIC) 
                goto fail;
        /* 
                Create a page-table that has trapframe and trampoline.
                The trampoline follow previous process instead of new.
        */
        pgdir = process_pagedir(p);
        if(!pgdir)
                goto fail;
        printf("elf.phoff = %d; elf.phnum = %d\n", elf.phoff, elf.phnum);
        for(i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
                
                if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
                        goto fail;
                printf("ph.type = %d\n", ph.type);
                printf("ph.off = %x; ph.filesize:%x\n", ph.off, ph.filesz);
                if(ph.type != ELF_PROG_LOAD)
                        continue;
                        
                /* Overflow */
                /* 2024-05-09 Fixed a overflow bug by TroyMitchell */
                if(ph.vaddr + ph.memsz < ph.vaddr)
                        goto fail;
                if(ph.memsz < ph.filesz)
                        goto fail;
                /* Aligned? */
                if(ph.vaddr % PGSIZE != 0)
                        goto fail;
                /* Extend virtual address and alloc physical memory */
                printf("vm_alloc: %x->%x\n",sz, ph.vaddr + ph.memsz);
                if((sz1 = vm_alloc(pgdir, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
                        goto fail;
                sz = sz1;
                /* Load seg to memory */
                if(loadseg(pgdir, ph.vaddr, ip, ph.off, ph.memsz) != 0)
                        goto fail;
        }
        iunlockput(ip);
        log_end();

        sz = PGROUNDUP(sz);

        /* Alloc memory of stack */
        sz1 = vm_alloc(pgdir, sz, sz + PGSIZE * 2, PTE_W);
        if(sz1 == 0)
                goto fail;
        sz = sp = sz1;
        stackbase = sp - PGSIZE;
        /* Set guard page */
        vm_clear(pgdir, sp - PGSIZE * 2);

        for(argc = 0; argv[argc] != 0; argc++) {
                if(argc >= MAXARG)
                        goto fail;
                sp -= strlen(argv[argc]);
                /* riscv sp must be 16-byte aligned */
                sp -= sp % 16;
                if(sp < stackbase)
                        goto fail;
                if(copyout(pgdir, sp, argv[argc], strlen(argv[argc])) != 0)
                        goto fail;
                ustack[argc] = sp;
        }
        ustack[argc ++] = 0;

        sp -= argc * sizeof(uint64);
        /* riscv sp must be 16-byte aligned */
        sp -= sp % 16;
        if(sp < stackbase)
                goto fail;
        if(copyout(pgdir, sp, (char*)ustack, argc * sizeof(uint64)) != 0)
                goto fail;

        /* Save page-table */
        oldpgdir = p->pagetable;
        oldsz = p->sz;
        
        p->pagetable = pgdir;
        p->sz = sz;
        /* 
                Arguments to user main(argc, argv).
                We just set a1 bcs the a0 via return to set.
        */
        p->trapframe->a1 = sp;
        p->trapframe->sp = sp;
        p->trapframe->epc = elf.entry;
        process_freepagedir(oldpgdir, oldsz);
        
        /* Rid of the last element (ustack[argc ++] = 0;) */
        return --argc;
fail:
        if(pgdir) {
                process_freepagedir(pgdir, sz);
        }
        if(ip) {
                iunlockput(ip);
        }
        log_end();
        return -1;
}
