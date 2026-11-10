#!/usr/bin/env python3
# test_net11.py - boot net11 in emu11 and exercise it: console commands, HTTP, telnet.
#   test_net11.py [emu11] [net11.lda]
import os, socket, subprocess, sys, threading, time, urllib.request

here = os.path.dirname(os.path.abspath(__file__))
emu = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "emu11", "emu11")
lda = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "src", "net11.lda")
HTTP, TELNET = 18080, 12323

out = []
p = subprocess.Popen([emu, "-t", "90", "-f", "%d:80" % HTTP, "-f", "%d:23" % TELNET, lda],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

def reader():
    for b in iter(lambda: p.stdout.read(1), b""):
        out.append(b)
threading.Thread(target=reader, daemon=True).start()

def console():
    return b"".join(out).decode("latin-1")

def wait_for(text, timeout=30):
    t = time.time() + timeout
    while time.time() < t:
        if text in console():
            return True
        time.sleep(0.1)
    return False

def typ(s):
    for ch in s:
        p.stdin.write(ch.encode())
        p.stdin.flush()
        time.sleep(0.005)

results = []
def check(name, ok, detail=""):
    results.append((name, ok))
    print("%-34s %s %s" % (name, "ok" if ok else "FAILED", detail))

check("boot banner", wait_for("Type help"))
check("DHCP address", wait_for("DHCP: address", 20))
check("NTP time", wait_for("Time: ", 40))

typ("ping 10.0.2.2 3\r")
check("ping gateway", wait_for("3 received", 15))
typ("host example.com\r")
check("DNS lookup", wait_for("example.com is", 15))
typ("date\r")
check("date", wait_for("UTC\r\npdp11>", 5) or wait_for(" UTC", 5))

# HTTP
try:
    t0 = time.time()
    body = urllib.request.urlopen("http://127.0.0.1:%d/" % HTTP, timeout=20).read().decode("latin-1")
    check("HTTP /", "PDP-11 ONLINE" in body and "</html>" in body,
          "%d bytes in %.2f s" % (len(body), time.time() - t0))
except Exception as e:
    check("HTTP /", False, str(e))
try:
    body = urllib.request.urlopen("http://127.0.0.1:%d/stats" % HTTP, timeout=20).read().decode()
    check("HTTP /stats", "Uptime" in body)
except Exception as e:
    check("HTTP /stats", False, str(e))
try:
    urllib.request.urlopen("http://127.0.0.1:%d/nope" % HTTP, timeout=20)
    check("HTTP 404", False)
except urllib.error.HTTPError as e:
    check("HTTP 404", e.code == 404)
except Exception as e:
    check("HTTP 404", False, str(e))

# several requests in a row
ok = 0
for i in range(10):
    try:
        if b"ONLINE" in urllib.request.urlopen("http://127.0.0.1:%d/" % HTTP, timeout=20).read():
            ok += 1
    except Exception:
        pass
check("HTTP 10 requests", ok == 10, "%d/10" % ok)

# telnet
try:
    s = socket.create_connection(("127.0.0.1", TELNET), timeout=10)
    buf = b""
    def recv_until(tok, t=10):
        global buf
        end = time.time() + t
        while tok not in buf and time.time() < end:
            try:
                d = s.recv(4096)
                if not d:
                    break
                buf += d
            except socket.timeout:
                break
        return tok in buf
    check("telnet banner", recv_until(b"pdp11> "))
    buf = b""
    s.sendall(b"sys\r\n")
    check("telnet sys", recv_until(b"TCP ") and recv_until(b"pdp11> "))
    buf = b""
    s.sendall(b"help\r\n")
    check("telnet help", recv_until(b"quit") and recv_until(b"pdp11> "))
    buf = b""
    s.sendall(b"mem 1000 20\r\n")
    check("telnet mem", recv_until(b"001000:") and recv_until(b"pdp11> "))
    buf = b""
    s.sendall(b"quit\r\n")
    check("telnet quit", recv_until(b"bye"))
    s.close()
except Exception as e:
    check("telnet", False, str(e))

typ("tcp\r")
time.sleep(1)
typ("sys\r")
wait_for("web requests", 5)
p.terminate()
p.wait()
print("---- console ----")
print(console()[-2500:])
print("passed %d of %d" % (sum(1 for _, ok in results if ok), len(results)))
sys.exit(0 if all(ok for _, ok in results) else 1)
