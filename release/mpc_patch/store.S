@ At 0x24fe2e4 (the caller of the name check stores its result): strb r0,[r4,#3100] -> str r0,[r4,#3100].
@ r4 = program + 4096. Byte 3100 is the drum flag (both getters read only that byte); 3101-3103 are padding before the
@ 8-byte field at 3104 (no code reads or writes them). The name check returns 0, 1 (DrumSynth:Multi) or 0x101
@ (Machinedrum Module), so byte 3101 marks Machinedrum for colours.S.
        .arm
        .text
        .globl _start
_start:
        str   r0, [r4, #3100]
