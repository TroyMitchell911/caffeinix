ENTRY(_start)

PHDRS
{
	phdr PT_PHDR PHDRS FLAGS(4);
	interp PT_INTERP FLAGS(4);
	text PT_LOAD FILEHDR PHDRS FLAGS(5);
}

SECTIONS
{
	. = 0x20000000 + SIZEOF_HEADERS;
	.interp : { KEEP(*(.interp)) } :text :interp
	.text : { *(.text) *(.rodata) } :text
	.dynamic : { *(.dynamic) } :text
	.dynsym : { *(.dynsym) } :text
	.dynstr : { *(.dynstr) } :text
	.hash : { *(.hash) *(.gnu.hash) } :text
	.rela.dyn : { *(.rela.dyn) } :text
	/DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
