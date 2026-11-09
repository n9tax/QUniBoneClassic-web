# runner.py - owns the one "demo" process.
#
# demo runs on a pseudo terminal, so its output is line buffered and its
# stdin behaves as at a real console. All output goes into a ring buffer with
# absolute byte offsets; clients (web page, terminal menu) poll "since offset".
#
# Only one emulation can drive the bus, so there is exactly one runner.

import errno
import fcntl
import os
import signal
import subprocess
import termios
import threading
import time

from . import core

BUFFER_MAX = 512 * 1024


def _make_ctty():
    # Child, after setsid(): make the pty slave (fd 0) the controlling tty, so
    # demo gets SIGHUP if the daemon dies and the master side closes.
    try:
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    except (IOError, OSError):
        pass


class Runner(object):
    def __init__(self, log=None):
        self.log = log or (lambda msg: None)
        self.lock = threading.RLock()      # re-entrant: stop()/start() call status()
        self.cond = threading.Condition(self.lock)
        self.proc = None
        self.master = None
        self.state = "stopped"        # stopped | running | stopping
        self.spec = None
        self.started_at = None
        self.stopped_at = None
        self.exit_code = None
        self.buf = bytearray()
        self.base = 0                 # absolute offset of buf[0]
        self.session = 0              # increments on each start
        self.session_log = None

    # ---- console buffer -------------------------------------------------

    def _append(self, data):
        with self.cond:
            self.buf.extend(data)
            if len(self.buf) > BUFFER_MAX:
                cut = len(self.buf) - BUFFER_MAX
                del self.buf[:cut]
                self.base += cut
            self.cond.notify_all()
        if self.session_log:
            try:
                self.session_log.write(data)
                self.session_log.flush()
            except (IOError, OSError, ValueError):
                pass

    def note(self, text):
        """Panel message into the console stream, marked so the UI can style it."""
        self._append(("\r\n\x1b[7m[qbcontrol] " + text + "\x1b[0m\r\n").encode("utf-8"))

    def read(self, since, wait=0.0):
        """Return (data, next_offset, base). Blocks up to "wait" seconds for new output."""
        deadline = time.time() + wait
        with self.cond:
            while True:
                end = self.base + len(self.buf)
                if since is None or since > end:
                    since = end if since is not None else self.base
                if since < end or wait <= 0:
                    break
                remaining = deadline - time.time()
                if remaining <= 0:
                    break
                self.cond.wait(min(remaining, 1.0))
            start = max(since, self.base)
            data = bytes(self.buf[start - self.base:])
            return data, self.base + len(self.buf), self.base

    # ---- status ---------------------------------------------------------

    def status(self):
        with self.lock:
            st = {
                "state": self.state,
                "session": self.session,
                "pid": self.proc.pid if self.proc and self.state != "stopped" else None,
                "started_at": self.started_at,
                "stopped_at": self.stopped_at,
                "exit_code": self.exit_code,
                "console_end": self.base + len(self.buf),
            }
            if self.spec:
                st["target"] = self.spec.get("target")
                st["label"] = self.spec.get("label")
                st["cmdfile"] = self.spec.get("cmdfile")
                st["argv"] = self.spec.get("argv")
                st["cwd"] = self.spec.get("cwd")
            return st

    def running(self):
        return self.state != "stopped"

    # ---- control --------------------------------------------------------

    def start(self, spec):
        with self.lock:
            if self.state != "stopped":
                raise core.QbcError("An emulation is already running (\"%s\"). Stop it first."
                                    % (self.spec or {}).get("label", "?"))
            if not os.path.exists(spec["argv"][0]):
                raise core.QbcError("demo program not found: %s\n"
                                    "Compile it with ./compile.sh" % spec["argv"][0])
            master, slave = os.openpty()
            # CR from a raw terminal ends a line as fgets() expects. No ISIG:
            # a stray Ctrl-C from a console must not kill demo while it drives
            # the bus - stopping goes through "q" so PRU and drivers shut down.
            attrs = termios.tcgetattr(slave)
            attrs[0] |= termios.ICRNL
            attrs[3] &= ~termios.ISIG
            termios.tcsetattr(slave, termios.TCSANOW, attrs)
            env = dict(os.environ)
            env["HOME"] = core.QUNIBONE_HOME   # stock scripts and demo use "~"
            env["TERM"] = "vt100"
            try:
                proc = subprocess.Popen(spec["argv"], cwd=spec["cwd"], env=env,
                                        stdin=slave, stdout=slave, stderr=slave,
                                        close_fds=True, start_new_session=True,
                                        preexec_fn=_make_ctty)
            except OSError as e:
                os.close(master)
                os.close(slave)
                raise core.QbcError("Cannot start %s: %s" % (spec["argv"][0], e))
            os.close(slave)
            self.proc = proc
            self.master = master
            self.spec = spec
            self.state = "running"
            self.session += 1
            self.started_at = time.time()
            self.stopped_at = None
            self.exit_code = None
            try:
                self.session_log = open(core.SESSION_LOG, "wb")
            except (IOError, OSError):
                self.session_log = None
        self.note("START %s" % spec["label"])
        self.note("cwd %s" % spec["cwd"])
        self.note(" ".join(spec["argv"]))
        self.log("started %s pid %d: %s" % (spec["label"], proc.pid, " ".join(spec["argv"])))
        t = threading.Thread(target=self._reader, args=(proc, master), name="demo-reader")
        t.daemon = True
        t.start()
        return self.status()

    def _reader(self, proc, master):
        while True:
            try:
                data = os.read(master, 4096)
            except OSError as e:
                if e.errno == errno.EINTR:
                    continue
                data = b""       # EIO: slave side closed, process gone
            if not data:
                break
            self._append(data)
        code = proc.wait()
        try:
            os.close(master)
        except OSError:
            pass
        with self.lock:
            self.state = "stopped"
            self.exit_code = code
            self.stopped_at = time.time()
            self.master = None
        self.note("demo exited with code %s" % code)
        self.log("demo exited, code %s" % code)
        if self.session_log:
            try:
                self.session_log.close()
            except (IOError, OSError):
                pass
            self.session_log = None

    def send(self, text):
        with self.lock:
            if self.state == "stopped" or self.master is None:
                raise core.QbcError("Nothing running")
            fd = self.master
        data = text.encode("utf-8") if not isinstance(text, bytes) else text
        while data:
            n = os.write(fd, data)
            data = data[n:]

    def stop(self, force=False, wait=False):
        """Leave the demo menus with "q" (bus drivers and PRU are shut down
        properly), escalate to SIGTERM / SIGKILL if demo does not exit."""
        with self.lock:
            if self.state == "stopped":
                return self.status()
            if self.state == "stopping" and not force:
                return self.status()
            self.state = "stopping"
            proc = self.proc
        t = threading.Thread(target=self._stopper, args=(proc, force), name="demo-stopper")
        t.daemon = True
        t.start()
        if wait:
            t.join(30)
        return self.status()

    def _stopper(self, proc, force):
        def gone(timeout):
            end = time.time() + timeout
            while time.time() < end:
                if proc.poll() is not None:
                    return True
                time.sleep(0.1)
            return proc.poll() is not None

        if not force:
            self.note("STOP: leaving demo menus")
            # device menu "q" -> main menu "q" -> exit. Extra newline first in
            # case a half typed line is pending.
            for _ in range(3):
                try:
                    self.send("\nq\n")
                except (core.QbcError, OSError):
                    break
                if gone(2.5):
                    return
        for sig, name, timeout in ((signal.SIGTERM, "SIGTERM", 4), (signal.SIGKILL, "SIGKILL", 4)):
            if proc.poll() is not None:
                return
            self.note("STOP: sending %s" % name)
            try:
                os.killpg(proc.pid, sig)
            except OSError:
                pass
            if gone(timeout):
                return
