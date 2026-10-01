@ Pad colours for a plugin drum program (MPC 3.9.1.2). Linked at 0x6872900: zero padding after the R+X LOAD segment's
@ file data (ends 0x687288c), inside its last mapped page (which ends 0x6873000). Nothing references it.
@ Replaces the loop at 0x2494bb4-0x2494dc that colours pads 0-7 from Akai's 8-entry table. Here: all 16 pads are set to
@ one red, close to the Machinedrum's LCD. In: r5 = the lambda's closure, [r5,#4] = ProgramPads. r4 is caller-saved.
@ This runs for every plugin drum program (so Akai's DrumSynth also gets 16 red pads); telling plugins apart at colour
@ time needs the program's identity, which is not reachable from here (see docs/HANDOFF-mpc-drum-pads.md).
        .arm
        .text
        .globl _start
        .equ  RED, 0x7f0000
_start:
        mov   r4, #0
1:      ldr   r0, [r5, #4]          @ ProgramPads
        mov   r1, r4                @ pad
        ldr   r2, Lrgb
        ldr   ip, Lset
        add   ip, pc, ip            @ ip = 0x2462e70 (set pad colour)
        blx   ip
        add   r4, r4, #1
        cmp   r4, #16
        bne   1b
        ldr   r0, [r5, #4]          @ as the original loop leaves it
        ldr   ip, Lret
        add   pc, pc, ip            @ back to 0x2494be0
Lset:   .word 0x2462e70 - (0x6872900 + 28 + 8)
Lret:   .word 0x2494be0 - (0x6872900 + 48 + 8)
Lrgb:   .word RED
