#!/usr/bin/env python3
"""PDP-11/23+ (KDF11-B) upgrade notes for the BA11-N: the 22-bit bus mod of
the H9273 backplane and the console cable for the 10-pin serial headers.

    python3 make_1123plus.py [out.pdf]

Needs reportlab.  Sources are listed on the last page.
"""
import os
import sys

from reportlab import rl_config
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import letter
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import inch
from reportlab.graphics.shapes import (Circle, Drawing, Line, PolyLine, Rect,
                                       String, Group)
from reportlab.platypus import (Image, KeepTogether, PageBreak, Paragraph,
                                SimpleDocTemplate, Spacer, Table, TableStyle)

OUT = sys.argv[1] if len(sys.argv) > 1 else "11-23plus-upgrade.pdf"
# CUPS (pdftopdf) cannot decode ASCII85+Flate streams and prints blank pages
rl_config.useA85 = 0

# --- Colours: one per new address line, used in every figure ------------
C18 = colors.HexColor("#d62728")
C19 = colors.HexColor("#ff7f0e")
C20 = colors.HexColor("#2ca02c")
C21 = colors.HexColor("#1f77b4")
LINES = [("C", "BDAL18", C18), ("D", "BDAL19", C19),
         ("E", "BDAL20", C20), ("F", "BDAL21", C21)]
GREY = colors.HexColor("#888888")
LIGHT = colors.HexColor("#eeeeee")
WARN = colors.HexColor("#b00000")
INK = colors.HexColor("#222222")

ss = getSampleStyleSheet()
BODY = ParagraphStyle("body", parent=ss["BodyText"], fontName="Helvetica",
                      fontSize=10, leading=13.5, spaceAfter=5)
SMALL = ParagraphStyle("small", parent=BODY, fontSize=8.5, leading=11)
H1 = ParagraphStyle("h1", parent=ss["Title"], fontName="Helvetica-Bold",
                    fontSize=19, leading=23, spaceAfter=4)
SUB = ParagraphStyle("sub", parent=BODY, alignment=TA_CENTER, fontSize=10.5,
                     textColor=GREY, spaceAfter=10)
H2 = ParagraphStyle("h2", parent=ss["Heading2"], fontName="Helvetica-Bold",
                    fontSize=13.5, leading=17, spaceBefore=10, spaceAfter=5)
H3 = ParagraphStyle("h3", parent=ss["Heading3"], fontName="Helvetica-Bold",
                    fontSize=11, leading=14, spaceBefore=6, spaceAfter=3)
CAP = ParagraphStyle("cap", parent=BODY, fontSize=8.5, leading=11,
                     alignment=TA_CENTER, textColor=GREY, spaceAfter=8)
CELL = ParagraphStyle("cell", parent=BODY, fontSize=9, leading=11.5,
                      spaceAfter=0)
CELLB = ParagraphStyle("cellb", parent=CELL, fontName="Helvetica-Bold")


def p(t, st=BODY):
    return Paragraph(t, st)


def bullets(items, st=BODY):
    return [Paragraph(t, st, bulletText="•") for t in items]


def steps(items, st=BODY):
    return [Paragraph(t, st, bulletText="%d." % (i + 1))
            for i, t in enumerate(items)]


def table(rows, widths, head=True, zebra=True, extra=()):
    data = [[c if not isinstance(c, str) else
             Paragraph(c, CELLB if (head and r == 0) else CELL)
             for c in row] for r, row in enumerate(rows)]
    t = Table(data, colWidths=widths, repeatRows=1 if head else 0)
    style = [("GRID", (0, 0), (-1, -1), 0.4, GREY),
             ("VALIGN", (0, 0), (-1, -1), "TOP"),
             ("TOPPADDING", (0, 0), (-1, -1), 3),
             ("BOTTOMPADDING", (0, 0), (-1, -1), 3)]
    if head:
        style.append(("BACKGROUND", (0, 0), (-1, 0), LIGHT))
    if zebra:
        for r in range(2 if head else 1, len(rows), 2):
            style.append(("BACKGROUND", (0, r), (-1, r),
                          colors.HexColor("#f8f8f8")))
    style.extend(extra)
    t.setStyle(TableStyle(style))
    return t


def s(d, x, y, text, size=8, color=INK, anchor="middle", bold=False):
    d.add(String(x, y, text, fontName="Helvetica-Bold" if bold else
                 "Helvetica", fontSize=size, fillColor=color,
                 textAnchor=anchor))


# Pin letters of one slot, A..V without G I O Q
PINS = "ABCDEFHJKLMNPRSTUV"


# --- Figure 1: the H9273 seen from the back --------------------------------
def fig_backplane():
    W, H = 500, 336
    d = Drawing(W, H)
    left = 62                      # room for the row labels
    blockw = 92                    # one slot: 18 pins
    gap_in = 6                     # between the two slots of a connector
    gap_mid = 30                   # between connector 2 and connector 1
    rowh = 30
    top = H - 46
    pitch = (blockw - 8) / 17.0
    # rear view, left to right: D C | B A ; inside a slot V..A
    slots = [("D", left), ("C", left + blockw + gap_in)]
    x1 = left + 2 * blockw + gap_in + gap_mid
    slots += [("B", x1), ("A", x1 + blockw + gap_in)]

    def pinx(slot_x, letter):          # rear view: pin A at the right
        i = PINS.index(letter)
        return slot_x + blockw - 4 - i * pitch

    s(d, (left + slots[1][1] + blockw) / 2, H - 14,
      "Connector 2: CD bus (leave alone)", 8.5, GREY)
    s(d, (x1 + slots[3][1] + blockw) / 2, H - 14,
      "Connector 1: Q-bus (slots A, B)", 8.5, INK, bold=True)
    for name, sx in slots:
        s(d, sx + blockw / 2, H - 30, "slot " + name, 8,
          INK if name in "AB" else GREY, bold=name == "B")

    for r in range(9):
        ytop = top - r * rowh
        s(d, left - 8, ytop - 13, "Row %d" % (r + 1), 8.5, INK, "end")
        if r == 0:
            s(d, left - 8, ytop - 23, "(CPU)", 7, GREY, "end")
        for name, sx in slots:
            fill = colors.HexColor("#fff7e0") if name == "B" else \
                colors.white if name == "A" else colors.HexColor("#f3f3f3")
            d.add(Rect(sx, ytop - 24, blockw, 20, fillColor=fill,
                       strokeColor=GREY, strokeWidth=0.6))
            for k, letter in enumerate(PINS):
                x = pinx(sx, letter)
                for side, dy in ((1, -9), (2, -19)):
                    d.add(Circle(x, ytop + dy, 1.1, fillColor=GREY,
                                 strokeColor=None))
    # the four new wires: straight down, row 1 to row 9, one pin per row
    bx = slots[2][1]
    for letter, sig, col in LINES:
        x = pinx(bx, letter)
        y0 = top - 9
        y9 = top - 8 * rowh - 9
        d.add(Line(x, y0, x, y9, strokeColor=col, strokeWidth=1.6))
        for r in range(9):
            d.add(Circle(x, top - r * rowh - 9, 2.2, fillColor=col,
                         strokeColor=colors.white, strokeWidth=0.4))
    # legend
    ly = 14
    lx = left
    s(d, lx, ly, "New wires on slot B, side 1:", 8, INK, "start", True)
    lx += 118
    for letter, sig, col in LINES:
        d.add(Line(lx, ly + 3, lx + 14, ly + 3, strokeColor=col,
                   strokeWidth=2))
        s(d, lx + 17, ly, "B%s1 = %s" % (letter, sig), 8, INK, "start")
        lx += 82
    return d


# --- Figure 2: slot B close up (rear view) ----------------------------------
SIDE1 = {"A": ("BDCOK", None), "B": ("BPOK", None),
         "C": ("BDAL18", C18), "D": ("BDAL19", C19), "E": ("BDAL20", C20),
         "F": ("BDAL21", C21), "H": ("spare", None), "J": ("GND", None)}
SIDE2 = {"A": ("+5 V", None), "B": ("−12 V", None), "C": ("GND", None),
         "D": ("+12 V", WARN), "E": ("BDAL02", None), "F": ("BDAL03", None)}


def fig_slotb():
    W, H = 500, 172
    d = Drawing(W, H)
    x0, x1 = 30, 470
    pitch = (x1 - x0) / 17.0
    y1, y2 = 106, 66
    d.add(Rect(x0 - 14, y2 - 14, x1 - x0 + 28, y1 - y2 + 28,
               fillColor=colors.HexColor("#fff7e0"), strokeColor=GREY,
               strokeWidth=0.8))

    def px(letter):                  # rear view: A at the right
        return x1 - PINS.index(letter) * pitch

    for letter in PINS:
        x = px(letter)
        for y, side, names in ((y1, 1, SIDE1), (y2, 2, SIDE2)):
            name, col = names.get(letter, (None, None))
            target = side == 1 and letter in "CDEF"
            if target:
                d.add(Circle(x, y, 6.5, fillColor=col, strokeColor=INK,
                             strokeWidth=0.6))
            elif col is WARN:
                d.add(Circle(x, y, 5, fillColor=colors.white,
                             strokeColor=WARN, strokeWidth=1.6))
            else:
                d.add(Circle(x, y, 3.2, fillColor=GREY, strokeColor=None))
        s(d, x, y1 + 13, letter, 8.5, INK, bold=letter in "CDEF")
    # signal names
    # names alternate between two heights so neighbours do not collide
    for letter, (name, col) in SIDE1.items():
        up = 36 if PINS.index(letter) % 2 else 25
        s(d, px(letter), y1 + up, name, 7, col or GREY,
          bold=col is not None)
    for letter, (name, col) in SIDE2.items():
        dn = 32 if PINS.index(letter) % 2 else 21
        s(d, px(letter), y2 - dn, name, 7,
          WARN if col is WARN else GREY, bold=col is WARN)
    s(d, x0 - 20, y1 - 3, "side 1", 7.5, INK, "end")
    s(d, x0 - 20, y2 - 3, "side 2", 7.5, INK, "end")
    s(d, (x0 + x1) / 2, 4,
      "Rear (wiring side) view of slot B in one row.  Pin V is at the "
      "left, pin A at the right (mirror image of the card side).", 8, GREY)
    s(d, px("V"), y1 + 25, "← toward slot C", 7, GREY)
    s(d, x1 + 10, y2 - 46, "toward slot A →", 7, GREY, "end")
    return d


# --- Figure 3: DEC Figure 2-1, scanned (kdf11ba-fig2-1.png) --------------
SWBLUE = colors.HexColor("#2b4f9e")
FIG21 = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "kdf11ba-fig2-1.png")


def fig_dec_board():
    w = 7.0 * inch
    return Image(FIG21, width=w, height=w * 1235 / 1680)


# --- Figure 5: the jumper stakes that matter, close up ----------------------
def fig_jumpers():
    W, H = 470, 250
    d = Drawing(W, H)
    JUMP = colors.HexColor("#1f77b4")

    def stake(x, y, name, side="left"):
        d.add(Circle(x, y, 2.6, fillColor=colors.white, strokeColor=INK,
                     strokeWidth=0.8))
        if side == "left":
            s(d, x - 6, y - 2.5, name, 7, INK, "end")
        elif side == "right":
            s(d, x + 6, y - 2.5, name, 7, INK, "start")
        else:
            s(d, x, y + 7, name, 7, INK)

    def jumper(x1, y1, x2, y2, color=JUMP):
        d.add(Line(x1, y1, x2, y2, strokeColor=color, strokeWidth=5,
                   strokeLineCap=1))

    top = H - 52
    sp = 12
    # A: power-up mode, the J10-J19 column (J19 at the top)
    s(d, 80, H - 12, "Power-up mode", 9, INK, bold=True)
    s(d, 80, H - 23, "column J10–J19, middle of the board", 7, GREY)
    order = ["J19", "J18", "J17", "J16", None, "J15", "J14", "J13", "J12",
             "J11", None, "J10"]
    for col, (x, title, pair, color) in enumerate((
            (45, "factory", ("J19", "J18"), JUMP),
            (125, "no ROMs", ("J18", "J17"), WARN))):
        s(d, x, top + 12, title, 7.5, color, bold=True)
        ys = {}
        y = top
        for n in order:
            if n:
                stake(x, y, n, "left" if col == 0 else "right")
                ys[n] = y
            y -= sp if n else sp * 0.6
        jumper(x, ys[pair[0]], x, ys[pair[1]], color)
    s(d, 45, 18, "mode 2: boot ROMs", 7, GREY)
    s(d, 125, 18, "mode 1: ODT", 7, WARN, bold=True)

    # B: character format of the console port, J36-J40 (J40 at the top)
    s(d, 245, H - 12, "Console character format", 9, INK, bold=True)
    s(d, 245, H - 23, "left edge, below J2 (J38 = ground)", 7, GREY)
    ys = {}
    for i, n in enumerate(["J40", "J39", "J38", "J37", "J36"]):
        ys[n] = top - i * sp
        stake(245, ys[n], n, "left")
    jumper(245, ys["J38"], 245, ys["J37"])
    for i, t in enumerate(["only J38–J37:", "8 bits, no parity,",
                           "1 stop bit (8N1)", "J39, J36, J40 open"]):
        s(d, 262, top - 12 - i * 10, t, 7, INK, "start")
    s(d, 245, top - 5 * sp - 26, "(2nd port: J28–J32, J30 = ground,",
      6.5, GREY)
    s(d, 245, top - 5 * sp - 35, "factory J30–J29 only)", 6.5, GREY)

    # C: baud clock, J41-J46 across the top-left corner
    s(d, 400, H - 12, "Baud clock (internal)", 9, INK, bold=True)
    s(d, 400, H - 23, "top-left corner, by J2", 7, GREY)
    xs = {}
    for i, n in enumerate(["J46", "J45", "J44", "J43", "J42", "J41"]):
        xs[n] = 345 + i * 22
        stake(xs[n], top - 10, n, "top")
    jumper(xs["J46"], top - 10, xs["J45"], top - 10)
    jumper(xs["J43"], top - 10, xs["J42"], top - 10)
    for i, t in enumerate(["J43–J42 in: console uses the", "on-board clock "
                           "(S2 1–4 work)", "J46–J45 in: same for the 2nd port",
                           "J41, J44 open (external clock)"]):
        s(d, 345 - 4, top - 34 - i * 10, t, 7, INK, "start")
    s(d, 400, 18, "blue = factory jumper   red = change", 7, GREY)
    return d


# --- Figure 4: the two switch packs, factory settings ---------------------
S1_FUNC = {1: "CPU test", 2: "memory test", 3: "DECnet boot", 4: "console dialogue",
           5: "–", 6: "–", 7: "RL01/RL02 boot", 8: "–"}


def fig_switches():
    W, H = 460, 200
    d = Drawing(W, H)
    rh = 17                          # one switch
    top = H - 34                     # switch 1 at the top (handle edge)

    def pack(x0, label, on, notes):
        d.add(Rect(x0 - 6, top - 8 * rh - 4, 52, 8 * rh + 10,
                   fillColor=SWBLUE, strokeColor=None))
        for i in range(8):
            y = top - (i + 1) * rh
            d.add(Rect(x0, y + 2, 40, rh - 4, fillColor=colors.white,
                       strokeColor=None))
            kx = x0 + 22 if (i + 1) in on else x0 + 2
            d.add(Rect(kx, y + 4, 16, rh - 8, fillColor=INK,
                       strokeColor=None))
            s(d, x0 - 12, y + 5, str(i + 1), 8, INK, bold=True)
            if notes.get(i + 1):
                s(d, x0 + 56, y + 5, notes[i + 1], 7.5, INK, "start")
        s(d, x0 + 4, top + 6, "OFF", 7, INK, bold=True)
        s(d, x0 + 34, top + 6, "ON", 7, INK, bold=True)
        s(d, x0 + 20, top + 20, label, 8.5, WARN, bold=True)
        s(d, x0 + 20, top - 8 * rh - 18, "toward the CPU chips: ON  →",
          6.5, GREY)

    pack(40, "S2 (E114) baud", {1, 5},
         {1: "console J1:", 2: "9600", 5: "2nd SLU J2:", 6: "9600"})
    pack(260, "S1 (E102) boot/diag", {1, 2, 4, 7}, S1_FUNC)
    return d


# --- Figure 5: the 10-pin header, face on -----------------------------------
HDR = {1: ("Clock in", GREY, "leave open"),
       2: ("GND", INK, "to DE-9 pin 5"),
       3: ("Transmit +", C21, "to DE-9 pin 2"),
       4: ("Transmit −", GREY, "leave open"),
       5: ("GND", GREY, "leave open"),
       6: ("(no pin: key)", GREY, ""),
       7: ("Receive −", C20, "join to pin 9"),
       8: ("Receive +", C18, "to DE-9 pin 3"),
       9: ("GND", C20, "join to pin 7"),
       10: ("+12 V", WARN, "leave open!")}


def fig_header():
    W, H = 460, 150
    d = Drawing(W, H)
    cx0 = 120
    pitch = 52
    yt, yb = 92, 52
    d.add(Rect(cx0 - 30, yb - 24, 4 * pitch + 60, yt - yb + 48,
               fillColor=colors.HexColor("#333333"), strokeColor=None))
    for col in range(5):
        odd = 9 - 2 * col                 # top row 9 7 5 3 1 (left to right)
        even = odd + 1                    # bottom row 10 8 6 4 2
        x = cx0 + col * pitch
        for n, y in ((odd, yt), (even, yb)):
            name, c, _ = HDR[n]
            if n == 6:
                d.add(Rect(x - 5, y - 5, 10, 10, fillColor=colors.HexColor(
                    "#333333"), strokeColor=colors.HexColor("#999999"),
                    strokeWidth=0.8, strokeDashArray=[2, 2]))
            else:
                d.add(Rect(x - 5, y - 5, 10, 10, fillColor=colors.HexColor(
                    "#d9b44a"), strokeColor=None))
            s(d, x + 12, y - 3, str(n), 9, colors.white, "start", True)
    for col in range(5):
        odd = 9 - 2 * col
        x = cx0 + col * pitch
        name, c, _ = HDR[odd]
        s(d, x, yt + 30, name, 7.5, c if c is not GREY else GREY,
          bold=c not in (GREY,))
        name, c, _ = HDR[odd + 1]
        s(d, x, yb - 36, name, 7.5, c, bold=c not in (GREY,))
    s(d, cx0 + 4 * pitch + 42, yt + 1, "pin 1 → upper right", 8, INK,
      "start")
    s(d, cx0 + 4 * pitch + 42, yb - 2, "pin 2 below it", 8, INK, "start")
    s(d, cx0 - 40, yb - 2, "pin 10", 8, INK, "end")
    s(d, W / 2, 6, "Header J1 (or J2) seen face-on, board component side up "
      "(DEC's orientation).  Confirm with a meter: see the checks below.",
      7.5, GREY)
    return d


# --- Figure 6: the cable ----------------------------------------------------
def fig_cable():
    W, H = 470, 250
    d = Drawing(W, H)
    # left: 10-pin socket as a column of wires 1..10
    lx = 110
    top = H - 30
    step = 18
    s(d, lx, H - 12, "10-pin IDC socket", 9, INK, bold=True)
    s(d, lx, H - 22, "(ribbon wire n = pin n; red stripe = 1)", 7, GREY)
    ys = {}
    for n in range(1, 11):
        y = top - n * step
        ys[n] = y
        name, c, note = HDR[n]
        d.add(Rect(lx - 10, y - 5, 20, 10, fillColor=colors.white,
                   strokeColor=GREY, strokeWidth=0.6))
        s(d, lx, y - 3, str(n), 8, INK, bold=True)
        s(d, lx - 32, y - 3, name, 7.5, c if c is not GREY else GREY,
          "end", bold=c is not GREY)
    # right: DE-9 female (to the PC)
    rx = 360
    s(d, rx, H - 12, "DE-9 female (plugs onto the PC)", 9, INK, bold=True)
    de9 = {2: "2  RxD (PC receives)", 3: "3  TxD (PC sends)",
           5: "5  Signal ground"}
    dys = {2: ys[3], 3: ys[8], 5: ys[2]}
    for n, label in de9.items():
        y = dys[n]
        d.add(Rect(rx - 10, y - 5, 20, 10, fillColor=colors.white,
                   strokeColor=GREY, strokeWidth=0.6))
        s(d, rx, y - 3, str(n), 8, INK, bold=True)
        s(d, rx + 16, y - 3, label[3:], 7.5, INK, "start")
    # wires
    d.add(Line(lx + 10, ys[3], rx - 10, dys[2], strokeColor=C21,
               strokeWidth=1.8))
    d.add(Line(lx + 10, ys[8], rx - 10, dys[3], strokeColor=C18,
               strokeWidth=1.8))
    d.add(Line(lx + 10, ys[2], rx - 10, dys[5], strokeColor=INK,
               strokeWidth=1.8))
    # 7 to 9 jumper at the socket end, on the left so it crosses nothing
    jx = lx - 22
    d.add(PolyLine([lx - 10, ys[7], jx, ys[7], jx, ys[9], lx - 10, ys[9]],
                   strokeColor=C20, strokeWidth=1.8))
    s(d, jx - 4, ys[10] - 18, "join 7 and 9 (short loop at the socket)",
      7.5, C20, "start", True)
    # cut wires
    for n in (1, 4, 5, 6, 10):
        y = ys[n]
        d.add(Line(lx + 10, y, lx + 22, y, strokeColor=GREY, strokeWidth=1))
        d.add(Line(lx + 20, y - 3, lx + 24, y + 3, strokeColor=GREY,
                   strokeWidth=1))
    s(d, lx + 30, ys[10] - 3, "cut back and insulate (pin 10 is +12 V)",
      7.5, WARN, "start")
    s(d, (lx + rx) / 2, 2, "Optional on the DE-9 for fussy software: "
      "join 7+8 (RTS-CTS) and 1+4+6 (DCD-DTR-DSR).", 7.5, GREY)
    return d


# --- Document ---------------------------------------------------------------
story = []
story += [p("PDP-11/23+ in the BA11-N", H1),
          p("22-bit bus modification of the H9273 backplane, setting up "
            "the KDF11-B (M8189), and a console cable for its 10-pin "
            "headers", SUB)]

story += [p("What this covers", H2),
          p("Your box is a BA11-N (PDP-11/23-AX) with the H9273 backplane. "
            "DEC built the H9273 for 18-bit addressing: the pins that carry "
            "the extra address lines BDAL18–21 are present in every "
            "slot but are not wired together. The 11/23+ CPU (KDF11-B, "
            "M8189) addresses up to 4 MB over those four lines. The "
            "modification is four wires, each joining one pin in all nine "
            "rows. Without it the 11/23+ still runs, limited to 256 KB, "
            "exactly as your 11/23 does today."),
          p("<b>The order to work in:</b>")]
story += steps([
    "Check the cards in the box (page 4): a few old cards use the same four "
    "pins for something else and must come out first.",
    "Modify the backplane (pages 2–3) and check it with a meter, all "
    "cards out.",
    "Set up the 11/23+ board and switch off the QBone devices it replaces "
    "(pages 4–8). <b>This step is required:</b> the 11/23+ has its own console "
    "port, second serial port and clock register at the same addresses as "
    "the QBone's DL11, 2nd DL11 and KW11.",
    "Make the console cable (pages 9–10) and power up into ODT.",
])
story += [p("Your current 11/23 CPU board (KDF11-A) can only go back into "
            "the modified backplane if it is etch revision C or later. "
            "Earlier ones use the same four pins for other signals (DEC "
            "MicroNote 5). The revision letter is etched in the copper "
            "next to the board's 50-xxxxx etch number.", SMALL)]

story += [PageBreak(), p("1. The backplane modification", H2),
          p("The four lines are on <b>slot B, side 1</b> of every row, at "
            "pin positions C, D, E and F (DEC names <b>BC1, BD1, BE1, "
            "BF1</b>). They are the same positions as AC1/AD1 "
            "(BDAL16/17) on slot A, which DEC already wired. Connect BC1 "
            "of row 1 to BC1 of row 2, row 2 to row 3, and so on down to "
            "row 9; the same for BD1, BE1 and BF1. Wire every row, empty "
            "ones too, in one unbranched run per line. Leave slots C and D "
            "alone: on the H9273 they are the CD bus, not the Q-bus."),
          KeepTogether([fig_backplane(),
          p("Figure 1. The H9273 seen from the back, where you solder. "
            "Everything is mirrored compared with the card side: slot A is "
            "on the right. Row 1 (the CPU row) is at the top.", CAP)]),
          KeepTogether([fig_slotb(),
          p("Figure 2. One row of slot B, from the back. Directly below "
            "BD1 is BD2, which is <b>+12 V</b>: a solder bridge there "
            "would put 12 V on an address line.", CAP)])]

story += [PageBreak(), p("Finding the pins with a meter", H3),
          p("Mirror images are easy to get wrong, so confirm the pins "
            "before soldering. Power off, all cards out, meter on "
            "continuity or ohms:")]
story += steps([
    "Find side 2 by its power pins. In both slots A and B, side 2 starts "
    "(from the A end) with <b>+5 V, −12 V, GND, +12 V</b>. The H786 "
    "supply has no −12 V, so that pin may be open. Check the other three "
    "against the power supply's +5 V, GND and +12 V terminals or wires.",
    "Tell slot B from slot A. On side 1, pins C and D of <b>slot A</b> "
    "(BDAL16/17) are already wired: row 1 beeps to row 2. On <b>slot B</b>, "
    "pins C, D, E and F do not beep to the next row. Their neighbours A and "
    "B (BDCOK, BPOK) do.",
    "So the targets are: slot B, side 1, the four pins directly above GND, "
    "+12 V, BDAL02 and BDAL03 of side 2. Before the mod none of them is "
    "connected to anything.",
])
story += [p("Doing the wiring", H3)]
story += bullets([
    "The H9273 has short pins, not wire-wrap posts, so the wires are "
    "soldered. 30 AWG Kynar wire-wrap wire is easy to route; anything "
    "from 30 to 24 AWG is fine. Each hop is just the row-to-row spacing.",
    "One colour per line helps. Use the colours of Figure 1, or tag "
    "the ends.",
    "Pre-tin the wire, heat briefly, and look at each joint with a "
    "magnifier for bridges to the side-2 pin below it and to the "
    "neighbouring pins.",
])
story += [p("Checks after soldering (all cards still out)", H3)]
story += [table([
    ["Check", "Expected"],
    ["BC1 row 1 to BC1 row 9 (and the same for BD1, BE1, BF1)",
     "continuity (0 Ω)"],
    ["Between any two of the four lines (e.g. BC1 to BD1, row 1)",
     "open"],
    ["Each line to GND (BC2), +5 V (BA2), +12 V (BD2) and BB2",
     "open"],
    ["Each line to its neighbours BB1 (BPOK) and BH1", "open"],
    ["BDAL16/17 (AC1, AD1) row 1 to row 9, as a sanity check of the "
     "method", "continuity, as before"],
], [4.3 * inch, 2.4 * inch])]
story += [Spacer(1, 4), p("Do these checks with the cards out: with cards "
                          "in, the bus terminators make every line read a "
                          "few hundred ohms to +5 V and ground.", SMALL)]

story += [PageBreak(), p("2. Before the first power-on", H2),
          p("Cards that clash with the new wiring", H3),
          p("DEC MicroNote 5 lists cards that use BC1–BF1 for other "
            "purposes. Take any of these out of the box, or check them "
            "first:")]
story += [table([
    ["Card", "Problem in a 22-bit backplane"],
    ["KDF11-A M8186 (11/23 CPU) before etch rev C", "uses BC1–BF1 for "
     "other signals"],
    ["RLV11 (M8013 + M8014) RL controller", "uses BC1, BD1; 18-bit DMA "
     "only"],
    ["KWV11-A clock, AAV11 D/A, ADV11 A/D", "use BC1"],
    ["LSI-11 and LSI-11/2 CPUs (KD11-F, KD11-HA)", "use these pins; 16-bit "
     "only"],
    ["DLV11-J M8043, CS rev E or earlier", "incompatible with the KDF11-A "
     "and KDF11-B"],
    ["BDV11, KPV11-B/-C, REV11, TEV11 terminators", "terminate 18 bits "
     "only (fine in a single box)"],
], [3.3 * inch, 3.4 * inch])]
story += [Spacer(1, 4), p("Memory: an 18-bit memory card (MSV11-D/E, "
                          "MSV11-C) works, but only while software stays "
                          "below 256 KB. 11nix and Unix V6 run the MMU in "
                          "18-bit mode, so nothing changes for them. A "
                          "22-bit OS would see such a card repeated every "
                          "256 KB.", SMALL)]

story += [p("QBone: turn off the devices the 11/23+ has built in", H3),
          p("Two devices answering at one address corrupt each other's "
            "data. In the control panel, before booting:")]
story += [table([
    ["QBone device", "Address", "11/23+ has", "What to do"],
    ["DL11 console SLU", "777560", "console port J1",
     "<b>Off</b> (you will use J1 and the new cable)"],
    ["DL11 2nd SLU (TU58)", "776500", "2nd port J2",
     "<b>Off</b>, or keep it and move J2 to 776540 (jumper J10–J12)"],
    ["KW11 line clock", "777546", "clock register (BEVNT)",
     "<b>Off</b> if the box's own line clock works (below)"],
    ["Address width", "", "", "Leave at <b>18</b> for 11nix and V6"],
], [1.45 * inch, 0.7 * inch, 1.35 * inch, 3.2 * inch])]
story += [Spacer(1, 4)]
story += bullets([
    "<b>Line clock:</b> the 11/23+'s clock register only passes on BEVNT, "
    "a 60 Hz signal from the H786 power supply, through backplane jumper "
    "W1 (factory installed). DEC's 11/03L front panel has an AUX ON/OFF "
    "switch that can gate it; <b>your front panel has no AUX switch</b>, "
    "so measure instead: power on, meter from <b>BR1</b> (slot B, side 1, "
    "pin R, BEVNT L) to chassis ground. <b>60 Hz</b> on the Hz range: the "
    "clock is there. A steady 3 to 3.5 V: no clock; check W1. Keep the "
    "probe off BS1 next to it (+12 V). In 11nix, "
    "<font face='Courier'>date; sleep 5; date</font> must show 5 seconds "
    "passing.",
    "<b>If there is no BEVNT:</b> keep the QBone KW11 on and add jumper "
    "J10–J15 on the 11/23+. That also turns off its boot ROMs and "
    "diagnostic registers, which you do not use.",
    "<b>Alternative console:</b> to keep the console on the QBone as today "
    "(no cable needed), leave the QBone DL11 on and add jumper J10–J14 "
    "to turn off the board's console port. Break-on-halt is already off "
    "from the factory, as DEC requires for this.",
])

story += [PageBreak(), p("3. Setting up the KDF11-B board", H2)]
story += [table([
    ["Setting", "Factory", "For your system"],
    ["Console baud rate, S2 switches 1–4 (E114, the pack nearer the "
     "outer edge)", "9600", "9600: switch 1 ON, 2–4 OFF"],
    ["2nd SLU baud rate, S2 switches 5–8", "9600",
     "9600: switch 5 ON, 6–8 OFF"],
    ["Boot/diagnostic options, S1 (E102, the pack nearer the CPU chips)",
     "1, 2, 4, 7 ON", "keep; S1 matters only with boot ROMs in "
     "E126/E127 (table below)"],
    ["Character format (J36–J40)", "8 bits, no parity, 1 stop",
     "keep"],
    ["Power-up mode: jumpers J18–J19 and J18–J17", "mode 2: "
     "J18–J19 in, J18–J17 out (boot ROM at 173000)",
     "<b>mode 1 (ODT): J18–J19 out, J18–J17 in</b> if ROM "
     "sockets E126/E127 are empty"],
    ["Halt/trap J16–J18", "out: HALT enters ODT", "keep"],
    ["Break-on-halt J3/J4/J5", "off (J5–J4 in)", "keep"],
    ["Bus grant jumpers W1, W2", "installed", "needed only in Q/Q "
     "backplanes. On the H9273 they connect CM1–CN1 and CR1–CS1 "
     "of row 2: remove them if the card in row 2 uses its C/D fingers"],
    ["Internal baud clock J43–J42, J45–J46", "internal",
     "keep"],
], [2.15 * inch, 1.9 * inch, 2.65 * inch])]
story += [PageBreak(), p("Where the jumpers and switches are", H3),
          p("The board has no silkscreen, so go by DEC's drawing. It shows "
            "the component side with the handle edge (J1, J2) at the top "
            "and the contact fingers at the bottom. Every jumper stake is "
            "drawn where it is on the board, as a small circle with its "
            "number; a bracket joining two stakes is a jumper installed at "
            "the factory. The stakes are in short columns, so count from a "
            "landmark: the J10–J19 column sits just below the CPU chips, "
            "J10 at the bottom; J36–J40 are at the left edge below J2; "
            "J41–J46 run along the top-left corner."),
          KeepTogether([fig_dec_board(), p(
    "Figure 3. DEC Figure 2-1, KDF11-BA jumper, switch and diagnostic "
    "display locations (KDF11-BA CPU Module User's Guide). <b>S2, baud "
    "rates</b> (E114) is the switch pack nearer the outer edge and the ROM "
    "sockets; <b>S1, boot/diagnostic</b> (E102) is nearer the CPU chips. "
    "Brackets are the factory jumpers.", CAP)])]
story += [PageBreak(), p("The jumpers that matter for the console", H3),
          KeepTogether([fig_jumpers(), p(
    "Figure 4. Close-ups of three stake groups of Figure 3, in their real "
    "order. On a board without boot ROMs, move the power-up jumper from "
    "J19–J18 to J18–J17 (mode 1), so the board starts in ODT and prints "
    "@. If a previous owner changed the other two groups, set them back as "
    "shown: wrong format jumpers give garbled characters, and with an "
    "external clock selected the S2 switches do nothing. DEC Tables 2-4, "
    "2-11, 2-12, 2-17, 2-19.", CAP)])]
story += [p("The two switch packs", H3),
          KeepTogether([fig_switches(), p(
    "Figure 5. Both packs as they sit on the board, at the factory "
    "settings: switch 1 at the handle end, ON toward the CPU chips (DEC's "
    "drawing; if your switches have ON printed on them, trust that). S2: "
    "9600 baud on both ports (DEC Tables 2-10, 2-14, 2-21). S1: DEC "
    "Table 2-20.", CAP)])]
story += [Spacer(1, 6), KeepTogether([table([
    ["S2 1–4 (console) or 5–8 (2nd port)", "Baud rate"],
    ["1 ON, 2 OFF, 3 OFF, 4 OFF  (5 ON, 6–8 OFF)", "<b>9600</b>"],
    ["all four OFF", "19200"],
    ["1 OFF, 2 ON, 3 OFF, 4 OFF", "7200"],
    ["1 ON, 2 ON, 3 OFF, 4 OFF", "4800"],
    ["1 ON, 2 OFF, 3 ON, 4 OFF", "2400  (more in DEC Table 2-10)"],
    ["1 OFF, 2 OFF, 3 OFF, 4 ON", "1200"],
    ["1 OFF, 2–4 ON  (9600 set the wrong way round)", "75"],
], [4.2 * inch, 2.5 * inch])])]
story += [table([
    ["S1 switch", "Factory", "Function (read by the boot ROMs only)"],
    ["1", "ON", "Run the CPU diagnostic at power-up"],
    ["2", "ON", "Run the memory diagnostic (Ctrl-C at the console skips "
     "it)"],
    ["3", "OFF", "DECnet boot disabled"],
    ["4", "ON", "Console test and boot dialogue"],
    ["5, 6", "OFF", "not used"],
    ["7", "ON", "Include the RL01/RL02 bootstrap program"],
    ["8", "OFF", "not used"],
], [0.9 * inch, 0.8 * inch, 5.0 * inch])]
story += [Spacer(1, 4), p(
    "With these settings and the ROMs fitted, the board tests the CPU and "
    "memory at power-up, then starts the console dialogue. With S1 "
    "switch 7 on, the ROMs can also boot an RL02 by themselves, so 11nix "
    "might one day boot without the QBone depositing its loader. This is "
    "untried; 11nix's boot block does work on a real RL controller.",
    SMALL)]
story += bullets([
    "<b>Mode 2 with DEC's BDV11 ROMs</b> in E126/E127: the board runs its "
    "CPU and memory tests and asks <font face='Courier'>START?</font>. "
    "Type <font face='Courier'>N</font> for ODT, then load and start "
    "11nix as now (START in the panel, then "
    "<font face='Courier'>10000g</font>).",
    "<b>Chips:</b> 11nix needs the MMU chip (KTF11-A) in socket E74. If the "
    "floating-point chip (KEF11-A) is in E76, programs with floating "
    "point, such as advent, will run.",
    "<b>Where things are:</b> J1 (console, “J1 CONSOLE SLU 1”) "
    "and J2 (“SLU 2”) are side by side on the handle edge, J2 "
    "nearer the corner (Figure 3).",
    "<b>Addresses:</b> console 777560 (vectors 60/64), 2nd SLU 776500 "
    "(300/304), clock register 777546 (vector 100), boot ROMs 173000.",
])

story += [PageBreak(), p("4. The console cable", H2),
          p("J1 and J2 are DEC's standard 10-pin serial header (the same as "
            "on the DLV11-J and 11/73). The port is RS-232 with no modem "
            "signals. The cable goes from a 10-pin IDC socket on ribbon "
            "cable to a DE-9 <b>female</b> that plugs onto the PC's "
            "serial port. It includes the null-modem crossing, so no "
            "adapter is needed."),
          KeepTogether([fig_header(),
          p("Figure 6. The header pins and what each one carries.", CAP)]),
          KeepTogether([fig_cable(),
          p("Figure 7. Wiring. This is the “DCE DE-9S” column of "
            "the standard DEC pinout.", CAP)])]
story += [table([
    ["Header pin", "Signal", "DE-9 female (PC)", "Note"],
    ["2", "Ground", "5", ""],
    ["3", "Transmit +  (data from the 11/23+)", "2 (RxD)", ""],
    ["8", "Receive +  (data to the 11/23+)", "3 (TxD)", ""],
    ["7 + 9", "Receive − tied to ground", "—",
     "required for RS-232"],
    ["1, 4, 5, 6, 10", "clock in, Transmit −, ground, key, +12 V",
     "—", "not connected; pin 10 is +12 V"],
], [0.95 * inch, 2.6 * inch, 1.3 * inch, 1.85 * inch])]

story += [p("Checking the header orientation with a meter", H3)]
story += steps([
    "Board out of the box, unpowered. The empty position (pin 6) marks the "
    "<b>even</b> row: 2, 4, (6), 8, 10.",
    "Pins 2, 5 and 9 are ground: they beep to each other and to any ground "
    "finger. The <b>end pin of the even row that beeps to ground is pin "
    "2</b>; the other end is pin 10 (+12 V).",
    "Make the cable so that ribbon wire 1 lands on pin 1. If your IDC "
    "socket has a solid position for the key, put it at pin 6.",
])
story += [p("First test", H3)]
story += steps([
    "Plug the cable onto J1 and the PC's serial port (/dev/ttyS0). Run "
    "picocom at 9600 8N1 with no flow control, as now.",
    "In power-up mode 1 the 11/23+ prints six octal digits (the PC), then "
    "<font face='Courier'>@</font>. Typing "
    "<font face='Courier'>177560/</font> should show the console's "
    "receiver status. A lone <font face='Courier'>@</font> or the "
    "<font face='Courier'>START?</font> prompt means the cable works.",
    "Nothing at all: swap header pins 3 and 8 at the DE-9 (pins 2 and 3), "
    "then check pin 7 is joined to 9.",
    "<b>One garbled character at power-up, then nothing:</b> that is the "
    "line switching on, not a baud problem. The board is in mode 2 with no "
    "boot ROMs and never reaches ODT. Flip HALT down: if ODT then prints "
    "a number and @ cleanly, the speed is right; set mode 1 (Figure 4).",
    "<b>Several garbled characters:</b> the speed or format is wrong. Try "
    "picocom at 300, 1200, 2400 and 19200; check S2 1–4 against the table "
    "above (ON as printed on the switch), J43–J42 (internal clock) and "
    "J38–J37 (8N1).",
])

story += [Spacer(1, 10), p("Sources", H3)]
story += bullets([
    "DEC, <i>KDF11-BA CPU Module User's Guide</i> (EK-KDFEB-UG-001): "
    "Tables 2-4, 2-6, 2-10 to 2-14, 2-17 to 2-22; Figures "
    "2-1, 2-3; Appendix D. bitsavers.org/pdf/dec/pdp11/1123/"
    "KDF11BA_UsersManual.pdf",
    "DEC, <i>BA11-N Mounting Box User's Guide</i> (EK-BA11N-UG-001): "
    "H9273 layout, backplane jumpers W1–W3, AUX ON/OFF and LTC. "
    "bitsavers.org/pdf/dec/qbus/BA11-N/",
    "DEC MicroNote 5, <i>Q22 Compatible Options</i> (1984).",
    "Computer History Wiki: “Upgrading QBUS backplanes”, "
    "“QBUS”, “DEC asynchronous serial line pinout” "
    "(gunkies.org).",
    "Frain Research, “22-Bit Q-Bus Conversions” "
    "(web.frainresearch.org).",
], SMALL)


def footer(c, doc):
    c.saveState()
    c.setFont("Helvetica", 7.5)
    c.setFillColor(GREY)
    c.drawString(doc.leftMargin, 0.45 * inch,
                 "PDP-11/23+ in the BA11-N — N9TAX RetroLAB")
    c.drawRightString(letter[0] - doc.rightMargin, 0.45 * inch,
                      "page %d" % doc.page)
    c.restoreState()


doc = SimpleDocTemplate(OUT, pagesize=letter, leftMargin=0.8 * inch,
                        rightMargin=0.8 * inch, topMargin=0.7 * inch,
                        bottomMargin=0.75 * inch,
                        title="PDP-11/23+ in the BA11-N",
                        author="N9TAX RetroLAB")
doc.build(story, onFirstPage=footer, onLaterPages=footer)
print(OUT)
