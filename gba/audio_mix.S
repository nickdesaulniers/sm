@ The mixer's inner loops (see audio_mix.iwram.c), by hand: in C, clang
@ runs out of registers for them, and spills.
@
@     void Audio_MixVoice(int32_t *sum, int n, AudioVoice *v);
@
@ Adds |n| (even) of a voice's samples, times its volume, to |sum|, as the
@ loop in Audio_MixChunk does, two at a time; or, played slower than half
@ the mixer's rate, those between its samples, a sample at a time. At the
@ end of the sample, it goes back to its loop, or is done (v->on false).
@ Silent (v->vol 0), it only moves on.
@
@     void Audio_EchoPairs(int32_t *wet, int pairs, uint32_t *buf, EchoState *s);
@
@ For each pair of samples: the echo buffer's word (both sides, 16 bits
@ each) and the one before it go through the filter (s->fir0 on the new,
@ s->fir1 on the old, /128, clamped); that's the pair's second sample of
@ echo, and the first is halfway from the last one. Both go to wet[48] on
@ (kAudioChunk on). Then the pair's average of |wet| (the voices' echo), and
@ the filtered echo at s->feedback (/128), clamped, go back in the buffer.
@ |wet| and the echo are as the voices' sums: right << 16 + left.
@
@     void Audio_OutputDry(int8_t *left, int8_t *right, const int32_t *sums, int n,
@                          const int32_t vol[4]);
@     void Audio_OutputEcho(int8_t *left, int8_t *right, const int32_t *sums, int n,
@                           const int32_t vol[4]);
@
@ The mixer's last step, as Audio_MixChunk has it: |n| samples of the
@ voices' sums (both sides in a word, as above), at the master volumes
@ (vol[0] left, vol[1] right), /8192, clamped, into |left| and |right|.
@ With the echo, its voices' sums are at sums[48] on (kAudioChunk on),
@ added in, and the echo's output at sums[96] on, at the echo volumes
@ (vol[2], vol[3]).

    .syntax unified
    .arm
    .section .iwram.text.mix, "ax", %progbits
    .p2align 2

    .equ OUT, 48 * 4            @ kAudioChunk words

    @ AudioVoice
    .equ V_PCM, 0
    .equ V_POS, 4
    .equ V_STEP, 8
    .equ V_END, 12
    .equ V_LOOP_LEN, 16
    .equ V_VOL, 20
    .equ V_ON, 24

    @ EchoState
    .equ S_FIR0, 0
    .equ S_FIR1, 4
    .equ S_FEEDBACK, 8
    .equ S_OLD, 12              @ the buffer's last word
    .equ S_LAST, 16             @ the last echo sample

@ Clamps \x to 16 bits, with \t a temporary and r8 0x7FFF.
.macro clamp16 x, t
    mov     \t, \x, lsl #16
    cmp     \x, \t, asr #16
    eorne   \x, r8, \x, asr #31
.endm

    .global Audio_MixVoice
    .type Audio_MixVoice, %function
Audio_MixVoice:
    push    {r4-r11, lr}
    ldmia   r2, {r3-r6}         @ pcm, pos, step, end
    ldr     r7, [r2, #V_VOL]
    cmp     r7, #0
    beq     20f
    cmp     r5, #0x8000
    blo     10f
    @ Whether it gets to the end in these |n|: if not, no need to look.
    mla     r8, r5, r1, r4
    cmp     r8, r6
    bhs     1f
    @ (The volume multiplies: the ARM's multiply is quicker by a small
    @ number, as the sample is, than by one as big as the volume.)
0:
    add     r8, r3, r4, lsr #16
    ldrsb   r8, [r8]
    add     r4, r4, r5
    add     r9, r3, r4, lsr #16
    ldrsb   r9, [r9]
    add     r4, r4, r5
    ldmia   r0, {r10, r12}
    mla     r10, r7, r8, r10
    mla     r12, r7, r9, r12
    stmia   r0!, {r10, r12}
    subs    r1, r1, #2
    bne     0b
    b       6f
1:
    add     r8, r3, r4, lsr #16
    ldrsb   r8, [r8]
    add     r4, r4, r5
    cmp     r4, r6
    bhs     3f
2:
    add     r9, r3, r4, lsr #16
    ldrsb   r9, [r9]
    add     r4, r4, r5
    cmp     r4, r6
    bhs     5f
4:
    ldmia   r0, {r10, r12}
    mla     r10, r7, r8, r10
    mla     r12, r7, r9, r12
    stmia   r0!, {r10, r12}
    subs    r1, r1, #2
    bne     1b
6:
    str     r4, [r2, #V_POS]
    pop     {r4-r11, lr}
    bx      lr
    @ At the end of the sample, after the first of two: back to its loop,
    @ or the voice is done, with that one.
3:
    ldr     r10, [r2, #V_LOOP_LEN]
    cmp     r10, #0
    sub     r4, r4, r10
    bne     2b
    ldr     r10, [r0]
    mla     r10, r7, r8, r10
    str     r10, [r0]
    b       7f
    @ After the second.
5:
    ldr     r10, [r2, #V_LOOP_LEN]
    cmp     r10, #0
    sub     r4, r4, r10
    bne     4b
    ldmia   r0, {r10, r12}
    mla     r10, r7, r8, r10
    mla     r12, r7, r9, r12
    stmia   r0, {r10, r12}
7:
    mov     r10, #0
    strb    r10, [r2, #V_ON]
    b       6b

    @ Slower than half the mixer's rate, each of its samples lasts two or
    @ more of the mixer's: halfway between them, by how far it's come (as
    @ the SNES's interpolation does, more smoothly), not in steps, which
    @ sound as a buzz at the sample's rate. r8 is the sample at r10, r9 how
    @ far the next is from it.
10:
    mvn     r10, #1             @ (not a sample's, nor the one before)
    mla     r8, r5, r1, r4
    cmp     r8, r6
    bhs     11f
    @ Not to the end in these |n|: two at a time, not looking.
17:
    cmp     r10, r4, lsr #16
    blne    15f
    mov     r11, r4, lsl #16
    mov     r11, r11, lsr #24
    mul     r12, r9, r11
    add     r6, r8, r12, asr #8
    add     r4, r4, r5
    cmp     r10, r4, lsr #16
    blne    15f
    mov     r11, r4, lsl #16
    mov     r11, r11, lsr #24
    mul     r12, r9, r11
    add     r12, r8, r12, asr #8
    add     r4, r4, r5
    ldmia   r0, {r11, lr}
    mla     r11, r7, r6, r11
    mla     lr, r7, r12, lr
    stmia   r0!, {r11, lr}
    subs    r1, r1, #2
    bne     17b
    b       6b
11:
    cmp     r10, r4, lsr #16
    blne    15f
    mov     r11, r4, lsl #16
    mov     r11, r11, lsr #24
    mul     r12, r9, r11
    add     r12, r8, r12, asr #8
    ldr     r11, [r0]
    mla     r11, r7, r12, r11
    str     r11, [r0], #4
    add     r4, r4, r5
    cmp     r4, r6
    bhs     16f
13:
    subs    r1, r1, #1
    bne     11b
    b       6b
16:
    ldr     r11, [r2, #V_LOOP_LEN]
    cmp     r11, #0
    sub     r4, r4, r11
    bne     13b
    b       7b
    @ The sample at r4, and the next: the one that was next, mostly; after
    @ the last, its loop's first, or the last again.
15:
    mov     r11, r4, lsr #16
    add     r12, r10, #1
    cmp     r11, r12
    addeq   r8, r8, r9
    ldrsbne r8, [r3, r11]
    mov     r10, r11
    ldr     r12, [r2, #V_END]
    add     r11, r11, #1
    cmp     r11, r12, lsr #16
    bhs     18f
14:
    ldrsb   r9, [r3, r11]
    sub     r9, r9, r8
    bx      lr
18:
    ldr     r12, [r2, #V_LOOP_LEN]
    movs    r12, r12, lsr #16
    moveq   r11, r10
    sub     r11, r11, r12
    b       14b

    @ Silent (its envelope or volume at 0: of the voices mixed, 30% in
    @ Norfair, 42% in Maridia): it plays on, unheard, as on the SNES.
20:
    mla     r4, r5, r1, r4
    cmp     r4, r6
    blo     6b
    ldr     r10, [r2, #V_LOOP_LEN]
    cmp     r10, #0
    beq     7b
21:
    sub     r4, r4, r10
    cmp     r4, r6
    bhs     21b
    b       6b
    .size Audio_MixVoice, . - Audio_MixVoice

    .global Audio_EchoPairs
    .type Audio_EchoPairs, %function
Audio_EchoPairs:
    push    {r4-r11, lr}
    ldr     r4, [r3, #S_FIR1]
    ldr     r5, [r3, #S_FEEDBACK]
    ldr     r6, [r3, #S_OLD]
    ldr     r7, [r3, #S_LAST]
    push    {r3}
    ldr     r3, [r3, #S_FIR0]
    mov     r8, #0x7f00
    orr     r8, r8, #0xff
1:
    ldr     r9, [r2]            @ the buffer's word
    @ Left: r12 = (l0 * fir0 + l1 * fir1) >> 7
    mov     r10, r9, lsl #16
    mov     r10, r10, asr #16
    mov     r11, r6, lsl #16
    mov     r11, r11, asr #16
    mul     r12, r10, r3
    mla     r12, r11, r4, r12
    mov     r12, r12, asr #7
    clamp16 r12, r10
    @ Right: lr
    mov     r10, r9, asr #16
    mov     r11, r6, asr #16
    mul     lr, r10, r3
    mla     lr, r11, r4, lr
    mov     lr, lr, asr #7
    clamp16 lr, r10
    mov     r6, r9
    @ The pair's echo: halfway from the last, then the new.
    mov     r10, r7, lsl #16
    mov     r10, r10, asr #16   @ last left
    sub     r11, r7, r10
    mov     r11, r11, asr #16   @ last right
    add     r10, r10, r12
    mov     r10, r10, asr #1
    add     r11, r11, lr
    mov     r11, r11, asr #1
    add     r10, r10, r11, lsl #16
    add     r7, r12, lr, lsl #16
    str     r10, [r0, #OUT]
    str     r7, [r0, #OUT + 4]
    @ The pair's voices' echo, both sides added: r11 left, r9 right.
    ldr     r9, [r0]
    mov     r11, r9, lsl #16
    mov     r11, r11, asr #16
    sub     r9, r9, r11
    mov     r9, r9, asr #16
    ldr     r10, [r0, #4]
    add     r9, r9, r10, asr #16
    mov     r10, r10, lsl #16
    mov     r10, r10, asr #16
    add     r9, r9, r10, lsr #31    @ (the left's borrow from the right)
    add     r11, r11, r10
    @ Halved, with the feedback, into the buffer.
    mul     r10, r12, r5
    mov     r11, r11, asr #1
    add     r11, r11, r10, asr #7
    clamp16 r11, r10
    mul     r10, lr, r5
    mov     r9, r9, asr #1
    add     r9, r9, r10, asr #7
    clamp16 r9, r10
    mov     r11, r11, lsl #16
    mov     r11, r11, lsr #16
    orr     r11, r11, r9, lsl #16
    str     r11, [r2], #4
    add     r0, r0, #8
    subs    r1, r1, #1
    bne     1b

    pop     {r3}
    str     r6, [r3, #S_OLD]
    str     r7, [r3, #S_LAST]
    pop     {r4-r11, lr}
    bx      lr
    .size Audio_EchoPairs, . - Audio_EchoPairs

@ Clamps \x to 8 bits, with \t a temporary and r8 127.
.macro clamp8 x, t
    mov     \t, \x, lsl #24
    cmp     \x, \t, asr #24
    eorne   \x, r8, \x, asr #31
.endm

    .global Audio_OutputDry
    .type Audio_OutputDry, %function
Audio_OutputDry:
    push    {r4-r8, lr}
    ldr     r12, [sp, #24]      @ vol
    ldmia   r12, {r4, r5}
    mov     r8, #127
1:
    ldr     r12, [r2], #4
    mov     r6, r12, lsl #16
    mov     r6, r6, asr #16     @ left
    sub     r12, r12, r6
    mov     r12, r12, asr #16   @ right
    mul     lr, r6, r4
    mul     r7, r12, r5
    mov     lr, lr, asr #13
    clamp8  lr, r6
    strb    lr, [r0], #1
    mov     r7, r7, asr #13
    clamp8  r7, r6
    strb    r7, [r1], #1
    subs    r3, r3, #1
    bne     1b
    pop     {r4-r8, lr}
    bx      lr
    .size Audio_OutputDry, . - Audio_OutputDry

    .global Audio_OutputEcho
    .type Audio_OutputEcho, %function
Audio_OutputEcho:
    push    {r4-r11, lr}
    ldr     r12, [sp, #36]      @ vol
    ldmia   r12, {r4-r7}
    mov     r8, #127
1:
    ldr     r10, [r2, #48 * 4]  @ the voices with echo
    ldr     r11, [r2, #96 * 4]  @ the echo
    ldr     r12, [r2], #4       @ the others
    add     r10, r10, r12
    mov     r12, r10, lsl #16
    mov     r12, r12, asr #16   @ left
    sub     r10, r10, r12
    mov     r10, r10, asr #16   @ right
    mul     lr, r12, r4
    mov     r12, r11, lsl #16
    mov     r12, r12, asr #16   @ the echo's left
    mla     lr, r12, r6, lr
    sub     r11, r11, r12
    mov     r11, r11, asr #16   @ the echo's right
    mul     r9, r10, r5
    mla     r9, r11, r7, r9
    mov     lr, lr, asr #13
    clamp8  lr, r12
    strb    lr, [r0], #1
    mov     r9, r9, asr #13
    clamp8  r9, r12
    strb    r9, [r1], #1
    subs    r3, r3, #1
    bne     1b
    pop     {r4-r11, lr}
    bx      lr
    .size Audio_OutputEcho, . - Audio_OutputEcho
