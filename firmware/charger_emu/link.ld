MEMORY {
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 63K  /* the last 1 KB page holds the settings */
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 20K
}
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  _sidata = LOADADDR(.data);
  .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  /DISCARD/ : { *(.ARM.*) *(.comment) }
}
