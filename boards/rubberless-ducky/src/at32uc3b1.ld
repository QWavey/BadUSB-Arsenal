/* ============================================================================
 * Linker Script for AT32UC3B1 (256K Flash / 32K RAM)
 * Custom HID Firmware — linked AFTER the DFU bootloader
 *
 * The factory DFU bootloader occupies the first 8 KB (0x80000000..0x80001FFF).
 * User application must start at 0x80002000 to avoid overwriting the bootloader.
 *
 * Memory layout:
 *   FLASH:  0x80002000 .. 0x8003FFFF  (248 KB usable for application)
 *   SRAM:   0x00000000 .. 0x00007FFF  ( 32 KB)
 *   USBB:   0xFFFE0000 (peripheral, not managed here)
 *
 * dfu-programmer will place the .hex file at the address specified here
 * (it strips the DFU bootloader region automatically).
 * ========================================================================= */

OUTPUT_FORMAT("elf32-avr32", "elf32-avr32", "elf32-avr32")
OUTPUT_ARCH(avr32:uc)
ENTRY(_start)

/* ---------------------------------------------------------------------------
 * Memory regions
 * ------------------------------------------------------------------------- */
MEMORY {
    FLASH (rx)  : ORIGIN = 0x80002000, LENGTH = 248K
    SRAM  (rwx) : ORIGIN = 0x00000000, LENGTH = 32K
}

/* ---------------------------------------------------------------------------
 * Section layout
 * ------------------------------------------------------------------------- */
SECTIONS {

    /* ---- Interrupt vector table (must be first in flash) ---- */
    .vectors :
    {
        . = ALIGN(4);
        KEEP(*(.vectors))
        KEEP(*(.vectors.*))
    } > FLASH

    /* ---- Read-only code and rodata ---- */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* ---- C++ / GCC exception tables (not really needed for bare-metal) ---- */
    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > FLASH

    /* ---- Initialized data: stored in FLASH, copied to SRAM at startup ---- */
    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH

    /* ---- Zero-initialized (BSS) data ---- */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    /* ---- Stack at top of SRAM ---- */
    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        _stack_end = .;
        . = . + 2048;        /* 2 KB stack */
        . = ALIGN(8);
        _stack_start = .;
    } > SRAM

    /* ---- Discard unwanted sections ---- */
    /DISCARD/ :
    {
        *(.note.*)
        *(.comment)
        *(.debug*)
    }
}

/* Symbols used by startup code */
PROVIDE(_stack = _stack_start);
PROVIDE(__stack = _stack_start);
PROVIDE(__heap_start__ = _ebss);
PROVIDE(__heap_end__   = _stack_end);
