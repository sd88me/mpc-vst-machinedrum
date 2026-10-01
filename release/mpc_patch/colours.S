@ Pad colours for a plugin drum program (MPC 3.9.1.2). Linked at 0x6872900: zero padding after the R+X LOAD segment's
@ file data (ends 0x687288c), inside its last mapped page (which ends 0x6873000).
@ Entered from 0x2494bb4 (colours_jump.S), the start of Akai's loop that colours pads 0-7 from the 8-entry table at
@ 0x4e3fb68 ({pad, pad, rgb} x 8). In: r5 = the closure {?, ProgramPads* (= program + 5440), isDrum}.
@ Machinedrum Module (mark byte program + 7197 = ProgramPads + 1757, set by store.S): all 16 pads one orange, like its
@ LCD. Anything else: Akai's loop, unchanged (re-do the three instructions the jump replaced and go back into it).
@ r4/r6 are saved by the caller's prologue, so they are free here.
        .arm
        .text
        .globl _start
        .equ  ORANGE, 0x7f3300
_start:
        ldr   r0, [r5, #4]          @ ProgramPads
        ldrb  r3, [r0, #1757]       @ Machinedrum mark
        cmp   r3, #0
        bne   md
        ldr   r6, Ltab              @ Akai's path: the replaced instructions
        add   r6, pc, r6            @ r6 = 0x4e3fb68
        add   r4, r6, #12
        ldr   ip, Lloop
        add   pc, pc, ip            @ -> 0x2494bc0 (add r6, r6, #108; the loop)
md:     mov   r4, #0
1:      ldr   r0, [r5, #4]
        mov   r1, r4
        ldr   r2, Lrgb
        ldr   ip, Lset
        add   ip, pc, ip            @ ip = 0x2462e70 (set pad colour)
        blx   ip
        add   r4, r4, #1
        cmp   r4, #16
        bne   1b
        ldr   r0, [r5, #4]          @ as Akai's loop leaves it
        ldr   ip, Lret
        add   pc, pc, ip            @ -> 0x2494be0
Ltab:   .word 0x4e3fb68 - (0x6872900 + 20 + 8)
Lloop:  .word 0x2494bc0 - (0x6872900 + 32 + 8)
Lset:   .word 0x2462e70 - (0x6872900 + 56 + 8)
Lret:   .word 0x2494be0 - (0x6872900 + 84 + 8)
Lrgb:   .word ORANGE
