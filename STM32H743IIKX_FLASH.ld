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
_Min_Heap_Size = 0x1000;      /* required amount of heap  */
_Min_Stack_Size = 0x2000; /* required amount of stack */

/* Specify the memory areas */
MEMORY
{
  /* The device has 2048K of flash, but only the first 128K belong to the
     bootloader: the application starts at 0x08020000 (IAP_APP_ADDRESS in
     Core/Inc/usbd_cdc_flash.h, and build.flash_offset=0x20000 in the Arduino
     core's platform.txt). Declaring the full 2048K here would let the linker
     place bootloader code on top of the application without a word of warning.
     With 128K the linker reports "region FLASH overflowed" instead.
     Raising this limit means raising the application base in all three places
     at once -- here, usbd_cdc_flash.h, and platform.txt. */
  FLASH (rx)     : ORIGIN = 0x08000000, LENGTH = 128K
  DTCMRAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D1 (xrw)   : ORIGIN = 0x24000000, LENGTH = 512K
  RAM_D2 (xrw)   : ORIGIN = 0x30000000, LENGTH = 288K
  /* First 32 bytes of SRAM4 are carved out of the region on purpose: they hold
     the boot-mode handoff record (see IAPServer/IAP_boot_handoff.h). Because the
     address is outside every MEMORY region the linker cannot place anything
     there, in this image or any future one -- a guarantee, not a convention.
     Keep this identical to the Arduino core's variants/.../ldscript.ld. */
  RAM_D3 (xrw)   : ORIGIN = 0x38000020, LENGTH = 64K - 32
  ITCMRAM (xrw)  : ORIGIN = 0x00000000, LENGTH = 64K
}

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

  /* ETH_CODE: add placement of DMA descriptors and RX buffers.
   *
   * Layout of RAM_D2 (288K). The order is deliberate:
   *
   *   0x30000000  lwIP heap       131048 + 24            = 131072 B -> 0x3001FFFF
   *   0x30020000  Rx descriptors  ETH_RX_DESC_CNT(4)  * 24 =    96 B -> 0x3002005F
   *   0x30020060  Tx descriptors  ETH_TX_DESC_CNT(4)  * 24 =    96 B -> 0x300200BF
   *   0x30020100  Rx pbuf pool    ETH_RX_BUFFER_CNT(12) * sizeof(RxBuff_t)(32+1536)
   *                                                      = 18816 B -> 0x30024A7F
   *   0x30024A80  free: 141K, contiguous
   *
   * The heap comes first because an MPU region must be aligned to its own size.
   * The 128K region that marks the heap non-cacheable can therefore only start
   * at 0x30000000 or 0x30020000 -- put the descriptors first and the heap is
   * forced up to 0x30020000, stranding the ~108K in between. Descriptors and
   * pool are fixed-size, so nothing here moves unless MEM_SIZE changes.
   *
   * The heap occupies MEM_SIZE_ALIGNED + 2 * SIZEOF_STRUCT_MEM, not MEM_SIZE:
   * lwIP adds two 12-byte sentinel headers (mem_size_t becomes u32_t once
   * MEM_SIZE exceeds 64000). MEM_SIZE 131048 is picked so that 131048 + 24 is
   * exactly 128K -- raising it without growing the MPU region would push the
   * heap into the descriptors.
   *
   * These three addresses are the single source of truth for where the data
   * really lives; the ETH_*Address values in the .ioc only feed CubeMX's MPU
   * region generation. Keep both in step -- and note that an overlap here is
   * silent, because the lwIP heap is a raw pointer, not a linker section, so ld
   * cannot catch it.
   */
  .lwip_sec (NOLOAD) :
  {
    . = ABSOLUTE(0x30020000);
    *(.RxDecripSection)

    . = ABSOLUTE(0x30020060);
    *(.TxDecripSection)

    . = ABSOLUTE(0x30020100);
    *(.Rx_PoolSection)
  } >RAM_D2


  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}


