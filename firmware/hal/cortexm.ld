/*
 * cortexm.ld - Cortex-M4 with 128 KB flash and 32 KB RAM.
 *
 * The stack is placed at the top of RAM and grows down toward .bss. The
 * _min_heap_stack guard at the end makes the link fail if statics have grown
 * far enough to leave no usable stack, which is a far better place to find
 * that out than at runtime.
 */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_min_stack_size = 2K;

ENTRY(Reset_Handler)

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  /* ARM unwind tables. Discarded would be wrong: the linker emits
   * references to them and dropping the sections breaks the link. */
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /* Fails the link rather than the field if statics crowd out the stack. */
  .stack_guard :
  {
    . = ALIGN(8);
    . = . + _min_stack_size;
    . = ALIGN(8);
  } > RAM

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
  }
}
