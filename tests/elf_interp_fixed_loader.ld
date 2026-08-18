ENTRY(_start)

PHDRS
{
	phdr PT_PHDR PHDRS FLAGS(4);
	text PT_LOAD FILEHDR PHDRS FLAGS(5);
}

SECTIONS
{
	. = 0x30000000 + SIZEOF_HEADERS;
	.text : { *(.text) *(.rodata) } :text
	/DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
