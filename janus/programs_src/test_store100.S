/*
 * LinxISA (LLVM-assembled) smoke test: store 100 to mem[0x100] then halt.
 *
 * Note: LinxISA SWI immediates in assembly are byte offsets; the encoded field
 * is word-scaled and the core model applies <<2 internally.
 */

    .section .text.start, "ax", @progbits
    .global _start
    .p2align 1
    .type _start, @function

_start:
    C.BSTART    FALL

    /* a0 = 100 */
    addi    zero, 100, ->a0

    /* mem[0x100] = a0 */
    swi     a0, [zero, 256]

    /* Halt (Janus bring-up model treats any EBREAK as stop). */
    ebreak  0

    C.BSTOP
    .size _start, . - _start

