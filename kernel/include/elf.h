/*
 * RV64 ELF file layout and loader-validation interfaces.
 *
 * The structures deliberately match the ELF64 on-disk ABI. Layout helpers
 * validate arithmetic before exec maps any segment into a process address
 * space.
 */
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

/**
 * elf_image_layout_init() - Validate an ELF header and initialize layout data
 * @layout: Output layout storage.
 * @header: ELF64 header read from the executable.
 *
 * Context: Caller supplies stable kernel memory; does not sleep.
 * Return: Zero, or -1 for NULL arguments or an unsupported or malformed ELF.
 */
int elf_image_layout_init(struct elf_image_layout *layout,
			  const struct elfhdr *header);
/**
 * elf_image_layout_add() - Incorporate one program header into an ELF layout
 * @layout: Layout initialized by elf_image_layout_init().
 * @header: Validated owning ELF header.
 * @program: Program header to validate and account for.
 *
 * Context: Does not sleep. Call once for each file-order program header.
 * Return: Zero or -1; discard the layout after a validation failure.
 */
int elf_image_layout_add(struct elf_image_layout *layout,
			 const struct elfhdr *header,
			 const struct proghdr *program);
/**
 * elf_image_layout_finish() - Check cross-segment ELF layout constraints
 * @layout: Fully populated ELF image layout.
 *
 * Context: Does not sleep.
 * Return: Zero for a complete valid layout, or -1 for an invalid or NULL one.
 */
int elf_image_layout_finish(const struct elf_image_layout *layout);
/**
 * elf_runtime_layout() - Choose a runtime placement for a validated image
 * @image: Finished ELF image layout.
 * @mapping_start: Candidate lowest mapped user address.
 * @address_limit: Exclusive upper bound for mappings.
 * @runtime: Output runtime layout.
 *
 * Context: Does not sleep; @runtime may not be NULL.
 * Return: Zero or -1 for invalid arguments, overflow, or an invalid range.
 * Output fields may be partially written on failure and must be discarded.
 */
int elf_runtime_layout(const struct elf_image_layout *image,
		       uint64 mapping_start, uint64 address_limit,
		       struct elf_runtime_layout *runtime);
/**
 * elf_relocate_address() - Add a PIE load bias without address overflow
 * @load_bias: Runtime base selected for the ELF image.
 * @address: Link-time ELF virtual address.
 * @relocated: Output relocated address.
 *
 * Context: Does not sleep; @relocated may not be NULL.
 * Return: Zero, or -1 for NULL output or overflow. Discard the output on error.
 */
int elf_relocate_address(uint64 load_bias, uint64 address,
			 uint64 *relocated);
/**
 * elf_interpreter_path_valid() - Validate a PT_INTERP byte sequence
 * @path: Kernel buffer holding interpreter bytes; NULL is invalid.
 * @size: Exact PT_INTERP file size including required NUL.
 * @capacity: Capacity of @path in bytes.
 *
 * Context: Does not sleep.
 * Return: 1 for an absolute path with exactly one terminating NUL within
 * @capacity, or 0 for invalid input. This is a predicate, not an errno result.
 */
int elf_interpreter_path_valid(const char *path, uint64 size,
			       uint64 capacity);

#endif
