#include <debug.h>
#include <dirent.h>
#include <elf.h>
#include <inode.h>
#include <linux_uapi.h>
#include <log.h>
#include <mystring.h>
#include <printf.h>
#include <scheduler.h>
#include <vm.h>

struct linux_auxv_entry {
	uint64 type;
	uint64 value;
};

static int flags2perm(int flags)
{
	int perm = 0;

	if (flags & ELF_PROG_FLAG_EXEC)
		perm |= PTE_X;
	if (flags & ELF_PROG_FLAG_WRITE)
		perm |= PTE_W;
	return perm;
}

static int loadseg(pagedir_t pgdir, uint64 va, inode_t ip,
		   uint64 offset, uint64 size)
{
	uint64 page_offset, pa, n;

	while (size) {
		pa = va2pa(pgdir, va);
		if (!pa)
			return -1;

		page_offset = va & (PGSIZE - 1);
		n = PGSIZE - page_offset;
		if (n > size)
			n = size;
		if (readi(ip, 0, pa + page_offset, offset, n) != n)
			return -1;

		va += n;
		offset += n;
		size -= n;
	}
	return 0;
}

static int build_linux_stack(pagedir_t pgdir, uint64 stack_top,
			     uint64 stack_base, char **argv, char **envp,
			     uint64 phdr, struct elfhdr *elf,
			     uint64 *new_sp)
{
	uint64 argv_address[MAXARG];
	uint64 envp_address[MAXARG];
	uint64 words[4 * MAXARG + 32];
	uint64 random[2];
	uint64 sp = stack_top;
	uint64 length;
	int argc, envc, nwords = 0;

	for (argc = 0; argv[argc]; argc++) {
		if (argc >= MAXARG - 1)
			return -1;
		length = strlen(argv[argc]) + 1;
		sp -= length;
		if (sp < stack_base ||
		    copyout(pgdir, sp, argv[argc], length) < 0)
			return -1;
		argv_address[argc] = sp;
	}
	for (envc = 0; envp[envc]; envc++) {
		if (envc >= MAXARG - 1)
			return -1;
		length = strlen(envp[envc]) + 1;
		sp -= length;
		if (sp < stack_base ||
		    copyout(pgdir, sp, envp[envc], length) < 0)
			return -1;
		envp_address[envc] = sp;
	}

	random[0] = 0x4341464645494e49ULL;
	random[1] = 0x582d4d55534c2d58ULL;
	sp -= sizeof(random);
	if (sp < stack_base ||
	    copyout(pgdir, sp, (char *)random, sizeof(random)) < 0)
		return -1;
	length = sp;

	words[nwords++] = argc;
	for (int i = 0; i < argc; i++)
		words[nwords++] = argv_address[i];
	words[nwords++] = 0; /* argv terminator */
	for (int i = 0; i < envc; i++)
		words[nwords++] = envp_address[i];
	words[nwords++] = 0; /* envp terminator */

#define AUX(tag, value) do { \
	words[nwords++] = (tag); \
	words[nwords++] = (value); \
} while (0)
	AUX(LINUX_AT_PHDR, phdr);
	AUX(LINUX_AT_PHENT, sizeof(struct proghdr));
	AUX(LINUX_AT_PHNUM, elf->phnum);
	AUX(LINUX_AT_PAGESZ, PGSIZE);
	AUX(LINUX_AT_ENTRY, elf->entry);
	AUX(LINUX_AT_UID, 0);
	AUX(LINUX_AT_EUID, 0);
	AUX(LINUX_AT_GID, 0);
	AUX(LINUX_AT_EGID, 0);
	AUX(LINUX_AT_SECURE, 0);
	AUX(LINUX_AT_RANDOM, length);
	AUX(LINUX_AT_EXECFN, argc ? argv_address[0] : 0);
	AUX(LINUX_AT_NULL, 0);
#undef AUX

	length = nwords * sizeof(uint64);
	sp = (sp - length) & ~15ULL;
	if (sp < stack_base || copyout(pgdir, sp, (char *)words, length) < 0)
		return -1;

	*new_sp = sp;
	return argc;
}

int exec_linux(char *path, char **argv, char **envp)
{
	int argc, i, logged = 0;
	uint64 off, oldsz, phdr = 0, sz = 0, sz1, sp, stackbase;
	inode_t ip = 0;
	struct elfhdr elf;
	struct proghdr ph;
	pagedir_t oldpgdir, pgdir = 0;
	process_t p = cur_proc();
	char *name, *path_p;

	log_begin();
	logged = 1;
	ip = namei(path);
	if (!ip)
		goto fail;
	ilock(ip);

	if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
		goto fail;
	if (elf.magic != ELF_MAGIC || elf.elf[0] != ELF_CLASS_64 ||
	    elf.elf[1] != ELF_DATA_LSB || elf.type != ELF_TYPE_EXEC ||
	    elf.machine != ELF_MACHINE_RISCV ||
	    elf.phentsize != sizeof(struct proghdr))
		goto fail;

	pgdir = process_pagedir(p);
	if (!pgdir)
		goto fail;

	for (i = 0, off = elf.phoff; i < elf.phnum;
	     i++, off += sizeof(ph)) {
		if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
			goto fail;
		if (ph.type != ELF_PROG_LOAD)
			continue;
		if (ph.vaddr + ph.memsz < ph.vaddr || ph.memsz < ph.filesz)
			goto fail;
		if ((ph.vaddr & (PGSIZE - 1)) !=
		    (ph.off & (PGSIZE - 1)))
			goto fail;

		if (elf.phoff >= ph.off &&
		    elf.phoff + elf.phnum * sizeof(ph) <= ph.off + ph.filesz)
			phdr = ph.vaddr + (elf.phoff - ph.off);

		sz1 = vm_alloc(pgdir, sz, ph.vaddr + ph.memsz,
		               flags2perm(ph.flags));
		if (!sz1)
			goto fail;
		sz = sz1;
		if (loadseg(pgdir, ph.vaddr, ip, ph.off, ph.filesz) < 0)
			goto fail;
	}
	if (!phdr)
		goto fail;

	iunlockput(ip);
	ip = 0;
	log_end();
	logged = 0;

	sz = PGROUNDUP(sz);
	sz1 = vm_alloc(pgdir, sz, sz + PGSIZE * 2, PTE_W);
	if (!sz1)
		goto fail;
	sp = sz1;
	stackbase = sp - PGSIZE;
	vm_clear(pgdir, sp - PGSIZE * 2);

	argc = build_linux_stack(pgdir, sp, stackbase, argv, envp, phdr,
	                         &elf, &sp);
	if (argc < 0)
		goto fail;
	sz = sz1;

	oldpgdir = p->pagetable;
	oldsz = p->sz;
	p->pagetable = pgdir;
	p->sz = sz;
	p->cur_thread->trapframe->a0 = argc;
	p->cur_thread->trapframe->a1 = sp + sizeof(uint64);
	p->cur_thread->trapframe->sp = sp;
	p->cur_thread->trapframe->epc = elf.entry;

	for (name = path_p = path; *path_p; path_p++) {
		if (*path_p == '/')
			name = path_p + 1;
	}
	safe_strncpy(p->name, name, MAXNAME);
	process_freepagedir(oldpgdir, oldsz);
	return argc;

fail:
	if (pgdir)
		process_freepagedir(pgdir, sz);
	if (ip)
		iunlockput(ip);
	if (logged)
		log_end();
	return -1;
}

/* Temporary source-level bridge for code removed by the Linux UAPI switch. */
int exec(char *path, char **argv)
{
	char *envp[] = { 0 };

	return exec_linux(path, argv, envp);
}
