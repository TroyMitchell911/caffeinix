PHDRS
{
	text PT_LOAD FILEHDR PHDRS FLAGS(5);
	rodata PT_LOAD FLAGS(4);
	data PT_LOAD FLAGS(4);
}

SECTIONS
{
	. = 0x10000 + SIZEOF_HEADERS;
	.text : { *(.text) } :text
	. = 0x10600;
	.rodata : { *(.rodata) } :rodata
	. = 0x10a00;
	.data : { *(.data) } :data
	/DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
