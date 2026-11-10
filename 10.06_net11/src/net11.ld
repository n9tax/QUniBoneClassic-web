/* net11.ld - program at 001000, stack at the top of the low 56 KB */
OUTPUT_FORMAT("a.out-pdp11")
ENTRY(_start)
SECTIONS
{
	. = 01000;
	.text : { crt0.o(.text) *(.text) }
	.data : { *(.data) }
	.bss : {
		_bss_start = .;
		*(.bss)
		*(COMMON)
		. = ALIGN(2);
		_bss_end = .;
	}
	_prog_end = .;
	_stack_top = 0160000;
}
