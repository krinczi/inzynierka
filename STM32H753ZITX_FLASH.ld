/*
******************************************************************************
**
**  File        : LinkerScript.ld
**
**  Author      : STM32CubeIDE
**
**  Abstract    : Linker script for STM32H7 series
**                2048Kbytes FLASH and 1056Kbytes RAM
**
**                Set heap size, stack size and stack location according
**                to application requirements.
**
**                Set memory bank area and size if external memory is used.
**
**  Target      : STMicroelectronics STM32
**
**  Distribution: The file is distributed as is, without any warranty
**                of any kind.
**
*****************************************************************************
** @attention
**
** Copyright (c) 2024 STMicroelectronics.
** All rights reserved.
**
** This software is licensed under terms that can be found in the LICENSE file
** in the root directory of this software component.
** If no LICENSE file comes with this software, it is provided AS-IS.
**
****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM_D1) + LENGTH(RAM_D1);    /* end of RAM */
/* Generate a link error if heap and stack don't fit into RAM */
_Min_Heap_Size = 0x200;      /* required amount of heap  */
_Min_Stack_Size = 0x400; /* required amount of stack */

/* Specify the memory areas */
MEMORY
{
  /*
   * Application code is restricted to Bank 1. Bank 2 is reserved for
   * download staging and the existing dual-slot configuration store.
   */
  FLASH (rx)              : ORIGIN = 0x08000000, LENGTH = 640K
  FLASH_EVENT_JOURNAL (r) : ORIGIN = 0x080A0000, LENGTH = 128K
  FLASH_CREDENTIALS_A (r) : ORIGIN = 0x080C0000, LENGTH = 128K
  FLASH_CREDENTIALS_B (r) : ORIGIN = 0x080E0000, LENGTH = 128K
  FLASH_STAGING_META (r)  : ORIGIN = 0x08100000, LENGTH = 128K
  FLASH_STAGING_DATA (r)  : ORIGIN = 0x08120000, LENGTH = 640K
  FLASH_CONFIG (r)        : ORIGIN = 0x081C0000, LENGTH = 256K
  DTCMRAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D1 (xrw)   : ORIGIN = 0x24000000, LENGTH = 512K
  RAM_D2 (xrw)   : ORIGIN = 0x30000000, LENGTH = 288K
  RAM_D3 (xrw)   : ORIGIN = 0x38000000, LENGTH = 64K
  ITCMRAM (xrw)  : ORIGIN = 0x00000000, LENGTH = 64K
}

__flash_staging_metadata_start__ = ORIGIN(FLASH_STAGING_META);
__flash_staging_metadata_end__ = ORIGIN(FLASH_STAGING_META) + LENGTH(FLASH_STAGING_META);
__flash_staging_data_start__ = ORIGIN(FLASH_STAGING_DATA);
__flash_staging_data_end__ = ORIGIN(FLASH_STAGING_DATA) + LENGTH(FLASH_STAGING_DATA);
__iot_event_journal_start__ = ORIGIN(FLASH_EVENT_JOURNAL);
__iot_event_journal_end__ = ORIGIN(FLASH_EVENT_JOURNAL) + LENGTH(FLASH_EVENT_JOURNAL);

ASSERT(ORIGIN(FLASH) + LENGTH(FLASH) <= ORIGIN(FLASH_EVENT_JOURNAL),
       "Application overlaps persistent event journal")
ASSERT(ORIGIN(FLASH_EVENT_JOURNAL) + LENGTH(FLASH_EVENT_JOURNAL) <= ORIGIN(FLASH_CREDENTIALS_A),
       "Persistent event journal overlaps credential slot A")
ASSERT(ORIGIN(FLASH) + LENGTH(FLASH) <= ORIGIN(FLASH_CREDENTIALS_A),
       "Application overlaps credential slot A")
ASSERT(ORIGIN(FLASH_CREDENTIALS_A) + LENGTH(FLASH_CREDENTIALS_A) <= ORIGIN(FLASH_CREDENTIALS_B),
       "Credential slots overlap")
ASSERT(ORIGIN(FLASH_CREDENTIALS_B) + LENGTH(FLASH_CREDENTIALS_B) <= ORIGIN(FLASH_STAGING_META),
       "Credential slot B overlaps Bank 2 staging metadata")
ASSERT(ORIGIN(FLASH_STAGING_DATA) + LENGTH(FLASH_STAGING_DATA) <= ORIGIN(FLASH_CONFIG),
       "Flash staging data overlaps device configuration")

/* Define output sections */
SECTIONS
{
  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH

  .ARM.extab (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } >FLASH
  .ARM (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections goes into RAM, load LMA copy after code */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */
    *(.RamFunc)        /* .RamFunc sections */
    *(.RamFunc*)       /* .RamFunc* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */
  } >RAM_D1 AT> FLASH

  /* Ethernet DMA descriptors and RX buffers must be placed in D2 SRAM.
     These fixed addresses match the layout expected by ethernetif.c. */
  .RxDecripSection 0x30000000 (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*(.RxDecripSection))
    . = ALIGN(32);
  } >RAM_D2

  .TxDecripSection 0x30000080 (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*(.TxDecripSection))
    . = ALIGN(32);
  } >RAM_D2

  .Rx_PoolSection 0x30000100 (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*(.Rx_PoolSection))
    . = ALIGN(32);
  } >RAM_D2

  /* Uninitialized data section */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss section */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM_D1

  /* User_heap_stack section, used to check that there is enough RAM left */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM_D1

  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
