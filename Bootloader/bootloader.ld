ENTRY(Reset_Handler)

MEMORY
{
  BOOT_FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
  BOOT_RAM (xrw)  : ORIGIN = 0x24000000, LENGTH = 384K
  STACK_RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(STACK_RAM) + LENGTH(STACK_RAM);
_Min_Stack_Size = 0x2000;
_Min_Heap_Size = 0x8000;

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(1024);
    KEEP(*(.isr_vector))
  } > BOOT_FLASH

  .reset_handler :
  {
    KEEP(*(.reset_handler))
    . = ALIGN(4);
  } > BOOT_FLASH

  __boot_ram_load__ = LOADADDR(.boot_ram);
  .boot_ram :
  {
    __boot_ram_start__ = .;
    *(.text*)
    *(.rodata*)
    *(.glue_7)
    *(.glue_7t)
    *(.ARM.extab*)
    *(.ARM.exidx*)
    *(.data*)
    . = ALIGN(8);
    __boot_ram_end__ = .;
  } > BOOT_RAM AT > BOOT_FLASH

  .bss (NOLOAD) :
  {
    __bss_start__ = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(8);
    __bss_end__ = .;
    end = .;
    _end = .;
    . = . + _Min_Heap_Size;
    __heap_end__ = .;
  } > BOOT_RAM

  .stack (NOLOAD) :
  {
    . = ALIGN(8);
    . = . + _Min_Stack_Size;
  } > STACK_RAM

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
  }
}

ASSERT(SIZEOF(.isr_vector) + SIZEOF(.reset_handler) +
       SIZEOF(.boot_ram) <= LENGTH(BOOT_FLASH),
       "Bootloader exceeds Bank 1 sector 0")
ASSERT(__heap_end__ <= ORIGIN(BOOT_RAM) + LENGTH(BOOT_RAM),
       "Bootloader RAM image and heap exceed BOOT_RAM")
