OUTPUT_ARCH( "riscv" )
ENTRY( _entry )

PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  /*
   * Keep the RV64 payload 2 MiB aligned and clear of resident OpenSBI.
   */
  . = 0x80200000;

  .text : {
    *(.text .text.*)
    . = ALIGN(0x1000);
    /* _trampoline = .; */
    *(trampsec)
    . = ALIGN(0x1000);
    *(sigtrampsec)
    . = ALIGN(0x1000);
    PROVIDE(etext = .);
  } :text
  

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  } :data

  .eh_frame : {
    *(.eh_frame .eh_frame.*)
  } :data

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
  } :data

  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
  } :data

  PROVIDE(end = .);
  ASSERT(etext == ADDR(.text) + SIZEOF(.text),
         "etext does not cover the final text section")
  ASSERT(end == ADDR(.bss) + SIZEOF(.bss),
         "end does not cover the final BSS section")
}
