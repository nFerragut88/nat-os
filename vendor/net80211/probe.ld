MEMORY {
  irom (rx)  : ORIGIN = 0x400D0000, LENGTH = 0x300000
  dram (rw)  : ORIGIN = 0x3FFB0000, LENGTH = 0x30000
}
SECTIONS {
  .text   : { *(.literal .literal.* .text .text.*) }   > irom
  .rodata : { *(.rodata .rodata.*) }                    > irom
  .data   : { *(.data .data.*) }                        > dram
  .bss    : { *(.bss .bss.* COMMON) }                   > dram
}
