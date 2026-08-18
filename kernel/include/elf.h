#ifndef __CAFFEINIX_KERNEL_ELF_H
#define __CAFFEINIX_KERNEL_ELF_H

#include <typedefs.h>

#define ELF_MAGIC                  0x464c457fU
#define ELF_CLASS_64              2
#define ELF_DATA_LSB              1
#define ELF_VERSION_CURRENT       1

#define ELF_TYPE_EXEC             2
#define ELF_TYPE_DYN              3

#define ELF_MACHINE_RISCV         243

#define ELF_PHDR_TABLE_MAX        65536

struct elfhdr {
	uint32 magic;
	uint8 elf[12];
	uint16 type;
	uint16 machine;
	uint32 version;
	uint64 entry;
	uint64 phoff;
	uint64 shoff;
	uint32 flags;
	uint16 ehsize;
	uint16 phentsize;
	uint16 phnum;
	uint16 shentsize;
	uint16 shnum;
	uint16 shstrndx;
};

struct proghdr {
	uint32 type;
	uint32 flags;
	uint64 off;
	uint64 vaddr;
	uint64 paddr;
	uint64 filesz;
	uint64 memsz;
	uint64 align;
};

_Static_assert(sizeof(struct elfhdr) == 64,
	       "ELF64 file header layout changed");
_Static_assert(sizeof(struct proghdr) == 56,
	       "ELF64 program header layout changed");

#define ELF_PROG_NULL             0
#define ELF_PROG_LOAD             1
#define ELF_PROG_DYNAMIC          2
#define ELF_PROG_INTERP           3
#define ELF_PROG_NOTE             4
#define ELF_PROG_SHLIB            5
#define ELF_PROG_PHDR             6
#define ELF_PROG_TLS              7
#define ELF_PROG_GNU_STACK        0x6474e551U
#define ELF_PROG_GNU_RELRO        0x6474e552U

#define ELF_PROG_FLAG_EXEC        1
#define ELF_PROG_FLAG_WRITE       2
#define ELF_PROG_FLAG_READ        4
#define ELF_PROG_FLAG_MASK        7

struct elf_image_layout {
	uint64 map_start;
	uint64 map_end;
	uint64 load_end;
	uint64 phdr;
	uint64 explicit_phdr;
	uint64 interp_offset;
	uint64 interp_size;
	uint64 entry;
	uint64 phdr_size;
	uint64 load_align;
	uint16 type;
	uint16 phnum;
	uint16 load_count;
	uint8 has_phdr;
	uint8 has_explicit_phdr;
	uint8 has_interp;
	uint8 entry_in_executable;
	uint8 stack_executable;
};

struct elf_runtime_layout {
	uint64 load_bias;
	uint64 map_start;
	uint64 map_end;
	uint64 phdr;
	uint64 entry;
};

int elf_image_layout_init(struct elf_image_layout *layout,
			  const struct elfhdr *header);
int elf_image_layout_add(struct elf_image_layout *layout,
			 const struct elfhdr *header,
			 const struct proghdr *program);
int elf_image_layout_finish(const struct elf_image_layout *layout);
int elf_runtime_layout(const struct elf_image_layout *image,
		       uint64 mapping_start, uint64 address_limit,
		       struct elf_runtime_layout *runtime);
int elf_relocate_address(uint64 load_bias, uint64 address,
			 uint64 *relocated);
int elf_interpreter_path_valid(const char *path, uint64 size,
			       uint64 capacity);

#endif
