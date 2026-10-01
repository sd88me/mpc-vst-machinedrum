@ Replaces the 16-byte helper at 0x2494954 (only caller 0x24fe2dc): jump to the cave.
        .arm
        .text
        .globl _start
_start:
        ldr   r2, [r0]
        ldr   ip, [pc, #0]
        add   pc, pc, ip
        .word 0x4a7b330 - 0x2494964
