ENTRY(_start)

PHDRS
{
	phdr PT_PHDR PHDRS FLAGS(4);
	text PT_LOAD FILEHDR PHDRS FLAGS(5);
}

SECTIONS
{
	__image_base = .;
	. = SIZEOF_HEADERS;
	.text : { *(.text) *(.rodata) } :text
	.dynamic : { *(.dynamic) } :text
	.dynsym : { *(.dynsym) } :text
	.dynstr : { *(.dynstr) } :text
	.hash : { *(.hash) *(.gnu.hash) } :text
	.rela.dyn : { *(.rela.dyn) } :text
	/DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
