#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <elf.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "ELF check failed at line %d\n", __LINE__); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

static struct elfhdr valid_header(uint16 type, uint16 phnum)
{
	struct elfhdr header = {
		.magic = ELF_MAGIC,
		.type = type,
		.machine = ELF_MACHINE_RISCV,
		.version = ELF_VERSION_CURRENT,
		.entry = 0x200,
		.phoff = sizeof(struct elfhdr),
		.ehsize = sizeof(struct elfhdr),
		.phentsize = sizeof(struct proghdr),
		.phnum = phnum,
	};

	header.elf[0] = ELF_CLASS_64;
	header.elf[1] = ELF_DATA_LSB;
	header.elf[2] = ELF_VERSION_CURRENT;
	return header;
}

static struct proghdr phdr_program(uint16 phnum)
{
	return (struct proghdr){
		.type = ELF_PROG_PHDR,
		.flags = ELF_PROG_FLAG_READ,
		.off = sizeof(struct elfhdr),
		.vaddr = sizeof(struct elfhdr),
		.filesz = (uint64)phnum * sizeof(struct proghdr),
		.memsz = (uint64)phnum * sizeof(struct proghdr),
		.align = sizeof(uint64),
	};
}

static struct proghdr load_program(uint64 offset, uint64 address,
				   uint64 filesz, uint64 memsz,
				   uint32 flags)
{
	return (struct proghdr){
		.type = ELF_PROG_LOAD,
		.flags = flags,
		.off = offset,
		.vaddr = address,
		.filesz = filesz,
		.memsz = memsz,
		.align = 0x1000,
	};
}

static void test_dynamic_layout(void)
{
	struct elfhdr header = valid_header(ELF_TYPE_DYN, 3);
	struct elf_image_layout image;
	struct elf_runtime_layout runtime;
	struct proghdr program;

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = phdr_program(header.phnum);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	program = load_program(0, 0, 0x880, 0x880,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	program = load_program(0x900, 0x900, 0x100, 0x800,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_WRITE);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	CHECK(elf_image_layout_finish(&image) == 0);
	CHECK(image.map_start == 0 && image.map_end == 0x2000);
	CHECK(image.phdr == sizeof(struct elfhdr));

	CHECK(elf_runtime_layout(&image, 0x10000000, 0x40000000,
				 &runtime) == 0);
	CHECK(runtime.load_bias == 0x10000000);
	CHECK(runtime.map_start == 0x10000000);
	CHECK(runtime.map_end == 0x10002000);
	CHECK(runtime.phdr == 0x10000000 + sizeof(struct elfhdr));
	CHECK(runtime.entry == 0x10000200);
}

static void test_exec_layout(void)
{
	struct elfhdr header = valid_header(ELF_TYPE_EXEC, 2);
	struct elf_image_layout image;
	struct elf_runtime_layout runtime;
	struct proghdr program;

	header.entry = 0x10200;
	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = load_program(0, 0x10000, 0x800, 0x1000,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	program = load_program(0x1000, 0x11000, 0x100, 0x1000,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_WRITE);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	CHECK(elf_image_layout_finish(&image) == 0);
	CHECK(elf_runtime_layout(&image, 0x10000, 0x40000000,
				 &runtime) == 0);
	CHECK(!runtime.load_bias && runtime.entry == header.entry);
	CHECK(elf_runtime_layout(&image, 0x20000, 0x40000000,
				 &runtime) < 0);
}

static void test_invalid_headers(void)
{
	struct elfhdr header = valid_header(ELF_TYPE_DYN, 1);
	struct elf_image_layout image;

	header.elf[2] = 0;
	CHECK(elf_image_layout_init(&image, &header) < 0);
	header = valid_header(ELF_TYPE_DYN, 1);
	header.ehsize--;
	CHECK(elf_image_layout_init(&image, &header) < 0);
	header = valid_header(ELF_TYPE_DYN, 0xffff);
	CHECK(elf_image_layout_init(&image, &header) < 0);
	header = valid_header(ELF_TYPE_DYN, 1);
	header.phoff = (uint64)-32;
	CHECK(elf_image_layout_init(&image, &header) < 0);
}

static void test_invalid_segments(void)
{
	struct elfhdr header = valid_header(ELF_TYPE_DYN, 3);
	struct elf_image_layout image;
	struct proghdr program;

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = load_program(0, 0, 0x900, 0x900,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	program = load_program(0x800, 0x800, 0x100, 0x100,
			       ELF_PROG_FLAG_READ);
	CHECK(elf_image_layout_add(&image, &header, &program) < 0);

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = load_program(0, 0, 0x100, 0x100,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	program.align = 3;
	CHECK(elf_image_layout_add(&image, &header, &program) < 0);

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = load_program(0, (uint64)-16, 8, 32,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	CHECK(elf_image_layout_add(&image, &header, &program) < 0);

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = load_program((uint64)-16, 0, 32, 32,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	CHECK(elf_image_layout_add(&image, &header, &program) < 0);
}

static void test_program_headers(void)
{
	struct elfhdr header = valid_header(ELF_TYPE_DYN, 2);
	struct elf_image_layout image;
	struct proghdr program;

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = phdr_program(header.phnum);
	program.vaddr += 8;
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	program = load_program(0, 0, 0x400, 0x400,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	CHECK(elf_image_layout_finish(&image) < 0);

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = load_program(0x1000, 0, 0x400, 0x400,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	CHECK(elf_image_layout_finish(&image) < 0);
}

static void test_runtime_overflow(void)
{
	struct elfhdr header = valid_header(ELF_TYPE_DYN, 1);
	struct elf_image_layout image;
	struct elf_runtime_layout runtime;
	struct proghdr program;

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = load_program(0, 0, 0x400, 0x1000,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	CHECK(elf_image_layout_finish(&image) == 0);
	CHECK(elf_runtime_layout(&image, (uint64)-0xfff, (uint64)-1,
				 &runtime) < 0);
}

static void test_dynamic_alignment(void)
{
	struct elfhdr header = valid_header(ELF_TYPE_DYN, 1);
	struct elf_image_layout image;
	struct elf_runtime_layout runtime;
	struct proghdr program;

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = load_program(0, 0, 0x1000, 0x1000,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	program.align = 0x200000;
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	CHECK(elf_image_layout_finish(&image) == 0);
	CHECK(image.load_align == 0x200000);
	CHECK(elf_runtime_layout(&image, 0x10001000, 0x40000000,
				 &runtime) < 0);
	CHECK(elf_runtime_layout(&image, 0x10000000, 0x40000000,
				 &runtime) == 0);
}

static void test_load_permissions(void)
{
	struct elfhdr header = valid_header(ELF_TYPE_DYN, 3);
	struct elf_image_layout image;
	struct proghdr program;

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = load_program(0, 0, 0x800, 0x1000,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_EXEC);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	program = load_program(0x1000, 0x1000, 0, 0x1000, 0);
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	program = (struct proghdr){
		.type = ELF_PROG_GNU_STACK,
		.flags = ELF_PROG_FLAG_READ | ELF_PROG_FLAG_WRITE |
			 ELF_PROG_FLAG_EXEC,
		.align = 16,
	};
	CHECK(elf_image_layout_add(&image, &header, &program) == 0);
	CHECK(elf_image_layout_finish(&image) == 0);
	CHECK(image.stack_executable);

	CHECK(elf_image_layout_init(&image, &header) == 0);
	program = load_program(0, 0, 0x800, 0x1000,
			       ELF_PROG_FLAG_READ | ELF_PROG_FLAG_WRITE |
			       ELF_PROG_FLAG_EXEC);
	CHECK(elf_image_layout_add(&image, &header, &program) < 0);
}

static void test_interpreter_paths(void)
{
	static const char valid[] = "/lib/ld-musl-riscv64.so.1";
	static const char relative[] = "lib/ld-musl-riscv64.so.1";
	static const char unterminated[] = {'/', 'l', 'i', 'b'};
	static const char embedded[] = {'/', 'l', 0, 'x', 0};

	CHECK(elf_interpreter_path_valid(valid, sizeof(valid), 512));
	CHECK(!elf_interpreter_path_valid(relative, sizeof(relative), 512));
	CHECK(!elf_interpreter_path_valid(unterminated,
					  sizeof(unterminated), 512));
	CHECK(!elf_interpreter_path_valid(embedded, sizeof(embedded), 512));
	CHECK(!elf_interpreter_path_valid(valid, 513, 512));
	CHECK(!elf_interpreter_path_valid("/", 1, 512));
}

int main(void)
{
	test_dynamic_layout();
	test_exec_layout();
	test_invalid_headers();
	test_invalid_segments();
	test_program_headers();
	test_runtime_overflow();
	test_dynamic_alignment();
	test_load_permissions();
	test_interpreter_paths();
	puts("ELF_OK");
	return EXIT_SUCCESS;
}
