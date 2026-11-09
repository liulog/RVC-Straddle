OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
    . = 0x80200000;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : {
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*) *(.data .data.*)
    }
    . = ALIGN(4096);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
    . = ALIGN(8);
    __bss_end = .;
    ASSERT(__bss_end < 0x88000000, "payload exceeds 128 MiB RAM")
}
