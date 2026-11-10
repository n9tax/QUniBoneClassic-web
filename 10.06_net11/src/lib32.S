/* lib32.S - 32 bit division for net11, replacing libgcc's.

   gcc 13 for the PDP-11 compiles 32 bit *signed* comparisons wrongly when the
   high words are equal (it compares the low words with a signed branch), and
   libgcc's __udivmodsi4 contains such a comparison: quotients above 0x3fff
   come out wrong. These routines do it in assembler, shift and subtract.

   Call: 2(sp) a high, 4(sp) a low, 6(sp) b high, 10(sp) b low.
   Return: r0 high, r1 low.
*/
	.text
	.globl	___udivsi3, ___umodsi3, ___divsi3, ___modsi3, _udivmod32

/* udm: r2:r3 dividend, r4:r5 divisor -> r2:r3 quotient, r0:r1 remainder */
udm:
	clr	r0
	clr	r1
	mov	$040,-(sp)
1:	asl	r3
	rol	r2
	rol	r1
	rol	r0
	bcs	4f			/* remainder overflowed 32 bits: >= divisor */
	cmp	r0,r4
	bne	2f
	cmp	r1,r5
2:	blo	3f
4:	sub	r5,r1
	sbc	r0
	sub	r4,r0
	inc	r3
3:	dec	(sp)
	bne	1b
	tst	(sp)+
	rts	pc

save:	/* jsr r5,save: push r4 r3 r2, r5 holds the return; caller pushed r5 */
	mov	r4,-(sp)
	mov	r3,-(sp)
	mov	r2,-(sp)
	jmp	(r5)

___udivsi3:
	mov	r5,-(sp)
	jsr	r5,save
	mov	014(sp),r2
	mov	016(sp),r3
	mov	020(sp),r4
	mov	022(sp),r5
	jsr	pc,udm
	mov	r2,r0
	mov	r3,r1
	br	out

___umodsi3:
	mov	r5,-(sp)
	jsr	r5,save
	mov	014(sp),r2
	mov	016(sp),r3
	mov	020(sp),r4
	mov	022(sp),r5
	jsr	pc,udm
	br	out

/* stack after save: 0 r2, 2 r3, 4 r4, 6 r5 (from jsr r5), 10 r5 (saved), 12 ret, 14 a ... */
out:
	mov	(sp)+,r2
	mov	(sp)+,r3
	mov	(sp)+,r4
	tst	(sp)+			/* return link of jsr r5,save */
	mov	(sp)+,r5
	rts	pc

/* signed: flags word on top, bit 0: a < 0, bit 1: b < 0 */
sdm:
	clr	-(sp)
	tst	r2
	bge	1f
	neg	r2
	neg	r3
	sbc	r2
	inc	(sp)
1:	tst	r4
	bge	2f
	neg	r4
	neg	r5
	sbc	r4
	add	$2,(sp)
2:	jsr	pc,udm
	mov	(sp)+,r4		/* flags */
	rts	pc

___divsi3:
	mov	r5,-(sp)
	jsr	r5,save
	mov	014(sp),r2
	mov	016(sp),r3
	mov	020(sp),r4
	mov	022(sp),r5
	jsr	pc,sdm
	cmp	r4,$1
	beq	1f
	cmp	r4,$2
	bne	2f
1:	neg	r2
	neg	r3
	sbc	r2
2:	mov	r2,r0
	mov	r3,r1
	br	out

___modsi3:
	mov	r5,-(sp)
	jsr	r5,save
	mov	014(sp),r2
	mov	016(sp),r3
	mov	020(sp),r4
	mov	022(sp),r5
	jsr	pc,sdm
	bit	$1,r4
	beq	1f
	neg	r0
	neg	r1
	sbc	r0
1:	br	out

/* u32 udivmod32(u32 a, u32 b, u32 *rem): quotient, and the remainder in *rem */
_udivmod32:
	mov	r5,-(sp)
	jsr	r5,save
	mov	014(sp),r2
	mov	016(sp),r3
	mov	020(sp),r4
	mov	022(sp),r5
	jsr	pc,udm
	mov	024(sp),r4
	mov	r0,(r4)+
	mov	r1,(r4)
	mov	r2,r0
	mov	r3,r1
	br	out
