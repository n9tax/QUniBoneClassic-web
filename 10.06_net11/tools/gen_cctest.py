#!/usr/bin/env python3
# gen_cctest.py - generate a C test of integer code generation for the PDP-11
# compiler: shifts, compares, multiply, divide, range tests. Expected values
# are computed here; the PDP-11 program prints every mismatch.
#   gen_cctest.py TYPE [PART] > cctest.c     TYPE: u8 s8 u16 s16 u32 s32 range
#   PART: shift, cmp, arith (default all). Compile with -fwrapv: the tests
#   overflow signed numbers on purpose.
import random, sys

random.seed(11)
lines = []
def emit(x):
    lines.append(x)

types = {
    "u8": (8, False), "s8": (8, True), "u16": (16, False), "s16": (16, True),
    "u32": (32, False), "s32": (32, True),
}
ctype = {"u8": "unsigned char", "s8": "signed char", "u16": "unsigned short",
         "s16": "short", "u32": "unsigned long", "s32": "long"}


def wrap(v, bits, signed):
    v &= (1 << bits) - 1
    if signed and v >= 1 << (bits - 1):
        v -= 1 << bits
    return v


def lit(v, t):
    bits, signed = types[t]
    if bits == 32:
        return "(%s)%dL" % (ctype[t], v) if signed and v != -2**31 else \
            ("(%s)%dUL" % (ctype[t], v & 0xffffffff))
    return "(%s)%d" % (ctype[t], v)


def interesting(bits, signed):
    vals = [0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x100, 0x1234, 0x7fff, 0x8000, 0xffff,
            0x10000, 0x12345678, 0x7fffffff, 0x80000000, 0xffffffff, 0x8001, 0x18000]
    vals += [random.getrandbits(bits) for _ in range(2)]
    vals = sorted(set(wrap(v, bits, signed) for v in vals))
    return vals if len(vals) <= 11 else sorted(random.sample(vals, 11))


n_checks = 0
emit('#include "hw.h"')
emit("static int fails, checks;")
emit("static void bad(int line, long got, long want) { if (fails++ < 40) cprint(\"FAIL line %d: got %ld want %ld\\n\", line, got, want); }")
emit("#define CHECK(expr, want) do { checks++; if ((long)(expr) != (long)(want)) bad(__LINE__, (long)(expr), (long)(want)); } while (0)")
emit("static volatile int vi;")
for t in types:
    emit("static volatile %s v_%s;" % (ctype[t], t))
which = sys.argv[1] if len(sys.argv) > 1 else "u16"
part = sys.argv[2] if len(sys.argv) > 2 else "all"     # shift, cmp, arith or all
nfunc = 0
body = []

def chunk():
    # split into functions: a PDP-11 function cannot be very big
    global nfunc
    if len(body) > 120:
        flush()

def flush():
    global nfunc, body
    if body:
        lines.append("static void t%d(void) {" % nfunc)
        lines.extend(body)
        lines.append("}")
        nfunc += 1
        body = []

def emit(x):
    body.append(x)

for t, (bits, signed) in types.items():
    if t != which:
        continue
    vals = interesting(bits, signed)
    promoted_bits = 16 if bits < 16 else bits     # C promotion to int (16 bit)
    for v in (vals if part in ("all", "shift") else []):
        for n in sorted(set([0, 1, 3, 4, 7, bits - 1] + ([8, 15, 16, 17, 31] if bits == 32 else [8, 12, 15] if bits == 16 else []))):
            if n >= promoted_bits:
                continue
            # right shift, value promoted to int (or long)
            if bits < 16:
                pv = v                          # promoted value keeps sign
                want = pv >> n
                pbits = 16
            else:
                want = v >> n if signed else (v & ((1 << bits) - 1)) >> n
                pbits = bits
            want_l = wrap(want, pbits, signed or bits < 16)
            emit("v_%s = %s; vi = %d;" % (t, lit(v, t), n))
            emit("CHECK((%s)(v_%s >> %d), %d);" % (ctype[t], t, n, wrap(want_l, bits, signed)))
            emit("CHECK((%s)(v_%s >> vi), %d);" % (ctype[t], t, wrap(want_l, bits, signed)))
            wl = wrap(v << n, bits, signed)
            emit("CHECK((%s)(v_%s << %d), %d);" % (ctype[t], t, n, wl))
            emit("CHECK((%s)(v_%s << vi), %d);" % (ctype[t], t, wl))
            emit("CHECK((v_%s >> vi) & 1, %d);" % (t, (want_l & 1)))
            n_checks += 5
            chunk()
    # compares against other values
    for a in (vals if part in ("all", "cmp") else []):
        for b in random.sample(vals, min(3, len(vals))) + [a]:
            emit("v_%s = %s;" % (t, lit(a, t)))
            for op, f in (("<", a < b), ("<=", a <= b), (">", a > b), (">=", a >= b), ("==", a == b)):
                emit("CHECK(v_%s %s %s, %d);" % (t, op, lit(b, t), 1 if f else 0))
                n_checks += 1
            chunk()
    # multiply / divide
    for a in (random.sample(vals, min(8, len(vals))) if part in ("all", "arith") else []):
        for b in random.sample(vals, min(5, len(vals))):
            emit("v_%s = %s;" % (t, lit(a, t)))
            emit("CHECK((%s)(v_%s * %s), %d);" % (ctype[t], t, lit(b, t), wrap(a * b, bits, signed)))
            n_checks += 1
            if b != 0:
                q = int(a / b)                  # C truncates toward zero
                r = a - q * b
                if not (signed and a == -(1 << (bits - 1)) and b == -1):
                    emit("CHECK((%s)(v_%s / %s), %d);" % (ctype[t], t, lit(b, t), wrap(q, bits, signed)))
                    emit("CHECK((%s)(v_%s %% %s), %d);" % (ctype[t], t, lit(b, t), wrap(r, bits, signed)))
                    n_checks += 2
            chunk()

# range tests (become bit tests in gcc)
for sets in ([3, 5, 8], [0, 2, 9, 15], [1, 4], [6, 7, 10, 12, 13]) if which == "range" else ():
    chunk()
    for x in range(0, 17):
        emit("v_u8 = %d;" % x)
        cond = " || ".join("v_u8 == %d" % s for s in sets)
        emit("CHECK((%s) ? 1 : 0, %d);" % (cond, 1 if x in sets else 0))
        n_checks += 1

flush()
def emit(x):
    lines.append(x)
emit("int main(void) { con_init();")
for i in range(nfunc):
    emit("t%d();" % i)
emit("cprint(\"cctest %s: %%d checks, %%d failed\\n\", checks, fails); return 0; }" % which)
emit("void fault(u16 v, u16 pc, u16 psw) { cprint(\"TRAP %o at %o\\n\", v, pc); halt_cpu(); }")
print("\n".join(lines))
