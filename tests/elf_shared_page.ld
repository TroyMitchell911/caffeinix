PHDRS
{
	text PT_LOAD FILEHDR PHDRS FLAGS(5);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x10000 + SIZEOF_HEADERS;
	.text : { *(.text) *(.rodata) } :text
	. = 0x10800;
	.data : { *(.data) } :data
	/DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
