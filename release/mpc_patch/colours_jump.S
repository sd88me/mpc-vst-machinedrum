@ At 0x2494bb4 (start of the 8-pad colour loop): jump to the 16-pad loop at 0x6872900.
        .arm
        .text
        .globl _start
_start:
        ldr   ip, [pc, #0]
        add   pc, pc, ip
        .word 0x6872900 - 0x2494bc0
