ENTRY(main);

PHDRS
{
  text PT_LOAD FILEHDR PHDRS;
  data PT_LOAD;
  dynamic PT_DYNAMIC;
  loader_comm PT_NOTE;
}

SECTIONS
{
  . = 0x802000 + SIZEOF_HEADERS;
  self_elf_start = . - SIZEOF_HEADERS;
  .text : {
    *(.start)
    *(.text.main)
    *(.text*)
    *(.syscall_impl)
    . = ALIGN(0x40);
    *(.orig)
    /* These alignment directives between data and text are needed to work
     * around what seems to be a CPU bug (!!) */
    . = ALIGN(0x40);
    *(.rodata*)
    *(.data.rel.ro*)
    *(.notrodata)
  } :text
  .loader_comm : { KEEP(*(.loader_comm)) } :loader_comm :text
  .ctors : {
    __ctors_start = .;
    KEEP(*(.ctors*))
    __ctors_end = .;
    ASSERT(__ctors_end - __ctors_start == 0, "mod.ld: we have constructors!");
  }
  . = ALIGN(0x40);
  .got : { *(.got) } :text
  .got2 : { *(.got2) } :text
  .plt : {
    LONG(0xdeadbeef) /* This is needed. Can you guess why? */
    *(.plt)
  } :text =0xde
  . += 4; /* avoid ambiguity between a reference to end of text or start of data */
  . = ALIGN(0x40);
  .data : {
    data_start = .;
    *(.data*)
  } :data
  .dynamic : { *(.dynamic) } :data :dynamic
  .gnu.version : { *(.gnu.version) } :data
  .gnu.version_r : { *(.gnu.version_r) } :data
  /DISCARD/ : { *(.comment) }
  .bss : {
    bss_start = .;
    *(.bss*)
    bss_end = .;
  }
}

ENTRY(main)
