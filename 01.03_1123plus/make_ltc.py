#!/usr/bin/env python3
"""The BA11-N line clock: how the H786 power supply's LTC signal reaches
BEVNT on the backplane, with test points.  For finding why the 11/23+ gets
no clock ticks.

    python3 make_ltc.py [out.pdf]

Needs reportlab.  The two scanned crops (h786-*.png) are from DEC's MNC11
print set MP00652 (H786 drawings).
"""
import os
import sys

from reportlab import rl_config
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import letter
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import inch
from reportlab.graphics.shapes import Circle, Drawing, Line, PolyLine, Rect, String
from reportlab.platypus import (Image, KeepTogether, PageBreak, Paragraph,
                                SimpleDocTemplate, Spacer, Table, TableStyle)

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "ba11n-line-clock.pdf")
# CUPS (pdftopdf) cannot decode ASCII85+Flate streams and prints blank pages
rl_config.useA85 = 0

INK = colors.HexColor("#222222")
GREY = colors.HexColor("#888888")
LIGHT = colors.HexColor("#eeeeee")
WARN = colors.HexColor("#b00000")
SIG = colors.HexColor("#1f77b4")        # the clock signal
AC = colors.HexColor("#2ca02c")         # the AC sample
TPC = colors.HexColor("#d62728")        # test points
BOXF = colors.HexColor("#f6f6f6")

ss = getSampleStyleSheet()
BODY = ParagraphStyle("body", parent=ss["BodyText"], fontName="Helvetica",
                      fontSize=10, leading=13.5, spaceAfter=5)
SMALL = ParagraphStyle("small", parent=BODY, fontSize=8.5, leading=11)
H1 = ParagraphStyle("h1", parent=ss["Title"], fontName="Helvetica-Bold",
                    fontSize=19, leading=23, spaceAfter=4)
SUB = ParagraphStyle("sub", parent=BODY, alignment=TA_CENTER, fontSize=10.5,
                     textColor=GREY, spaceAfter=10)
H2 = ParagraphStyle("h2", parent=ss["Heading2"], fontName="Helvetica-Bold",
                    fontSize=13.5, leading=17, spaceBefore=8, spaceAfter=5)
CAP = ParagraphStyle("cap", parent=BODY, fontSize=8.5, leading=11,
                     alignment=TA_CENTER, textColor=GREY, spaceAfter=8)
CELL = ParagraphStyle("cell", parent=BODY, fontSize=9, leading=11.5, spaceAfter=0)
CELLB = ParagraphStyle("cellb", parent=CELL, fontName="Helvetica-Bold")


def p(t, st=BODY):
    return Paragraph(t, st)


def bullets(items, st=BODY):
    return [Paragraph(t, st, bulletText="•") for t in items]


def steps(items, st=BODY):
    return [Paragraph(t, st, bulletText="%d." % (i + 1)) for i, t in enumerate(items)]


def table(rows, widths):
    data = [[Paragraph(c, CELLB if r == 0 else CELL) for c in row]
            for r, row in enumerate(rows)]
    t = Table(data, colWidths=widths, repeatRows=1)
    st = [("GRID", (0, 0), (-1, -1), 0.4, GREY),
          ("VALIGN", (0, 0), (-1, -1), "TOP"),
          ("BACKGROUND", (0, 0), (-1, 0), LIGHT),
          ("TOPPADDING", (0, 0), (-1, -1), 3),
          ("BOTTOMPADDING", (0, 0), (-1, -1), 3)]
    for r in range(2, len(rows), 2):
        st.append(("BACKGROUND", (0, r), (-1, r), colors.HexColor("#f8f8f8")))
    t.setStyle(TableStyle(st))
    return t


def s(d, x, y, text, size=8, color=INK, anchor="start", bold=False):
    d.add(String(x, y, text, fontName="Helvetica-Bold" if bold else "Helvetica",
                 fontSize=size, fillColor=color, textAnchor=anchor))


def box(d, x, y, w, h, title, lines=(), dash=None, fill=BOXF, tsize=8.5):
    d.add(Rect(x, y, w, h, fillColor=fill, strokeColor=INK, strokeWidth=0.8,
               strokeDashArray=dash))
    s(d, x + 5, y + h - 12, title, tsize, INK, bold=True)
    for i, t in enumerate(lines):
        s(d, x + 5, y + h - 24 - i * 10, t, 7.2, INK)


def wire(d, pts, color=SIG, width=1.6, dash=None):
    d.add(PolyLine(pts, strokeColor=color, strokeWidth=width, strokeDashArray=dash))


def tp(d, x, y, n):
    d.add(Circle(x, y, 7, fillColor=colors.white, strokeColor=TPC, strokeWidth=1.2))
    s(d, x, y - 3, str(n), 7.5, TPC, "middle", True)


def pin(d, x, y, label, color=SIG):
    d.add(Circle(x, y, 2.4, fillColor=color, strokeColor=None))
    if label:
        s(d, x + 4, y + 3, label, 6.5, GREY)


# --- Figure 1: the whole path ------------------------------------------------
def fig_path():
    W, H = 520, 430
    d = Drawing(W, H)
    # H786 power supply
    d.add(Rect(6, 124, 256, 298, fillColor=None, strokeColor=GREY,
               strokeWidth=1, strokeDashArray=(4, 3)))
    s(d, 14, 410, "H786 power supply", 10, INK, bold=True)

    box(d, 16, 342, 72, 50, "T1", ["start-up", "transformer"])
    box(d, 104, 272, 150, 128, "Monitor board", [
        "54-12528, plugs into J3",
        "E1: LM339 comparator,",
        "  pins 11 (+), 10 (-), out 13,",
        "  R14 120k hysteresis",
        "R21 820 Ω to Q1 base,",
        "  R48 1.8 kΩ base to +12 V",
        "Q1: MPSA05, open collector,",
        "  emitter to ground,",
        "  no pull-up on the board"])
    box(d, 16, 136, 236, 104, "Master board", [
        "54-12530",
        "J3: socket for the monitor",
        "  board (contacts A, B, D)",
        "J5, J6: two 10-pin headers",
        "  wired in parallel, pin for pin",
        "  pin 2 = RTC = contact A"])
    pin(d, 130, 228, "", AC)
    pin(d, 176, 228, "")
    s(d, 112, 218, "J3-B, D", 6.5, AC)
    s(d, 172, 218, "J3-A", 6.5, SIG, "end")
    d.add(Rect(204, 192, 40, 18, fillColor=colors.white, strokeColor=INK))
    s(d, 224, 198, "J5", 8, INK, "middle", True)
    d.add(Rect(204, 148, 40, 18, fillColor=colors.white, strokeColor=INK))
    s(d, 224, 154, "J6", 8, INK, "middle", True)

    # AC sample: T1 -> J3-B/D -> monitor fingers AB/AD
    wire(d, [52, 342, 52, 256, 130, 256], AC)
    wire(d, [130, 228, 130, 272], AC)
    s(d, 74, 260, "low-voltage AC", 7, AC)
    s(d, 134, 260, "AB, AD", 6.5, AC)
    tp(d, 62, 256, 1)
    # RTC: monitor finger AA -> J3-A -> J5-2 and J6-2
    wire(d, [176, 272, 176, 228])
    wire(d, [176, 228, 176, 201, 204, 201])
    wire(d, [190, 201, 190, 157, 204, 157])
    s(d, 180, 260, "AA (RTC)", 6.5, SIG)
    tp(d, 240, 286, 2)
    tp(d, 192, 246, 3)

    # cable A -> backplane
    wire(d, [244, 201, 290, 201, 290, 340, 326, 340])
    s(d, 266, 205, "pin 2", 7, SIG)
    box(d, 316, 268, 196, 144, "H9273 backplane", [])
    d.add(Rect(326, 328, 46, 24, fillColor=colors.white, strokeColor=INK))
    s(d, 349, 342, "10-pin", 7, INK, "middle", True)
    s(d, 349, 332, "pin 2", 7, INK, "middle")
    d.add(Rect(386, 333, 34, 14, fillColor=colors.white, strokeColor=INK))
    s(d, 403, 337, "W1 0 Ω", 6.5, INK, "middle")
    wire(d, [372, 340, 386, 340])
    wire(d, [420, 340, 470, 340, 470, 312])
    s(d, 426, 358, "BR1 = BEVNT L", 7.5, SIG, bold=True)
    s(d, 426, 348, "all 9 rows", 6.5, GREY)
    tp(d, 458, 324, 4)
    box(d, 432, 276, 74, 36, "KDF11-B", ["row 1: counts", "falling edges"],
        tsize=7.5)
    s(d, 324, 312, "Pull-up: BR1 sat at 5 V", 7, WARN)
    s(d, 324, 302, "with no cards in; source", 7, WARN)
    s(d, 324, 292, "not found yet (perhaps", 7, WARN)
    s(d, 324, 282, "the front panel board)", 7, WARN)

    # cable B -> front panel
    wire(d, [244, 157, 302, 157, 302, 200, 316, 200])
    s(d, 266, 161, "pin 2", 7, SIG)
    box(d, 316, 136, 196, 112, "Front panel (bezel) board", [
        "J1: 10-pin, pin 2 = the clock line",
        "W1, W2: jumpers. DEC: 'in' = the AUX",
        "  ON/OFF switch controls LTC, and",
        "  OFF disables it",
        "S1-S3: switch contact holes. Your",
        "  panel has no AUX switch fitted",
        "Pass-through lines: SRUN (pin 3) for",
        "  RUN, BHALT (pin 9) from HALT"])
    tp(d, 302, 178, 5)
    s(d, 316, 120, "J5 and J6 are identical: either one may go to", 7, GREY)
    s(d, 316, 111, "the backplane and the other to the front panel.", 7, GREY)

    # legend
    y = 92
    s(d, 6, y, "Test points", 9, INK, bold=True)
    for i, t in enumerate([
            "AC sample into the monitor board (fingers AB and AD, or J3-B and J3-D)",
            "Q1 base (comparator output through R21)",
            "Q1 collector = monitor finger AA = J3-A = J5 / J6 pin 2",
            "BR1 on the backplane (BEVNT L), or 10-pin header pin 2",
            "front panel J1 pin 2"]):
        tp(d, 14, y - 13 - i * 15 + 3, i + 1)
        s(d, 28, y - 13 - i * 15, t, 7.5, INK)
    s(d, 400, y, "AC sample", 7, GREY)
    wire(d, [370, y + 3, 394, y + 3], AC)
    s(d, 400, y - 12, "clock line", 7, GREY)
    wire(d, [370, y - 9, 394, y - 9])
    return d


# --- Document ------------------------------------------------------------
story = []
story += [p("The BA11-N line clock", H1),
          p("How the H786 power supply's LTC signal reaches BEVNT on the "
            "H9273 backplane, and where to measure it", SUB)]
story += [p("The 11/23+ (KDF11-B) gets its 60 Hz clock interrupt from bus "
            "line <b>BEVNT L</b> (backplane pin BR1). The board's clock "
            "register at 177546 passes it on: each high-to-low edge is one "
            "tick. In the BA11-N the edges come from the H786 power supply. "
            "Measured so far: with the backplane unplugged the supply's "
            "output switches at 60 Hz; with the backplane plugged in (no "
            "cards) BR1 sits at a steady 5 V, so the 11/23+ sees no edges "
            "and 11nix gets no ticks."),
          p("Figure 1 is drawn from DEC's H786 drawings: the master board "
            "(54-12530) and the power monitor board (54-12528). DEC "
            "documents the front panel board only in outline (BA11-N "
            "guide, Figure 2-3 and Table 2-2), so its inside is not drawn."),
          Spacer(1, 4),
          KeepTogether([fig_path(), p(
              "Figure 1. The clock path. Q1 on the monitor board is the "
              "only thing in the supply that drives the line, and it can "
              "only pull it low.", CAP)])]

story += [PageBreak(), p("The 10-pin headers J5 / J6 and the backplane", H2),
          table([
              ["Pin", "H786 J5 / J6", "From (inside the H786)",
               "Backplane 10-pin"],
              ["1", "PWR OK", "monitor finger L (AC L)", "BPOK H (BB1)"],
              ["<b>2</b>", "<b>RTC</b>", "<b>monitor finger A: Q1 "
               "collector</b>", "<b>BEVNT L (BR1), through W1</b>"],
              ["3", "pass-through J5 to J6", "none", "SRUN L (AF1): RUN "
               "light"],
              ["4", "pass-through", "none", "key (no pin)"],
              ["5", "pass-through", "none", "GND"],
              ["6", "GND", "monitor finger M", ""],
              ["7", "pass-through", "none", ""],
              ["8", "SYNC", "master board J4-L", ""],
              ["9", "pass-through", "none", "BHALT L (AP1): HALT "
               "switch"],
              ["10", "DC OK", "monitor finger N (DC L)", "BDCOK H (BA1): "
               "RESTART"],
          ], [0.45 * inch, 1.35 * inch, 2.2 * inch, 2.7 * inch]),
          Spacer(1, 4),
          p("H786 side from the master board drawing (Figure 3). Backplane "
            "side from the Avitech QBUS front panel project (H9270 "
            "numbering, which they list for the H9273 as well); pins 6 to 8 "
            "are not documented there. Buzz out your own cable before "
            "relying on any pin but 2.", SMALL)]

story += [p("What each test point should show", H2),
          table([
              ["TP", "Where", "Working (scope, 60 Hz)", "If not"],
              ["1", "Monitor fingers AB and AD (J3-B, J3-D)",
               "AC, a few volts", "T1, its wiring or J3"],
              ["2", "Q1 base", "square, about 0 V and 0.7 V",
               "Comparator E1, R21, R48 or the +12 V supply"],
              ["3", "Q1 collector (finger AA, J3-A, J5/J6 pin 2), "
               "backplane plugged in", "square, about 0.2 V low and "
               "3 to 5 V high", "Q1, finger A or the J3 contact: TP2 "
               "switching but TP3 flat means Q1 does not pull down"],
              ["4", "BR1 or 10-pin pin 2", "same as TP3",
               "TP3 good, TP4 flat: the cable, the header crimp or W1"],
              ["5", "Front panel J1 pin 2", "same as TP3",
               "see the front panel test below"],
          ], [0.35 * inch, 2.0 * inch, 1.9 * inch, 2.45 * inch]),
          Spacer(1, 4),
          p("With nothing connected the line has no pull-up, so readings "
            "on an unplugged cable only show that Q1 switches, not real "
            "levels. For bench tests clip about 2.2 kΩ from pin 2 to "
            "+5 V.", SMALL)]

story += [p("Finding the fault, step by step", H2)]
story += steps([
    "<b>Front panel first.</b> Power off, unplug the front panel's 10-pin "
    "cable (at the panel, or at whichever of J5/J6 it uses). Power on, "
    "backplane connected, 11/23+ installed. Scope BR1. <b>60 Hz now:</b> "
    "the front panel was holding the line. Look at its W1/W2 jumpers and "
    "the empty AUX switch position (S1 to S3 holes). If BR1 floats with "
    "the panel unplugged, the panel was the pull-up: clip 2.2 kΩ to "
    "+5 V for this test.",
    "<b>Still flat:</b> scope TP3 (Q1 collector). Switching at TP3 but "
    "not at TP4: the path between them (cable, crimp, W1). Flat at TP3: "
    "scope TP2. Switching at TP2 but not TP3: Q1 or the finger contact. "
    "Flat at TP2: TP1, the comparator and its supply.",
    "<b>When BR1 switches:</b> boot 11nix with the QBone's KW11 off and "
    "run <font face='Courier'>date; sleep 5; date</font>. It must show 5 "
    "seconds.",
])

story += [PageBreak(), p("DEC's drawings", H2),
          KeepTogether([Image(os.path.join(HERE, "h786-monitor-ltc.png"),
                              width=6.8 * inch, height=6.8 * inch * 440 / 1200),
                        p("Figure 2. H786 power monitor board 54-12528, the "
                          "LTC part (MP00652 page 310). AB1/AB2 and AD1/AD2: "
                          "the AC sample; AA1/AA2: the RTC output.", CAP)]),
          KeepTogether([Image(os.path.join(HERE, "h786-master-j5j6.png"),
                              width=4.9 * inch, height=4.9 * inch * 1000 / 1250),
                        p("Figure 3. H786 master board 54-12530 (MP00652 page "
                          "296): T1 and J3 (top), and the two 10-pin headers "
                          "J6 and J5 wired in parallel to J3 (bottom).",
                          CAP)])]
story += [p("Sources", H2)]
story += bullets([
    "DEC MNC11 field maintenance print set MP00652 (Jul 1978), H786 "
    "drawings: master board 54-12530 (page 296), power monitor board "
    "54-12528 (page 310). bitsavers.org/pdf/dec/pdp11/minc/",
    "DEC BA11-N Mounting Box User's Guide (EK-BA11N-UG-001): backplane "
    "jumper W1 (Table 2-1), front panel board jumpers W1 to W4 (Figure 2-3, "
    "Table 2-2), front panel switches (Table 2-5).",
    "Avitech QBUS Front Panel Project: the 10-pin backplane header "
    "(avitech.com.au/?page_id=1657).",
    "Computer History Wiki: H786 power supply (gunkies.org).",
], SMALL)


def footer(c, doc):
    c.saveState()
    c.setFont("Helvetica", 7.5)
    c.setFillColor(GREY)
    c.drawString(0.75 * inch, 0.5 * inch, "BA11-N line clock — N9TAX RetroLAB")
    c.drawRightString(letter[0] - 0.75 * inch, 0.5 * inch, "page %d" % doc.page)
    c.restoreState()


doc = SimpleDocTemplate(OUT, pagesize=letter, leftMargin=0.75 * inch,
                        rightMargin=0.75 * inch, topMargin=0.7 * inch,
                        bottomMargin=0.75 * inch, title="The BA11-N line clock",
                        author="N9TAX RetroLAB")
doc.build(story, onFirstPage=footer, onLaterPages=footer)
print(OUT)
