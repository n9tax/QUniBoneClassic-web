/* crt0.S - net11 start-up, interrupt and trap entry points.

   Loaded at 001000 and started there. Runs in kernel mode with memory
   management off, so the program sees addresses 000000-157777 as memory
   and 160000-177777 as the I/O page, on any QBUS or UNIBUS PDP-11 with
   EIS (11/23, 11/23+, 11/73 ...). No floating point.
*/
	.text
	.globl	_start, _main, _bss_start, _bss_end, _stack_top
	.globl	_clk_isr, _ticks
	.globl	_probe, _trap_stubs, _fault, _spl7, _spl0, _halt_cpu, _wait_intr

_start:
	mtps	$0340			/* no interrupts while setting up */
	mov	$_stack_top,sp
	mov	$_bss_start,r0
1:	cmp	r0,$_bss_end
	bhis	2f
	clr	(r0)+
	br	1b
2:	jsr	pc,_main
	halt
	br	_start

/* KW11-L line clock, 60 or 50 Hz: count ticks */
_clk_isr:
	inc	_ticks
	rti

/* int probe(volatile unsigned *addr): 1 if a word read of addr works,
   0 on a bus timeout (no device at that address) */
_probe:
	mov	@$4,-(sp)
	mov	@$6,-(sp)
	mov	$3f,@$4
	mov	$0340,@$6
	mov	6(sp),r0
	tst	(r0)
	mov	$1,r0
	br	4f
3:	tst	(sp)+			/* drop the trap frame: PC */
	mtps	(sp)+			/* and PSW, as before the trap */
	clr	r0
4:	mov	(sp)+,@$6
	mov	(sp)+,@$4
	rts	pc

/* processor priority */
_spl7:	mtps	$0340
	rts	pc
_spl0:	mtps	$0
	rts	pc
_halt_cpu:
	halt
	rts	pc
_wait_intr:
	wait
	rts	pc

/* Catch traps and unexpected interrupts. Each stub pushes its vector,
   main() points the vectors at them. fault(vector, pc, psw) reports. */
_trap_stubs:
	mov	$04,-(sp)
	br	9f
	mov	$010,-(sp)
	br	9f
	mov	$014,-(sp)
	br	9f
	mov	$020,-(sp)
	br	9f
	mov	$024,-(sp)
	br	9f
	mov	$030,-(sp)
	br	9f
	mov	$034,-(sp)
	br	9f
	mov	$0244,-(sp)		/* floating point exception */
	br	9f
	mov	$0250,-(sp)		/* memory management */
	br	9f
	mov	$0,-(sp)		/* any other vector */
	br	9f
9:	jsr	pc,_fault
	tst	(sp)+
	rti
