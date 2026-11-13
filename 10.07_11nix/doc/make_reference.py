#!/usr/bin/env python3
"""make_reference.py - build 11nix-reference.pdf, the 11nix command reference.

    python3 make_reference.py [output.pdf]

Needs reportlab (pip install reportlab).
"""
import sys
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import letter
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import inch
from reportlab.platypus import (BaseDocTemplate, Frame, KeepTogether, PageTemplate, Paragraph,
                                Spacer, Table, TableStyle, CondPageBreak)

OUT = sys.argv[1] if len(sys.argv) > 1 else "11nix-reference.pdf"

INK = colors.HexColor("#1d1d1f")
MUTED = colors.HexColor("#5b5b60")
ACCENT = colors.HexColor("#8a3b12")      # DEC-ish rust
RULE = colors.HexColor("#d9d4cc")
SHADE = colors.HexColor("#f6f3ee")
NOTE_BG = colors.HexColor("#fbf1e6")

body = ParagraphStyle("body", fontName="Helvetica", fontSize=8.6, leading=11.2, textColor=INK)
small = ParagraphStyle("small", parent=body, fontSize=7.8, leading=10, textColor=MUTED)
cmd = ParagraphStyle("cmd", parent=body, fontName="Courier-Bold", fontSize=8.4, leading=11)
h1 = ParagraphStyle("h1", fontName="Helvetica-Bold", fontSize=20, leading=24, textColor=INK)
sub = ParagraphStyle("sub", parent=body, fontSize=9.5, leading=13, textColor=MUTED)
h2 = ParagraphStyle("h2", fontName="Helvetica-Bold", fontSize=11, leading=14, textColor=ACCENT,
                    spaceBefore=9, spaceAfter=4)
code = ParagraphStyle("code", fontName="Courier", fontSize=8.3, leading=10.6, textColor=INK,
                      alignment=TA_LEFT)


def c(text):
    """Inline code inside a description."""
    return '<font face="Courier-Bold">%s</font>' % escape(text)


def P(text, style=body):
    return Paragraph(text, style)


def section(title):
    return [CondPageBreak(1.1 * inch), P(escape(title), h2)]


def commands(rows, col=1.75 * inch):
    """rows: (command, description) - the command is escaped, the description is markup."""
    data = [[P(escape(a), cmd), P(b)] for a, b in rows]
    t = Table(data, colWidths=[col, None], hAlign="LEFT")
    style = [
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 4),
        ("RIGHTPADDING", (0, 0), (-1, -1), 4),
        ("TOPPADDING", (0, 0), (-1, -1), 2.2),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 2.2),
        ("LINEBELOW", (0, 0), (-1, -2), 0.4, RULE),
    ]
    for i in range(0, len(rows), 2):
        style.append(("BACKGROUND", (0, i), (-1, i), SHADE))
    t.setStyle(TableStyle(style))
    return t


def block(lines):
    """A code block: what you type, exactly."""
    text = "<br/>".join(escape(l).replace(" ", "&nbsp;") for l in lines)
    t = Table([[P(text, code)]], colWidths=[None], hAlign="LEFT")
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), SHADE),
        ("LINEBEFORE", (0, 0), (0, -1), 2, ACCENT),
        ("LEFTPADDING", (0, 0), (-1, -1), 8),
        ("TOPPADDING", (0, 0), (-1, -1), 5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
    ]))
    return t


def note(text):
    t = Table([[P(text)]], colWidths=[None], hAlign="LEFT")
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), NOTE_BG),
        ("LEFTPADDING", (0, 0), (-1, -1), 8),
        ("RIGHTPADDING", (0, 0), (-1, -1), 8),
        ("TOPPADDING", (0, 0), (-1, -1), 5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
    ]))
    return t


def gap(h=5):
    return Spacer(1, h)


story = []
story += [
    P("11nix Command Reference", h1),
    gap(2),
    P("11nix 0.1 on the PDP-11/23 with a QBone &nbsp;&middot;&nbsp; DEC MMU, DEQNA/DELQA and QBone "
      "port (c) 2026 N9TAX RetroLAB &nbsp;&middot;&nbsp; based on Fuzix", sub),
    gap(8),
]

# --- Starting and stopping ---------------------------------------------------
story += section("Starting and stopping")
story += [commands([
    ("START, then 10000g", "Boot: press START in the QBone web panel. At the ODT prompt "
     + c("@") + " on the console type " + c("10000g") + ". 11nix boots to a " + c("#")
     + " prompt: you are root."),
    ("halt", "<b>Stop the machine properly.</b> Writes everything to the disk, marks it clean, and "
     "halts the processor. " + c("reboot") + " does the same."),
    ("START, 10000g again", "Boot again after a halt. Always START first: the kernel overwrites "
     "the boot loader, so " + c("10000g") + " alone does not work."),
    ("sync", "Write buffered data to the disk now. Not a shutdown: the disk stays marked in use."),
]), gap(6), note(
    "<b>Read-only disk?</b> If the machine stopped any other way (HALT switch, power, crash), the "
    "next boot prints " + c("WARNING: the disk is READ-ONLY") + ". Nothing can be saved, and the "
    "network does not start. Repair it: " + c("fsck /dev/hd0") + ", answer " + c("y")
    + " to every question (inodes \"not of correct type\" are old network sockets: zap them). "
    "fsck halts the machine when it is done: START, " + c("10000g") + ".")]

# --- Network setup -----------------------------------------------------------
story += section("Network setup (once)")
story += [
    P("There is no DHCP: the address is in " + c("/etc/netrc") + ", the DNS server in "
      + c("/etc/resolv.conf") + ". Example for a LAN 10.1.1.x with the router at 10.1.1.1:"),
    gap(4),
    block([
        "# cat > /etc/netrc",
        "ipaddr  10.1.1.80",
        "gateway 10.1.1.1",
        "netmask 255.255.255.0",
        "                              <- press Ctrl-D here",
        "# echo nameserver 10.1.1.15 > /etc/resolv.conf",
        "# halt                        <- then START, 10000g",
    ]),
    gap(4),
    P("At boot it prints " + c("network: inet 10.1.1.80 ...") + ". From another computer: "
      + c("telnet 10.1.1.80") + " (log in as " + c("root") + "), or " + c("http://10.1.1.80/")
      + " in a browser. The BeagleBone itself cannot reach the PDP-11 over the DELQA; every other "
      "machine can.", body),
]

# --- Keys and the shell ------------------------------------------------------
story += section("Keys at the prompt")
story += [commands([
    ("Ctrl-C", "Interrupt the running program."),
    ("Ctrl-D", "End of input (finishes " + c("cat > file") + ")."),
    ("Ctrl-H", "Erase a character. Ctrl-U erases the whole line."),
    ("Ctrl-S / Ctrl-Q", "Pause / resume output. Ctrl-O throws output away until pressed again."),
    ("Backspace prints ^?", "Your terminal sends DEL: type " + c("stty erase '^?'") + ". To keep "
     "it, put it in " + c("/.profile") + ": " + c("echo \"stty erase '^?'\" > /.profile")),
])]

story += section("The shell (V7 Bourne shell)")
story += [commands([
    ("cmd > file", "Output to a file (" + c(">>") + " appends). " + c("< file") + " reads from a file."),
    ("cmd1 | cmd2", "Pipe. Careful: " + c("^") + " is also a pipe in this shell, so quote it: "
     + c("'^?'") + "."),
    ("cmd &", "Run in the background; " + c("ps") + " shows it, " + c("kill PID") + " stops it."),
    ("cmd1 ; cmd2", "One after the other."),
    ("* ? [abc]", "Wildcards: " + c("ls /bin/c*") + "."),
    ("`cmd`", "Insert the output of a command: " + c("echo today is `date`") + "."),
    ("'text'  \"text\"", "Quotes. Inside " + c("\"...\"") + ", " + c("$VAR") + " is still replaced."),
    ("VAR=value", "Set a variable; " + c("export VAR") + " passes it to programs. " + c("$PATH")
     + " is " + c("/bin") + "."),
    ("sh file", "Run a shell script. " + c("if/then/else/fi") + ", " + c("for x in ...; do ...; done")
     + ", " + c("while") + " work as in any Bourne shell."),
    ("cd dir, pwd", "Change / show the directory. There is no " + c("~") + ", history or tab completion."),
    ("exit", "Leave the shell (logs out a telnet session)."),
])]

# --- Files -------------------------------------------------------------------
story += section("Files and directories")
story += [commands([
    ("ls -l [dir]", "List files, long form (" + c("ls -a") + " shows hidden ones)."),
    ("cat file", "Show a file. " + c("more file") + " shows it a page at a time."),
    ("head / tail file", "First / last 10 lines (" + c("head -20 file") + ", " + c("tail -n 20 file") + ")."),
    ("cp a b, mv a b", "Copy / move or rename."),
    ("rm file", "Delete. " + c("rmdir dir") + " deletes an empty directory."),
    ("mkdir dir", "Make a directory."),
    ("ln a b", "A second name for a file (" + c("vi") + " is a link to " + c("levee") + ")."),
    ("touch file", "Create an empty file, or update its time."),
    ("chmod 755 file", "Permissions. " + c("chown") + " / " + c("chgrp") + " change the owner / group."),
    ("du [dir], df", "Disk used by files / free space on the disk."),
    ("od -c file", "Dump a file byte by byte. " + c("dd") + " copies raw blocks."),
    ("tar -cvf x.tar dir", "Archive a directory; " + c("tar -tvf x.tar") + " lists, " + c("tar -xvf x.tar") + " unpacks."),
    ("cmp a b", "Compare two files. " + c("sum") + " / " + c("cksum") + " give checksums."),
    ("rx file, sx file", "XMODEM receive / send over the console (from your terminal program)."),
])]

story += section("Text")
story += [commands([
    ("grep word file", "Lines containing a pattern (" + c("-v") + " the others, " + c("-i")
     + " ignore case). " + c("fgrep") + " for plain text."),
    ("sed s/old/new/ file", "Replace text in a stream."),
    ("sort, uniq", "Sort lines; drop repeated lines."),
    ("wc file", "Count lines, words, characters."),
    ("tr a-z A-Z", "Translate characters."),
    ("cut -c1-10 file", "Columns of each line (" + c("-d: -f1") + " for fields)."),
    ("tee file", "Copy a pipe into a file too."),
    ("xargs cmd", "Run a command with the words read from input."),
    ("echo text", "Print text. " + c("banner HI") + " prints it big."),
])]

# --- System ------------------------------------------------------------------
story += section("System")
story += [commands([
    ("ps", "Running processes. " + c("kill PID") + ", " + c("kill -9 PID") + ", " + c("killall name") + "."),
    ("free", "Memory in KB: 176 KB for programs, 14 processes at most, the rest swapped to disk."),
    ("uptime, uname -a", "Time since boot; system name and version."),
    ("date", "The time (UTC). It is set from the internet at boot; " + c("setdate")
     + " sets it by hand."),
    ("mount", "Mounted disks. " + c("/dev/hd0") + " is the RL02."),
    ("fsck /dev/hd0", "Check and repair the disk (see Starting and stopping)."),
    ("stty", "Terminal settings (" + c("stty erase '^?'") + ")."),
    ("passwd", "Set root's password (none at first: anyone on the LAN can telnet in)."),
    ("su, id, who, whoami", "Change user; who you are; who is logged in."),
    ("which cmd", "Where a program is."),
])]

# --- Network -----------------------------------------------------------------
story += section("Network")
story += [commands([
    ("ifconfig", "Address, netmask, gateway and the Ethernet address of the DELQA."),
    ("ping host", "Ping a name or address until Ctrl-C."),
    ("dig name [@server]", "Look up a name in DNS."),
    ("telnet host [port]", "Connect to another machine (" + c("-a") + " passes ANSI codes through)."),
    ("htget http://host/path file", "Fetch a web page into a file (http only, no https)."),
    ("ntpdate -s server", "Set the clock (UTC). " + c("-o -6") + " adds an offset in hours, "
     "for example " + c("ntpdate -s -o -6 pool.ntp.org") + " for US Central standard time."),
    ("tinyirc server:port [nick]", "A small IRC client."),
    ("echoping addr [port] [msg]", "Send a UDP echo."),
]), gap(5), P(
    "Started at boot by " + c("/etc/rc") + ": " + c("netd") + " (TCP/IP, reads " + c("/etc/netrc")
    + "), " + c("telnetd") + " (port 23), " + c("httpd") + " (port 80, serves " + c("/www")
    + "), " + c("ntpdate") + ". Edit " + c("/etc/rc") + " to change what starts, for example the "
    "time zone offset of ntpdate.")]

# --- Editors -----------------------------------------------------------------
story += section("ed, the line editor (works on any terminal)")
story += [
    P("Type a command and Enter. Lines are numbered from 1; " + c("$") + " is the last line."),
    gap(3),
    commands([
        ("ed file", "Start editing a file."),
        ("1,$p", "Print all lines. " + c("3p") + " prints line 3."),
        ("/text/", "Go to the next line containing text."),
        ("3c   or  /text/c", "Replace that line: type the new text, then " + c(".") + " alone on a line."),
        ("$a", "Add lines after the last one; finish with " + c(".") + " alone. " + c("3i")
         + " inserts before line 3."),
        ("3d", "Delete line 3. " + c("2,4d") + " deletes lines 2 to 4."),
        ("s/old/new/", "Replace text in the current line (" + c("1,$s/old/new/g") + ": everywhere)."),
        ("w", "Save."),
        ("q", "Quit (asks \"Really quit?\" when the changes are not saved)."),
    ], col=1.5 * inch),
]

story += section("vi (levee), the screen editor (VT100 / ANSI terminal)")
story += [
    P(c("vi file") + " (or " + c("levee file") + "). It starts in <b>command mode</b>: keys are "
      "commands. " + c("i") + ", " + c("a") + " or " + c("o") + " switch to typing; <b>Esc</b> "
      "switches back. It edits files up to about 4 KB."),
    gap(3),
    commands([
        ("arrows  or  h j k l", "Move left, down, up, right. " + c("0") + " / " + c("$")
         + " start / end of line, " + c("G") + " last line, " + c("1G") + " first."),
        ("i  a  A  o", "Type before the cursor / after it / at the end of the line / on a new "
         "line below. Then Esc."),
        ("x", "Delete the character under the cursor."),
        ("dd", "Delete the line. " + c("3dd") + " deletes 3 lines."),
        ("cw", "Change a word: type the new one, then Esc."),
        ("r c", "Replace one character with c."),
        ("yy  p", "Copy the line; put it below."),
        ("u", "Undo."),
        ("/text", "Search; " + c("n") + " finds the next one."),
        (":wq   or  ZZ", "Save and quit."),
        (":w   :q!", "Save / quit without saving."),
    ], col=1.5 * inch),
]

# --- Files of note -----------------------------------------------------------
story += section("Files to know")
story += [commands([
    ("/etc/netrc", "This machine's IP address, gateway, netmask."),
    ("/etc/resolv.conf", "The DNS server: " + c("nameserver 10.1.1.15") + "."),
    ("/etc/rc", "Run at boot: starts the network, telnetd, httpd, ntpdate."),
    ("/.profile", "Run when the console shell starts (your " + c("stty") + " settings)."),
    ("/www/index.html", "The home page of the web server."),
    ("/usr/lib/basic/*.bas", "Examples for " + c("ubasic /usr/lib/basic/demo.bas") + "."),
])]

story += section("Fun")
story += [commands([
    ("/usr/games/advent", "Colossal Cave Adventure. " + c("quit") + " to stop."),
    ("ubasic file.bas", "Integer BASIC."),
    ("cal 11 2026", "A calendar. " + c("factor 1170") + " prime factors, " + c("seq 1 10")
     + " numbers."),
])]


def frame_page(canvas, doc):
    canvas.saveState()
    w, h = letter
    canvas.setStrokeColor(ACCENT)
    canvas.setLineWidth(2)
    canvas.line(doc.leftMargin, h - 0.45 * inch, w - doc.rightMargin, h - 0.45 * inch)
    canvas.setFont("Helvetica", 7.5)
    canvas.setFillColor(MUTED)
    canvas.drawString(doc.leftMargin, 0.4 * inch, "11nix command reference  ·  N9TAX RetroLAB")
    canvas.drawRightString(w - doc.rightMargin, 0.4 * inch, "page %d" % doc.page)
    canvas.restoreState()


doc = BaseDocTemplate(OUT, pagesize=letter, leftMargin=0.65 * inch, rightMargin=0.65 * inch,
                      topMargin=0.65 * inch, bottomMargin=0.65 * inch,
                      title="11nix Command Reference", author="N9TAX RetroLAB",
                      subject="11nix 0.1 for the PDP-11/23 with a QBone")
frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="f",
              leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
doc.addPageTemplates([PageTemplate(id="p", frames=[frame], onPage=frame_page)])
doc.build(story)
print(OUT)
