@ The mixer's inner loops (see audio_mix.iwram.c), by hand: in C, clang
@ runs out of registers for them, and spills.
@
@     void Audio_MixVoice(int32_t *sum, int n, AudioVoice *v);
@
@ Adds |n| (even) of a voice's samples, times its volume, to |sum|, as the
@ loop in Audio_MixChunk does, four at a time; or, played slower than half
@ the mixer's rate, those between its samples, a sample at a time. At the
@ end of the sample, it goes back to its loop, or is done (v->on false).
@ Silent (v->vol 0), it only moves on.
@
@     void Audio_ClearSums(int32_t *sums, int n, int32_t value);
@
@ Sets |n| (even) words to |value|, four at a time (memset's loop is two or
@ three times slower).
@
@     void Audio_EchoPairs(int32_t *wet, int pairs, uint32_t *buf, EchoState *s);
@
@ For each pair of samples: the echo buffer's word (both sides, 16 bits
@ each) and the one before it go through the filter (s->fir0 on the new,
@ s->fir1 on the old, /128, clamped); that's the pair's second sample of
@ echo, and the first is halfway from the last one. Both go to wet[48] on
@ (kAudioChunk on). Then the pair's average of |wet| (the voices' echo), and
@ the filtered echo at s->feedback (/128), clamped, go back in the buffer.
@ The echo is as the voices' sums: right << 16 + left. |wet| has 0x80008000
@ more in each word, and the buffer's words (s->old too) are that, as 16
@ bits each side: 0 to 0xffff, 0x8000 more than the sample. Then a side is
@ one `and` or shift, with no sign to take care of, and the average of two
@ sides is a sum and a shift. (Audio_MixChunk starts the other voices' sums
@ at 0x80008000 less, so that the sums add up as before.) The game's
@ filter, 127 on the old word alone, is x - ((x + 127) >> 7) on a side: the
@ loop does that, and any other in the block at 4.
@
@     void Audio_EchoUnheard(int32_t *wet, int pairs, uint32_t *buf, EchoState *s);
@
@ Audio_EchoPairs, when the feedback is 0 and no echo is heard, for 2 pairs
@ or more: the pairs before the last put in the buffer the average of |wet|
@ (no filter, no clamp: the average of two 16-bit sides is one), as the
@ filter's output, not heard, has no part in it. The last runs as above,
@ with the word before it as its old, for the state it leaves.
@
@     void Audio_OutputEcho(int8_t *left, int8_t *right, const int32_t *sums, int n,
@                           const int32_t vol[4]);
@
@ The mixer's last step, as Audio_MixChunk has it, with the echo: |n|
@ samples of the voices' sums (both sides in a word, as above), at the
@ master volumes (vol[0] left, vol[1] right), /8192, clamped, into |left|
@ and |right|. The voices' with echo are at sums[48] on (kAudioChunk on),
@ added in, and the echo's output at sums[96] on, at the echo volumes
@ (vol[2], vol[3]). With those 0, the echo's output isn't read.

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
    .equ S_OLD, 12              @ the buffer's last word (see Audio_EchoPairs)
    .equ S_LAST, 16             @ the last echo sample

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
    @ number, as the sample is, than by one as big as the volume.) Four
    @ samples at a time, for the sums' loads and stores, and the loop: two
    @ first, if |n| is two more than a multiple of four.
    tst     r1, #2
    beq     0f
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
    beq     6f
0:
    add     r8, r3, r4, lsr #16
    ldrsb   r8, [r8]
    add     r4, r4, r5
    add     r9, r3, r4, lsr #16
    ldrsb   r9, [r9]
    add     r4, r4, r5
    ldmia   r0, {r6, r10, r11, r12}
    mla     r6, r7, r8, r6
    mla     r10, r7, r9, r10
    add     r8, r3, r4, lsr #16
    ldrsb   r8, [r8]
    add     r4, r4, r5
    add     r9, r3, r4, lsr #16
    ldrsb   r9, [r9]
    add     r4, r4, r5
    mla     r11, r7, r8, r11
    mla     r12, r7, r9, r12
    stmia   r0!, {r6, r10, r11, r12}
    subs    r1, r1, #4
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
    mla     r8, r5, r1, r4
    cmp     r8, r6
    blo     12f
    mvn     r10, #1             @ (not a sample's, nor the one before)
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

    @ Not to the end in these |n|: two at a time. The position is in two
    @ parts, so that its sample changing comes as a carry: its fraction in the
    @ top of r4 (r5 the step the same), and the index of the sample after
    @ (r10), from the end's: r3, moved up to it, takes the index below 0,
    @ and the end is a sign. r8 is the sample, r9 how far the next is from it.
12:
    ldr     r6, [r2, #V_END]
    mov     r6, r6, lsr #16
    add     r3, r3, r6
    mov     r10, r4, lsr #16
    sub     r10, r10, r6
    ldrsb   r8, [r3, r10]
    mov     r9, #0
    mov     r4, r4, lsl #16
    mov     r5, r5, lsl #16
    bl      19f
22:
    mov     r11, r4, lsr #24
    mul     lr, r9, r11
    add     r11, r8, lr, asr #8
    adds    r4, r4, r5
    blcs    19f
    mov     r12, r4, lsr #24
    mul     lr, r9, r12
    add     r12, r8, lr, asr #8
    adds    r4, r4, r5
    blcs    19f
    ldmia   r0, {r6, lr}
    mla     r6, r7, r11, r6
    mla     lr, r7, r12, lr
    stmia   r0!, {r6, lr}
    subs    r1, r1, #2
    bne     22b
    ldr     r12, [r2, #V_END]
    mov     r11, r4, lsr #16
    add     r12, r10, r12, lsr #16
    sub     r12, r12, #1
    orr     r4, r11, r12, lsl #16
    b       6b
    @ On to the next sample (r8 ← r8 + r9), and how far the one after is
    @ from it. At the end (r10 is 0, and what's read is past it): its loop's
    @ first, or the last again, as 15 has it.
19:
    add     r8, r8, r9
    adds    r10, r10, #1
    ldrsb   r9, [r3, r10]
    sub     r9, r9, r8
    bxmi    lr
    ldr     r6, [r2, #V_LOOP_LEN]
    movs    r6, r6, lsr #16
    moveq   r6, #1
    sub     r6, r10, r6
    ldrsb   r9, [r3, r6]
    sub     r9, r9, r8
    bx      lr

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

    .global Audio_ClearSums
    .type Audio_ClearSums, %function
Audio_ClearSums:
    str     r4, [sp, #-4]!
    mov     r3, r2
    mov     r4, r2
    mov     r12, r2
    tst     r1, #2
    stmne   r0!, {r2, r3}
    movs    r1, r1, lsr #2
    beq     2f
1:
    stmia   r0!, {r2, r3, r4, r12}
    subs    r1, r1, #1
    bne     1b
2:
    ldr     r4, [sp], #4
    bx      lr
    .size Audio_ClearSums, . - Audio_ClearSums

    .global Audio_EchoPairs
    .type Audio_EchoPairs, %function
Audio_EchoPairs:
    push    {r4-r11, lr}
    push    {r3}
    ldr     r4, [r3, #S_FEEDBACK]
    ldr     r6, [r3, #S_OLD]
    ldr     r8, [r3, #S_LAST]
    ldr     r9, [r3, #S_FIR0]
    ldr     r12, [r3, #S_FIR1]
    sub     r12, r12, #127
    orr     r9, r9, r12         @ 0 if (0, 127), as the game has it
    mov     r7, r8, lsl #16
    mov     r7, r7, asr #16     @ the last echo's left
    sub     r8, r8, r7
    mov     r8, r8, asr #16     @ and right
    mov     r5, #0xff
    orr     r5, r5, #0xff00
1:
    @ The filter, on the old word's sides, 0 to 0xffff (0x8000 more than
    @ they are). With (0, 127), x - ((x + 127) >> 7) is x * 127 >> 7. r10
    @ left, r11 right.
    and     r10, r6, r5
    mov     r11, r6, lsr #16
    cmp     r9, #0
    bne     4f
    add     r12, r10, #127
    sub     r10, r10, #0x7f00   @ (0x8000 less, and 0x100 back from the shift)
    sub     r10, r10, r12, asr #7
    add     r12, r11, #127
    sub     r11, r11, #0x7f00
    sub     r11, r11, r12, asr #7
5:
    @ The pair's echo: halfway from the last, then the new.
    add     lr, r10, r11, lsl #16
    add     r12, r7, r10
    mov     r12, r12, asr #1
    add     r6, r8, r11
    mov     r6, r6, asr #1
    add     r12, r12, r6, lsl #16
    str     r12, [r0, #OUT]
    str     lr, [r0, #OUT + 4]
    mov     r7, r10
    mov     r8, r11
    @ The pair's average of the voices' echo, each side 0 to 0xffff, and
    @ the feedback.
    mul     r6, r10, r4
    mul     r10, r11, r4
    ldmia   r0, {r11, r12}
    and     lr, r11, r5
    and     r3, r12, r5
    add     lr, lr, r3
    mov     lr, lr, lsr #1
    add     lr, lr, r6, asr #7
    mov     r6, r11, lsr #16
    add     r6, r6, r12, lsr #16
    mov     r6, r6, lsr #1
    add     r6, r6, r10, asr #7
    orr     r3, lr, r6
    movs    r3, r3, lsr #16
    bne     3f
2:
    orr     r3, lr, r6, lsl #16
    swp     r6, r3, [r2]        @ into the buffer, the word there is the next old
    add     r2, r2, #4
    add     r0, r0, #8
    subs    r1, r1, #1
    bne     1b
    pop     {r3}
    str     r6, [r3, #S_OLD]
    add     r7, r7, r8, lsl #16
    str     r7, [r3, #S_LAST]
    pop     {r4-r11, lr}
    bx      lr
    @ Clamped to 16 bits (0 to 0xffff here).
3:
    cmp     lr, #0x10000
    mvnhs   lr, lr, asr #31
    movhs   lr, lr, lsr #16
    cmp     r6, #0x10000
    mvnhs   r6, r6, asr #31
    movhs   r6, r6, lsr #16
    b       2b
    @ The filter with two taps, the new word's and the old's: left in r10,
    @ right in r11, their sides now what they are, and clamped.
4:
    ldr     r3, [sp]
    ldr     r12, [r2]
    ldr     lr, [r3, #S_FIR0]
    ldr     r3, [r3, #S_FIR1]
    and     r6, r12, r5
    mov     r12, r12, lsr #16
    sub     r6, r6, #0x8000
    sub     r12, r12, #0x8000
    sub     r10, r10, #0x8000
    sub     r11, r11, #0x8000
    mul     r6, lr, r6
    mla     r6, r3, r10, r6
    mul     r12, lr, r12
    mla     r12, r3, r11, r12
    mov     r10, r6, asr #7
    mov     r11, r12, asr #7
    mov     r6, r10, lsl #16
    cmp     r10, r6, asr #16
    movgt   r10, #0x7f00
    orrgt   r10, r10, #0xff
    mvnlt   r10, #0x7f00
    biclt   r10, r10, #0xff
    mov     r6, r11, lsl #16
    cmp     r11, r6, asr #16
    movgt   r11, #0x7f00
    orrgt   r11, r11, #0xff
    mvnlt   r11, #0x7f00
    biclt   r11, r11, #0xff
    b       5b
    .size Audio_EchoPairs, . - Audio_EchoPairs

    .global Audio_EchoUnheard
    .type Audio_EchoUnheard, %function
Audio_EchoUnheard:
    @ The last pair's old word, before the pairs before it write over it.
    add     r12, r2, r1, lsl #2
    ldr     r12, [r12, #-8]
    str     r12, [r3, #S_OLD]
    sub     r1, r1, #1
    push    {r4-r8, lr}
    mov     r4, #0xff
    orr     r4, r4, #0xff00
1:
    ldmia   r0!, {r6, r7}
    and     r8, r6, r4
    and     r12, r7, r4
    add     r8, r8, r12
    mov     r8, r8, lsr #1
    mov     r12, r6, lsr #16
    add     r12, r12, r7, lsr #16
    mov     r12, r12, lsr #1
    orr     r8, r8, r12, lsl #16
    str     r8, [r2], #4
    subs    r1, r1, #1
    bne     1b
    pop     {r4-r8, lr}
    mov     r1, #1
    b       Audio_EchoPairs
    .size Audio_EchoUnheard, . - Audio_EchoUnheard

@ Clamps \x to 8 bits, with \t a temporary and \k 127.
.macro clamp8 x, t, k=r8
    mov     \t, \x, lsl #24
    cmp     \x, \t, asr #24
    eorne   \x, \k, \x, asr #31
.endm

    .global Audio_OutputEcho
    .type Audio_OutputEcho, %function
Audio_OutputEcho:
    push    {r4-r11, lr}
    ldr     r12, [sp, #36]      @ vol
    ldmia   r12, {r4-r7}
    orrs    r9, r6, r7
    beq     2f
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
    @ No echo to hear (its volumes 0): half the work.
2:
    mov     r6, #127
3:
    ldr     r7, [r2, #48 * 4]
    ldr     r8, [r2], #4
    add     r7, r7, r8
    mov     r8, r7, lsl #16
    mov     r8, r8, asr #16     @ left
    sub     r7, r7, r8
    mov     r7, r7, asr #16     @ right
    mul     r9, r8, r4
    mul     r10, r7, r5
    mov     r9, r9, asr #13
    clamp8  r9, r8, r6
    strb    r9, [r0], #1
    mov     r10, r10, asr #13
    clamp8  r10, r8, r6
    strb    r10, [r1], #1
    subs    r3, r3, #1
    bne     3b
    pop     {r4-r11, lr}
    bx      lr
    .size Audio_OutputEcho, . - Audio_OutputEcho
