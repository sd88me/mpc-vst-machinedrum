@ Code cave for the MPC 3.9.1.2 drum-layout patch. Linked at 0x4a7b330 (see make_patch.sh).
@ In: r0 = juce::String*, r2 = its UTF-8 text, lr = return address into the caller.
@ Out: r0 = 1 for "Machinedrum Module", else the result of the original String == "DrumSynth:Multi" check.
        .arm
        .text
        .globl _start
_start:
        adr   r1, name
1:      ldrb  r3, [r2], #1
        ldrb  ip, [r1], #1
        cmp   r3, ip
        bne   2f
        cmp   r3, #0
        bne   1b
        mov   r0, #1
        bx    lr
2:      ldr   r1, L1
        add   r1, pc, r1
        ldr   ip, L2
        add   pc, pc, ip
L1:     .word 0x4ab2760 - (0x4a7b330 + 40 + 8)
L2:     .word 0x9289ec  - (0x4a7b330 + 48 + 8)
name:   .asciz "Machinedrum Module"
