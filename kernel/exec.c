#include <elf.h>
#include <file.h>
#include <linux_uapi.h>
#include <mem_layout.h>
#include <mystring.h>
#include <palloc.h>
#include <scheduler.h>
#include <vfs.h>
#include <vm.h>
#include <vma.h>

struct elf_image {
	file_t file;
	struct proghdr *programs;
	struct elfhdr header;
	struct elf_image_layout layout;
	struct elf_runtime_layout runtime;
	unsigned int program_order;
};

struct linux_exec_layout {
	uint64 phdr;
	uint64 entry;
	uint64 base;
	uint16 phnum;
};

static int flags2perm(int flags)
{
	int perm;

	if (!flags)
		return PTE_R;
	perm = PTE_U;
	if (flags & (ELF_PROG_FLAG_READ | ELF_PROG_FLAG_WRITE))
		perm |= PTE_R;
	if (flags & ELF_PROG_FLAG_EXEC)
		perm |= PTE_X;
	if (flags & ELF_PROG_FLAG_WRITE)
		perm |= PTE_W;
	return perm;
}

static int flags2prot(int flags)
{
	int protection = 0;

	if (flags & ELF_PROG_FLAG_READ)
		protection |= LINUX_PROT_READ;
	if (flags & ELF_PROG_FLAG_WRITE)
		protection |= LINUX_PROT_WRITE;
	if (flags & ELF_PROG_FLAG_EXEC)
		protection |= LINUX_PROT_EXEC;
	return protection;
}

static int loadseg(pagedir_t pgdir, uint64 va, file_t file,
		   uint64 offset, uint64 size)
{
	uint64 page_offset, pa, n;

	while (size) {
		pa = vm_user_pa(pgdir, va);
		if (!pa)
			return -1;

		page_offset = va & (PGSIZE - 1);
		n = PGSIZE - page_offset;
		if (n > size)
			n = size;
		if (vfs_file_pread(file, 0, pa, n, offset) != n)
			return -1;

		va += n;
		offset += n;
		size -= n;
	}
	return 0;
}

static void elf_image_close(struct elf_image *image)
{
	if (image->programs) {
		free_pages(image->programs, image->program_order);
		image->programs = 0;
	}
	if (image->file) {
		vfs_file_put(image->file);
		image->file = 0;
	}
}

static int elf_program_order(uint64 size, unsigned int *order)
{
	uint64 capacity = PGSIZE;

	*order = 0;
	while (capacity < size) {
		if (capacity > (uint64)-1 / 2)
			return -1;
		capacity <<= 1;
		(*order)++;
	}
	return 0;
}

static int elf_image_open(const char *path, struct elf_image *image)
{
	int i;

	*image = (struct elf_image){0};
	if (vfs_open_file(path, VFS_OPEN_READ, 0, &image->file) < 0)
		return -1;
	if (vfs_file_pread(image->file, 0, (uint64)&image->header,
			   sizeof(image->header), 0) != sizeof(image->header) ||
	    elf_image_layout_init(&image->layout, &image->header) < 0)
		goto fail;
	if (elf_program_order(image->layout.phdr_size,
			      &image->program_order) < 0)
		goto fail;
	image->programs = alloc_pages(image->program_order, 0);
	if (!image->programs ||
	    vfs_file_pread(image->file, 0, (uint64)image->programs,
			   image->layout.phdr_size, image->header.phoff) !=
	    image->layout.phdr_size)
		goto fail;
	for (i = 0; i < image->header.phnum; i++) {
		if (elf_image_layout_add(&image->layout, &image->header,
					 &image->programs[i]) < 0)
			goto fail;
	}
	if (elf_image_layout_finish(&image->layout) < 0)
		goto fail;
	return 0;

fail:
	elf_image_close(image);
	return -1;
}

static int elf_image_interpreter(const struct elf_image *image, char *path)
{
	uint64 size = image->layout.interp_size;

	if (!image->layout.has_interp || size > MAXPATH ||
	    vfs_file_pread(image->file, 0, (uint64)path, size,
			   image->layout.interp_offset) != size ||
	    !elf_interpreter_path_valid(path, size, MAXPATH))
		return -1;
	return 0;
}

static int elf_image_place(struct elf_image *image, struct vma_set *vmas,
			   uint64 hint)
{
	uint64 length = image->layout.map_end - image->layout.map_start;
	uint64 low, mapping_start;

	if (image->header.type == ELF_TYPE_EXEC) {
		mapping_start = image->layout.map_start;
		if (mapping_start < PGSIZE || mapping_start > USER_MMAP_TOP ||
		    length > USER_MMAP_TOP - mapping_start ||
		    !vma_range_free(vmas, mapping_start,
				    mapping_start + length))
			return -1;
	} else {
		low = image->layout.map_start;
		if (low < PGSIZE)
			low = PGSIZE;
		if (hint < low)
			hint = low;
		if (vma_find_gap_aligned(vmas, low, USER_MMAP_TOP, hint,
					 length, image->layout.load_align,
					 image->layout.map_start,
					 &mapping_start) < 0)
			return -1;
	}
	return elf_runtime_layout(&image->layout, mapping_start,
				  USER_MMAP_TOP, &image->runtime);
}

static int elf_image_map(struct elf_image *image, pagedir_t pgdir,
			 struct vma_set *vmas)
{
	const struct proghdr *program;
	uint64 address, map_end, map_start;
	int i;

	for (i = 0; i < image->header.phnum; i++) {
		program = &image->programs[i];
		if (program->type != ELF_PROG_LOAD || !program->memsz)
			continue;
		if (elf_relocate_address(image->runtime.load_bias,
					 program->vaddr, &address) < 0 ||
		    elf_relocate_address(image->runtime.load_bias,
					 PGROUNDDOWN(program->vaddr),
					 &map_start) < 0 ||
		    elf_relocate_address(image->runtime.load_bias,
					 PGROUNDUP(program->vaddr +
						   program->memsz),
					 &map_end) < 0 ||
		    vm_alloc_load_range(pgdir, map_start, map_end,
					flags2perm(program->flags)) < 0 ||
		    vma_insert_elf(vmas, map_start, map_end,
				    flags2prot(program->flags), image->file,
				    PGROUNDDOWN(program->off)) < 0 ||
		    loadseg(pgdir, address, image->file, program->off,
			    program->filesz) < 0)
			return -1;
	}
	return 0;
}

static int build_linux_stack(pagedir_t pgdir, uint64 stack_top,
			     uint64 stack_base, char **argv, char **envp,
			     const struct linux_exec_layout *exec,
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
	words[nwords++] = 0;
	for (int i = 0; i < envc; i++)
		words[nwords++] = envp_address[i];
	words[nwords++] = 0;

#define AUX(tag, value) do { \
	words[nwords++] = (tag); \
	words[nwords++] = (value); \
} while (0)
	AUX(LINUX_AT_PHDR, exec->phdr);
	AUX(LINUX_AT_PHENT, sizeof(struct proghdr));
	AUX(LINUX_AT_PHNUM, exec->phnum);
	AUX(LINUX_AT_PAGESZ, PGSIZE);
	AUX(LINUX_AT_BASE, exec->base);
	AUX(LINUX_AT_ENTRY, exec->entry);
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
	struct elf_image executable = {0}, interpreter = {0};
	struct linux_exec_layout exec;
	struct vma_set new_vmas, old_vmas;
	char interpreter_path[MAXPATH];
	pagedir_t oldpgdir, pgdir = 0;
	process_t process = cur_proc();
	uint64 entry, oldsz, sp, sz = 0;
	char *name, *path_p;
	int argc, stack_permissions = PTE_W;
	uint32 stack_protection = LINUX_PROT_READ | LINUX_PROT_WRITE;

	vma_set_init(&new_vmas);
	vma_set_init(&old_vmas);
	if (elf_image_open(path, &executable) < 0)
		goto fail;
	if (executable.layout.has_interp &&
	    elf_image_interpreter(&executable, interpreter_path) < 0)
		goto fail;

	pgdir = process_pagedir(process);
	if (!pgdir ||
	    elf_image_place(&executable, &new_vmas, USER_PIE_BASE) < 0 ||
	    elf_image_map(&executable, pgdir, &new_vmas) < 0)
		goto fail;
	sz = executable.runtime.map_end;
	entry = executable.runtime.entry;
	exec.base = 0;

	if (executable.layout.has_interp) {
		if (elf_image_open(interpreter_path, &interpreter) < 0 ||
		    interpreter.layout.has_interp ||
		    elf_image_place(&interpreter, &new_vmas,
				    USER_INTERP_BASE) < 0 ||
		    elf_image_map(&interpreter, pgdir, &new_vmas) < 0)
			goto fail;
		entry = interpreter.runtime.entry;
		exec.base = interpreter.runtime.load_bias;
	}
	exec.phdr = executable.runtime.phdr;
	exec.entry = executable.runtime.entry;
	exec.phnum = executable.header.phnum;
	if (executable.layout.stack_executable) {
		stack_permissions |= PTE_X;
		stack_protection |= LINUX_PROT_EXEC;
	}
	elf_image_close(&interpreter);
	elf_image_close(&executable);

	if (vm_alloc_range(pgdir, USER_STACK_BASE, USER_STACK_TOP,
			   stack_permissions) < 0 ||
	    vma_insert(&new_vmas, USER_STACK_BASE, USER_STACK_TOP,
		       stack_protection,
		       LINUX_MAP_PRIVATE, VMA_ANONYMOUS, VMA_STACK,
		       0, 0) < 0)
		goto fail;
	sp = USER_STACK_TOP;
	argc = build_linux_stack(pgdir, USER_STACK_TOP, USER_STACK_BASE,
				 argv, envp, &exec, &sp);
	if (argc < 0)
		goto fail;

	sleeplock_acquire(&process->mmap_lock);
	oldpgdir = process->pagetable;
	oldsz = process->sz;
	vma_set_move(&old_vmas, &process->vmas);
	vma_set_move(&process->vmas, &new_vmas);
	process->pagetable = pgdir;
	process->sz = sz;
	process->brk = sz;
	process->brk_start = sz;
	sleeplock_release(&process->mmap_lock);
	cur_thread()->trapframe->a0 = argc;
	cur_thread()->trapframe->a1 = sp + sizeof(uint64);
	cur_thread()->trapframe->sp = sp;
	cur_thread()->trapframe->epc = entry;
	memset(cur_thread()->trapframe->f, 0,
	       sizeof(cur_thread()->trapframe->f));
	cur_thread()->trapframe->fcsr = 0;

	for (name = path_p = path; *path_p; path_p++) {
		if (*path_p == '/')
			name = path_p + 1;
	}
	safe_strncpy(process->name, name, MAXNAME);
	process_freepagedir(oldpgdir, oldsz);
	vma_set_destroy(&old_vmas);
	return argc;

fail:
	elf_image_close(&interpreter);
	elf_image_close(&executable);
	if (pgdir)
		process_freepagedir(pgdir, sz);
	vma_set_destroy(&new_vmas);
	return -1;
}
