ENTRY(_start)

PHDRS
{
	text PT_LOAD FILEHDR PHDRS FLAGS(5);
	guard PT_LOAD FLAGS(0);
	stack PT_GNU_STACK FLAGS(7);
}

SECTIONS
{
	. = 0x12000 + SIZEOF_HEADERS;
	.text : { *(.text) *(.rodata) } :text
	. = ALIGN(0x1000);
	__guard_start = .;
	.guard (NOLOAD) : { . += 0x1000; } :guard
	/DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
