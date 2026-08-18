#include <elf.h>
#include <riscv.h>

static int add_overflow(uint64 left, uint64 right, uint64 *sum)
{
	*sum = left + right;
	return *sum < left;
}

static int power_of_two(uint64 value)
{
	return value && !(value & (value - 1));
}

static int program_alignment_valid(const struct proghdr *program)
{
	if (program->align <= 1)
		return 1;
	if (!power_of_two(program->align))
		return 0;
	return (program->vaddr & (program->align - 1)) ==
	       (program->off & (program->align - 1));
}

static int program_region_valid(const struct proghdr *program)
{
	uint64 end;

	if (program->filesz > program->memsz ||
	    add_overflow(program->off, program->filesz, &end) ||
	    add_overflow(program->vaddr, program->memsz, &end))
		return 0;
	return program_alignment_valid(program);
}

int elf_image_layout_init(struct elf_image_layout *layout,
			  const struct elfhdr *header)
{
	uint64 table_end, table_size;

	if (!layout || !header || header->magic != ELF_MAGIC ||
	    header->elf[0] != ELF_CLASS_64 ||
	    header->elf[1] != ELF_DATA_LSB ||
	    header->elf[2] != ELF_VERSION_CURRENT ||
	    (header->type != ELF_TYPE_EXEC && header->type != ELF_TYPE_DYN) ||
	    header->machine != ELF_MACHINE_RISCV ||
	    header->version != ELF_VERSION_CURRENT ||
	    header->ehsize != sizeof(*header) ||
	    header->phentsize != sizeof(struct proghdr) || !header->phnum ||
	    header->phoff < sizeof(*header))
		return -1;
	table_size = (uint64)header->phnum * sizeof(struct proghdr);
	if (table_size > ELF_PHDR_TABLE_MAX ||
	    add_overflow(header->phoff, table_size, &table_end))
		return -1;

	*layout = (struct elf_image_layout){0};
	layout->entry = header->entry;
	layout->phdr_size = table_size;
	layout->load_align = PGSIZE;
	layout->type = header->type;
	layout->phnum = header->phnum;
	return 0;
}

static int elf_load_layout_add(struct elf_image_layout *layout,
			       const struct elfhdr *header,
			       const struct proghdr *program)
{
	uint64 candidate, file_end, map_end, segment_end, table_end;
	uint64 map_start;

	if (!program_region_valid(program) ||
	    (program->flags & ~ELF_PROG_FLAG_MASK) ||
	    (program->vaddr & (PGSIZE - 1)) !=
	    (program->off & (PGSIZE - 1)) ||
	    add_overflow(program->vaddr, program->memsz, &segment_end) ||
	    (layout->load_count && program->vaddr < layout->load_end))
		return -1;
	if (program->align > layout->load_align)
		layout->load_align = program->align;
	if (!program->memsz)
		return 0;
	if (segment_end > (uint64)-1 - (PGSIZE - 1))
		return -1;
	map_start = PGROUNDDOWN(program->vaddr);
	map_end = PGROUNDUP(segment_end);
	if (!layout->load_count || map_start < layout->map_start)
		layout->map_start = map_start;
	if (map_end > layout->map_end)
		layout->map_end = map_end;
	layout->load_end = segment_end;
	layout->load_count++;
	if ((program->flags & ELF_PROG_FLAG_EXEC) &&
	    header->entry >= program->vaddr &&
	    header->entry < segment_end)
		layout->entry_in_executable = 1;

	table_end = header->phoff + layout->phdr_size;
	file_end = program->off + program->filesz;
	if (header->phoff < program->off || table_end > file_end)
		return 0;
	candidate = program->vaddr + (header->phoff - program->off);
	if (layout->has_phdr && layout->phdr != candidate)
		return -1;
	layout->phdr = candidate;
	layout->has_phdr = 1;
	return 0;
}

int elf_image_layout_add(struct elf_image_layout *layout,
			 const struct elfhdr *header,
			 const struct proghdr *program)
{
	uint64 end;

	if (!layout || !header || !program || layout->type != header->type ||
	    layout->phnum != header->phnum || layout->entry != header->entry)
		return -1;
	switch (program->type) {
	case ELF_PROG_LOAD:
		return elf_load_layout_add(layout, header, program);
	case ELF_PROG_INTERP:
		if (layout->has_interp || !program_region_valid(program) ||
		    program->filesz < 2)
			return -1;
		layout->interp_offset = program->off;
		layout->interp_size = program->filesz;
		layout->has_interp = 1;
		return 0;
	case ELF_PROG_PHDR:
		if (layout->has_explicit_phdr ||
		    !program_region_valid(program) ||
		    program->off != header->phoff ||
		    program->filesz < layout->phdr_size ||
		    program->memsz < layout->phdr_size)
			return -1;
		layout->explicit_phdr = program->vaddr;
		layout->has_explicit_phdr = 1;
		return 0;
	case ELF_PROG_DYNAMIC:
	case ELF_PROG_TLS:
	case ELF_PROG_GNU_RELRO:
		return program_region_valid(program) ? 0 : -1;
	case ELF_PROG_GNU_STACK:
		if ((program->flags & ~ELF_PROG_FLAG_MASK) ||
		    !program_alignment_valid(program) || program->filesz)
			return -1;
		if (program->flags & ELF_PROG_FLAG_EXEC)
			layout->stack_executable = 1;
		return 0;
	default:
		if (add_overflow(program->off, program->filesz, &end))
			return -1;
		return 0;
	}
}

int elf_image_layout_finish(const struct elf_image_layout *layout)
{
	if (!layout || !layout->load_count ||
	    layout->map_start >= layout->map_end || !layout->has_phdr ||
	    !layout->entry_in_executable)
		return -1;
	if (layout->has_explicit_phdr &&
	    layout->explicit_phdr != layout->phdr)
		return -1;
	return 0;
}

int elf_relocate_address(uint64 load_bias, uint64 address,
			 uint64 *relocated)
{
	if (!relocated || add_overflow(load_bias, address, relocated))
		return -1;
	return 0;
}

int elf_runtime_layout(const struct elf_image_layout *image,
		       uint64 mapping_start, uint64 address_limit,
		       struct elf_runtime_layout *runtime)
{
	uint64 load_bias;

	if (!image || !runtime || mapping_start % PGSIZE ||
	    !power_of_two(image->load_align) ||
	    mapping_start < image->map_start ||
	    (image->type == ELF_TYPE_EXEC &&
	     mapping_start != image->map_start))
		return -1;
	load_bias = mapping_start - image->map_start;
	if (load_bias & (image->load_align - 1) ||
	    elf_relocate_address(load_bias, image->map_end,
				 &runtime->map_end) < 0 ||
	    runtime->map_end > address_limit ||
	    elf_relocate_address(load_bias, image->phdr,
				 &runtime->phdr) < 0 ||
	    runtime->phdr >= address_limit ||
	    elf_relocate_address(load_bias, image->entry,
				 &runtime->entry) < 0 ||
	    runtime->entry >= address_limit)
		return -1;
	runtime->load_bias = load_bias;
	runtime->map_start = mapping_start;
	return 0;
}

int elf_interpreter_path_valid(const char *path, uint64 size,
			       uint64 capacity)
{
	uint64 i;

	if (!path || size < 2 || size > capacity || path[0] != '/' ||
	    path[size - 1])
		return 0;
	for (i = 1; i + 1 < size; i++) {
		if (!path[i])
			return 0;
	}
	return 1;
}
