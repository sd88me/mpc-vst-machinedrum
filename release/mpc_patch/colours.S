@ 16 pad colours for a plugin drum program (MPC 3.9.1.2). Linked at 0x6872900: zero padding after the R+X LOAD
@ segment's file data (ends 0x687288c), inside its last mapped page (which ends 0x6873000).
@ Replaces the loop at 0x2494bb4-0x2494bdc that colours pads 0-7 from the 8-entry table at 0x4e3fb68
@ ({pad, pad, rgb} x 8). Here: pad i (0-15) gets table[i & 7].rgb. In: r5 = the caller's object, [r5,#4] = ProgramPads.
@ r4/r6 are saved by the caller's prologue, so they are free here.
        .arm
        .text
        .globl _start
_start:
        ldr   r6, Ltab
        add   r6, pc, r6            @ r6 = 0x4e3fb68
        mov   r4, #0
1:      and   r3, r4, #7
        add   r3, r3, r3, lsl #1    @ *3 words = 12 bytes
        add   r3, r6, r3, lsl #2
        ldr   r2, [r3, #8]          @ rgb
        mov   r1, r4                @ pad
        ldr   r0, [r5, #4]          @ ProgramPads
        ldr   ip, Lset
        add   ip, pc, ip            @ ip = 0x2462e70 (set pad colour)
        blx   ip
        add   r4, r4, #1
        cmp   r4, #16
        bne   1b
        ldr   r0, [r5, #4]          @ as the original loop leaves it
        ldr   ip, Lret
        add   pc, pc, ip            @ back to 0x2494be0
Ltab:   .word 0x4e3fb68 - (0x6872900 + 4 + 8)
Lset:   .word 0x2462e70 - (0x6872900 + 40 + 8)
Lret:   .word 0x2494be0 - (0x6872900 + 68 + 8)
