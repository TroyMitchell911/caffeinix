#include <elf.h>
#include <file.h>
#include <linux_uapi.h>
#include <mem_layout.h>
#include <mystring.h>
#include <palloc.h>
#include <random.h>
#include <scheduler.h>
#include <syscall.h>
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

struct exec_aslr_layout {
	uint64 pie_hint;
	uint64 interpreter_hint;
	uint64 stack_top;
	uint64 mmap_top;
	uint64 brk_gap;
};

static int random_page_offset(uint64 size, uint64 *offset)
{
	uint64 pages, value;

	if (!offset || size < PGSIZE || size % PGSIZE ||
	    get_random_u64(&value) < 0)
		return -1;
	pages = size / PGSIZE;
	*offset = value % pages * PGSIZE;
	return 0;
}

static int exec_aslr_layout_init(struct exec_aslr_layout *layout)
{
	uint64 brk_offset, interpreter_offset, mmap_offset;
	uint64 pie_offset, stack_offset;

	if (!layout ||
	    random_page_offset(USER_PIE_RND_SIZE, &pie_offset) < 0 ||
	    random_page_offset(USER_INTERP_RND_SIZE,
			       &interpreter_offset) < 0 ||
	    random_page_offset(USER_STACK_RND_SIZE, &stack_offset) < 0 ||
	    random_page_offset(USER_MMAP_RND_SIZE, &mmap_offset) < 0 ||
	    random_page_offset(USER_BRK_RND_SIZE, &brk_offset) < 0)
		return -1;
	layout->pie_hint = USER_PIE_BASE + pie_offset;
	layout->interpreter_hint = USER_INTERP_BASE + interpreter_offset;
	layout->stack_top = USER_STACK_TOP - stack_offset;
	layout->mmap_top = USER_MMAP_TOP - mmap_offset;
	layout->brk_gap = brk_offset;
	return 0;
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

static int protection2perm(uint32 protection)
{
	int permissions = PTE_U;

	if (protection & (LINUX_PROT_READ | LINUX_PROT_WRITE))
		permissions |= PTE_R;
	if (protection & LINUX_PROT_WRITE)
		permissions |= PTE_W;
	if (protection & LINUX_PROT_EXEC)
		permissions |= PTE_X;
	return permissions;
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
	int64 result;
	int i;

	*image = (struct elf_image){0};
	result = vfs_open_file(path, VFS_OPEN_EXEC, 0, &image->file);
	if (result < 0)
		return linux_error(result);
	if (!image->file->path.dentry ||
	    !image->file->path.dentry->inode ||
	    image->file->path.dentry->inode->type == VFS_INODE_DIRECTORY) {
		elf_image_close(image);
		return -LINUX_EACCES;
	}
	result = vfs_file_pread(image->file, 0, (uint64)&image->header,
				sizeof(image->header), 0);
	if (result < 0) {
		result = linux_error(result);
		goto fail;
	}
	if (result != sizeof(image->header) ||
	    elf_image_layout_init(&image->layout, &image->header) < 0) {
		result = -LINUX_ENOEXEC;
		goto fail;
	}
	if (elf_program_order(image->layout.phdr_size,
			      &image->program_order) < 0) {
		result = -LINUX_ENOEXEC;
		goto fail;
	}
	image->programs = alloc_pages(image->program_order, 0);
	if (!image->programs) {
		result = -LINUX_ENOMEM;
		goto fail;
	}
	result = vfs_file_pread(image->file, 0, (uint64)image->programs,
				image->layout.phdr_size, image->header.phoff);
	if (result < 0) {
		result = linux_error(result);
		goto fail;
	}
	if (result != image->layout.phdr_size) {
		result = -LINUX_ENOEXEC;
		goto fail;
	}
	for (i = 0; i < image->header.phnum; i++) {
		if (elf_image_layout_add(&image->layout, &image->header,
					 &image->programs[i]) < 0) {
			result = -LINUX_ENOEXEC;
			goto fail;
		}
	}
	if (elf_image_layout_finish(&image->layout) < 0) {
		result = -LINUX_ENOEXEC;
		goto fail;
	}
	return 0;

fail:
	elf_image_close(image);
	return result;
}

static void elf_image_exec_credentials(
	const struct elf_image *image,
	struct process_credentials *credentials)
{
	struct vfs_inode *inode = image->file->path.dentry->inode;
	sleeplock_t lock = inode->superblock ?
		&inode->superblock->attribute_lock : 0;
	uint32 gid, mode, uid;

	if (lock)
		sleeplock_acquire(lock);
	mode = inode->mode;
	uid = inode->uid;
	gid = inode->gid;
	if (lock)
		sleeplock_release(lock);
	process_credentials_get(credentials);
	if (mode & 04000)
		credentials->euid = uid;
	if ((mode & 02000) && (mode & 00010))
		credentials->egid = gid;
	credentials->suid = credentials->euid;
	credentials->fsuid = credentials->euid;
	credentials->sgid = credentials->egid;
	credentials->fsgid = credentials->egid;
}

static int elf_image_interpreter(const struct elf_image *image, char *path)
{
	int64 result;
	uint64 size = image->layout.interp_size;

	if (!image->layout.has_interp || size > MAXPATH)
		return -LINUX_ENOEXEC;
	result = vfs_file_pread(image->file, 0, (uint64)path, size,
				image->layout.interp_offset);
	if (result < 0)
		return linux_error(result);
	if (result != size ||
	    !elf_interpreter_path_valid(path, size, MAXPATH))
		return -LINUX_ENOEXEC;
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
	const struct vm_area *area;
	const struct proghdr *program;
	uint64 file_length, map_end, map_start;
	int i;

	for (i = 0; i < image->header.phnum; i++) {
		program = &image->programs[i];
		if (program->type != ELF_PROG_LOAD || !program->memsz)
			continue;
		if (elf_relocate_address(image->runtime.load_bias,
					 PGROUNDDOWN(program->vaddr),
					 &map_start) < 0 ||
		    elf_relocate_address(image->runtime.load_bias,
					 PGROUNDUP(program->vaddr +
						   program->memsz),
					 &map_end) < 0 ||
		    program->vaddr - PGROUNDDOWN(program->vaddr) >
			(uint64)-1 - program->filesz)
			return -1;
		file_length = program->vaddr - PGROUNDDOWN(program->vaddr) +
			      program->filesz;
		area = vma_find(vmas, map_start);
		if (vma_insert_elf_file(vmas, map_start, map_end,
					 flags2prot(program->flags), image->file,
					 PGROUNDDOWN(program->off),
					 file_length) < 0)
			return -1;
		if (area) {
			uint64 page, pa, copy_end, copy_start, file_start;
			int j;

			area = vma_find(vmas, map_start);
			if (!area || area->end < map_start + PGSIZE ||
			    vm_alloc_load_range(pgdir, map_start,
						map_start + PGSIZE,
						protection2perm(
							area->protection)) < 0)
				return -1;
			pa = vm_user_pa(pgdir, map_start);
			if (!pa)
				return -1;
			for (j = 0; j < image->header.phnum; j++) {
				const struct proghdr *load = &image->programs[j];

				if (load->type != ELF_PROG_LOAD || !load->filesz ||
				    elf_relocate_address(image->runtime.load_bias,
							 load->vaddr,
							 &file_start) < 0)
					continue;
				copy_start = file_start > map_start ?
					file_start : map_start;
				copy_end = file_start + load->filesz;
				if (copy_end > map_start + PGSIZE)
					copy_end = map_start + PGSIZE;
				if (copy_start >= copy_end)
					continue;
				page = load->off + copy_start - file_start;
				if (vfs_file_pread(image->file, 0,
						   pa + copy_start - map_start,
						   copy_end - copy_start,
						   page) !=
				    (int64)(copy_end - copy_start))
					return -1;
			}
		}
	}
	return 0;
}

static int build_linux_stack(pagedir_t pgdir, uint64 stack_top,
			     uint64 stack_base, char **argv, char **envp,
			     const char *execfn,
			     const struct linux_exec_layout *exec,
			     const struct process_credentials *credentials,
			     uint64 *new_sp)
{
	uint64 argv_address[MAXARG];
	uint64 envp_address[MAXARG];
	uint64 execfn_address;
	uint64 words[4 * MAXARG + 32];
	uint8 random[16];
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
	length = strlen(execfn) + 1;
	sp -= length;
	if (sp < stack_base ||
	    copyout(pgdir, sp, (char *)execfn, length) < 0)
		return -1;
	execfn_address = sp;

	if (get_random_bytes(random, sizeof(random)) < 0)
		return -1;
	sp -= sizeof(random);
	if (sp < stack_base ||
	    copyout(pgdir, sp, (char *)random, sizeof(random)) < 0) {
		memset(random, 0, sizeof(random));
		return -1;
	}
	memset(random, 0, sizeof(random));
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
	AUX(LINUX_AT_UID, credentials->uid);
	AUX(LINUX_AT_EUID, credentials->euid);
	AUX(LINUX_AT_GID, credentials->gid);
	AUX(LINUX_AT_EGID, credentials->egid);
	AUX(LINUX_AT_SECURE, credentials->uid != credentials->euid ||
			     credentials->gid != credentials->egid);
	AUX(LINUX_AT_RANDOM, length);
	AUX(LINUX_AT_EXECFN, execfn_address);
	AUX(LINUX_AT_NULL, 0);
#undef AUX

	length = nwords * sizeof(uint64);
	sp = (sp - length) & ~15ULL;
	if (sp < stack_base || copyout(pgdir, sp, (char *)words, length) < 0)
		return -1;

	*new_sp = sp;
	return argc;
}

static int copy_process_cmdline(char **argv, void **page_out,
				uint32 *length_out)
{
	char *page;
	uint32 length = 0;
	int index;

	page = palloc_zero();
	if (!page)
		return -LINUX_ENOMEM;
	for (index = 0; argv[index]; index++) {
		uint64 argument_length, available;

		if (index >= MAXARG - 1) {
			pfree(page);
			return -LINUX_E2BIG;
		}
		if (length == PROCESS_CMDLINE_MAX)
			continue;
		argument_length = strlen(argv[index]) + 1;
		available = PROCESS_CMDLINE_MAX - length;
		if (argument_length > available) {
			if (available > 1)
				memmove(page + length, argv[index], available - 1);
			page[PROCESS_CMDLINE_MAX - 1] = 0;
			length = PROCESS_CMDLINE_MAX;
			continue;
		}
		memmove(page + length, argv[index], argument_length);
		length += argument_length;
	}
	*page_out = page;
	*length_out = length;
	return 0;
}

static int exec_elf(char *path, const char *execfn, char **argv, char **envp)
{
	struct elf_image executable = {0}, interpreter = {0};
	struct exec_aslr_layout aslr;
	struct linux_exec_layout exec;
	struct process_credentials credentials;
	struct vma_set new_vmas, old_vmas;
	char interpreter_path[MAXPATH];
	void *new_cmdline = 0;
	uint32 new_cmdline_length = 0;
	pagedir_t oldpgdir, pgdir = 0;
	process_t process = cur_proc();
	thread_t current = cur_thread();
	uint64 brk_start, entry, oldsz, sp, stack_base, sz = 0;
	const char *name, *path_p;
	int argc, error = -LINUX_ENOEXEC, stack_permissions = PTE_W;
	uint32 stack_protection = LINUX_PROT_READ | LINUX_PROT_WRITE;

	if (process_exec_begin(process, current) < 0)
		return -LINUX_EAGAIN;
	vma_set_init(&new_vmas);
	vma_set_init(&old_vmas);
	error = copy_process_cmdline(argv, &new_cmdline,
				     &new_cmdline_length);
	if (error < 0)
		goto fail;
	if (exec_aslr_layout_init(&aslr) < 0) {
		error = -LINUX_EIO;
		goto fail;
	}
	error = elf_image_open(path, &executable);
	if (error < 0)
		goto fail;
	elf_image_exec_credentials(&executable, &credentials);
	if (executable.layout.has_interp) {
		error = elf_image_interpreter(&executable,
					      interpreter_path);
		if (error < 0)
			goto fail;
	}

	pgdir = process_pagedir(process, current);
	if (!pgdir) {
		error = -LINUX_ENOMEM;
		goto fail;
	}
	if (elf_image_place(&executable, &new_vmas, aslr.pie_hint) < 0 ||
	    elf_image_map(&executable, pgdir, &new_vmas) < 0) {
		error = -LINUX_ENOEXEC;
		goto fail;
	}
	sz = executable.runtime.map_end;
	entry = executable.runtime.entry;
	exec.base = 0;

	if (executable.layout.has_interp) {
		error = elf_image_open(interpreter_path, &interpreter);
		if (error < 0)
			goto fail;
		if (interpreter.layout.has_interp ||
		    elf_image_place(&interpreter, &new_vmas,
				    aslr.interpreter_hint) < 0 ||
		    elf_image_map(&interpreter, pgdir, &new_vmas) < 0) {
			error = -LINUX_ENOEXEC;
			goto fail;
		}
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

	stack_base = aslr.stack_top - USER_STACK_SIZE;
	if (vm_alloc_range(pgdir, stack_base, aslr.stack_top,
			   stack_permissions) < 0 ||
	    vma_insert(&new_vmas, stack_base, aslr.stack_top,
		       stack_protection,
		       LINUX_MAP_PRIVATE, VMA_ANONYMOUS, VMA_STACK,
		       0, 0) < 0) {
		error = -LINUX_ENOMEM;
		goto fail;
	}
	sp = aslr.stack_top;
	argc = build_linux_stack(pgdir, aslr.stack_top, stack_base,
				 argv, envp, execfn, &exec, &credentials, &sp);
	if (argc < 0) {
		error = -LINUX_E2BIG;
		goto fail;
	}
	if (sz > (uint64)-1 - aslr.brk_gap)
		goto fail;
	brk_start = PGROUNDUP(sz) + aslr.brk_gap;
	if (brk_start < sz || brk_start >= aslr.mmap_top ||
	    !vma_range_free(&new_vmas, PGROUNDUP(sz), brk_start + PGSIZE))
		goto fail;
	if (process_exec_quiesce(process, current) < 0) {
		error = -LINUX_EAGAIN;
		goto fail;
	}

	sleeplock_acquire(&process->mmap_lock);
	oldpgdir = process->pagetable;
	oldsz = process->sz;
	vma_set_move(&old_vmas, &process->vmas);
	vma_set_move(&process->vmas, &new_vmas);
	process->pagetable = pgdir;
	process->sz = brk_start;
	process->brk = brk_start;
	process->brk_start = brk_start;
	process->mmap_top = aslr.mmap_top;
	sleeplock_release(&process->mmap_lock);
	current->trapframe->a0 = argc;
	current->trapframe->a1 = sp + sizeof(uint64);
	current->trapframe->sp = sp;
	current->trapframe->epc = entry;
	memset(current->trapframe->f, 0, sizeof(current->trapframe->f));
	current->trapframe->fcsr = 0;
	__atomic_store_n(&process->membarrier_private_expedited, 0,
	                 __ATOMIC_RELEASE);
	spinlock_acquire(&process->lock);
	process->credentials = credentials;
	spinlock_release(&process->lock);
	signal_process_exec(process, current);

	for (name = path_p = execfn; *path_p; path_p++) {
		if (*path_p == '/')
			name = path_p + 1;
	}
	safe_strncpy(process->name, name, MAXNAME);
	process_set_cmdline(process, new_cmdline, new_cmdline_length);
	new_cmdline = 0;
	process_exec_end(process, 1);
	process_freepagedir(oldpgdir, oldsz);
	vma_set_destroy(&old_vmas);
	return argc;

fail:
	if (new_cmdline)
		pfree(new_cmdline);
	elf_image_close(&interpreter);
	elf_image_close(&executable);
	if (pgdir)
		process_freepagedir(pgdir, sz);
	vma_set_destroy(&new_vmas);
	process_exec_end(process, 0);
	return error;
}

#define EXEC_SCRIPT_BUFFER_SIZE 256
#define EXEC_SCRIPT_MAX_DEPTH   4

static int exec_script_or_elf(char *path, const char *execfn, char **argv,
			      char **envp, int depth);

static int exec_script(char *path, const char *execfn, char **argv,
		       char **envp, int depth)
{
	char buffer[EXEC_SCRIPT_BUFFER_SIZE + 1];
	char *interpreter, *optional, *separator;
	char *script_argv[MAXARG];
	file_t file = 0;
	int64 length;
	int input = 1, output = 0;

	length = vfs_open_file(path, VFS_OPEN_EXEC, 0, &file);
	if (length < 0)
		return linux_error(length);
	length = vfs_file_pread(file, 0, (uint64)buffer,
				EXEC_SCRIPT_BUFFER_SIZE, 0);
	vfs_file_put(file);
	if (length < 0)
		return linux_error(length);
	if (length < 2 || buffer[0] != '#' || buffer[1] != '!')
		return -LINUX_ENOEXEC;
	buffer[length] = 0;

	separator = buffer + 2;
	while (*separator == ' ' || *separator == '\t')
		separator++;
	interpreter = separator;
	while (*separator && *separator != '\n' &&
	       *separator != ' ' && *separator != '\t')
		separator++;
	if (separator == interpreter)
		return -LINUX_ENOEXEC;
	if (!*separator && length == EXEC_SCRIPT_BUFFER_SIZE)
		return -LINUX_ENOEXEC;
	if (*separator && *separator != '\n') {
		*separator++ = 0;
		while (*separator == ' ' || *separator == '\t')
			separator++;
		optional = separator;
	} else {
		optional = 0;
		if (*separator)
			*separator = 0;
	}

	if (optional) {
		char *end = optional;

		while (*end && *end != '\n')
			end++;
		while (end > optional &&
		       (end[-1] == ' ' || end[-1] == '\t'))
			end--;
		*end = 0;
		if (!*optional)
			optional = 0;
	}
	if (strlen(interpreter) >= MAXPATH)
		return -LINUX_ENOEXEC;
	if (depth >= EXEC_SCRIPT_MAX_DEPTH)
		return -LINUX_ELOOP;

	script_argv[output++] = interpreter;
	if (optional)
		script_argv[output++] = optional;
	script_argv[output++] = path;
	while (argv[input]) {
		if (output >= MAXARG - 1)
			return -LINUX_E2BIG;
		script_argv[output++] = argv[input++];
	}
	script_argv[output] = 0;
	return exec_script_or_elf(interpreter, execfn, script_argv, envp,
				  depth + 1);
}

static int exec_script_or_elf(char *path, const char *execfn, char **argv,
			      char **envp, int depth)
{
	int result = exec_elf(path, execfn, argv, envp);

	if (result != -LINUX_ENOEXEC)
		return result;
	return exec_script(path, execfn, argv, envp, depth);
}

int exec_linux(char *path, char **argv, char **envp)
{
	return exec_script_or_elf(path, path, argv, envp, 0);
}
