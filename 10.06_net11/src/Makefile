# net11 - a small network system for the PDP-11 with a DEQNA/DELQA
#
# Needs a pdp11-aout cross toolchain (binutils + gcc, C only), see ../README.md.
# Output: net11.lda (absolute loader format: "m lp net11.lda" in the QBone demo
# program, then start at 1000), and net11.out (a.out with symbols).

CROSS ?= pdp11-aout-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJDUMP = $(CROSS)objdump
CFLAGS = -m40 -msoft-float -Os -Wall -Wno-pointer-sign -fno-builtin -ffreestanding -nostdinc \
	-fno-delete-null-pointer-checks --param=min-pagesize=0 \
	-isystem $(shell $(CC) -print-file-name=include)
LIBGCC = $(shell $(CC) -m40 -msoft-float -print-libgcc-file-name)

OBJS = crt0.o main.o lib.o lib32.o console.o clock.o qe.o net.o tcp.o dhcp.o dns.o sntp.o \
	http.o telnet.o monitor.o

all: net11.lda

net11.out: $(OBJS) net11.ld
	$(LD) -N -T net11.ld -o $@ $(OBJS) $(LIBGCC) -Map net11.map

net11.lda: net11.out ../tools/aout2lda.py
	python3 ../tools/aout2lda.py net11.out $@ 1000
	@# the 11/23 has no floating point unit: there must be no FPU instructions
	@if $(OBJDUMP) -d net11.out | grep -E '\s(setd|setf|seti|setl|ldf|stf|ldd|std|cfcc|ldfps|stfps)\s'; then \
		echo "FPU instructions in net11.out"; exit 1; fi

%.o: %.c hw.h net.h
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

dis: net11.out
	$(OBJDUMP) -d net11.out > net11.dis

clean:
	rm -f *.o net11.out net11.lda net11.map net11.dis
