.syntax unified
.cpu cortex-m7
.fpu fpv5-d16
.thumb

.global g_pfnVectors
.global Reset_Handler
.extern Boot_Main
.extern __boot_ram_load__
.extern __boot_ram_start__
.extern __boot_ram_end__
.extern __bss_start__
.extern __bss_end__
.extern _estack

.section .isr_vector,"a",%progbits
.type g_pfnVectors, %object
g_pfnVectors:
    .word _estack
    .word Reset_Handler
    .rept 14
    .word Default_Handler
    .endr
.size g_pfnVectors, .-g_pfnVectors

.section .reset_handler,"ax",%progbits
.thumb_func
Reset_Handler:
    ldr r0, =__boot_ram_load__
    ldr r1, =__boot_ram_start__
    ldr r2, =__boot_ram_end__
1:
    cmp r1, r2
    bcs 2f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 1b
2:
    ldr r0, =__bss_start__
    ldr r1, =__bss_end__
    movs r2, #0
3:
    cmp r0, r1
    bcs 4f
    str r2, [r0], #4
    b 3b
4:
    bl Boot_Main
5:
    b 5b

.thumb_func
Default_Handler:
    b Default_Handler
