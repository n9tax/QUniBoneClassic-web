# updater.py - run qbweb-update.sh from the web panel.
#
# The update script copies new files, recompiles demo, and at the end restarts
# this daemon. So it must not be a child of the daemon: under systemd it runs
# as a transient unit (systemd-run), outside the qbcontrol service, and
# survives the restart. Its state lives in files, so a restarted daemon still
# knows how the update went:
#
#   ~/.qbcontrol/update/log    output of the script
#   ~/.qbcontrol/update/pid    pid of the shell running it
#   ~/.qbcontrol/update/exit   its exit code, written when it is done
#   ~/.qbcontrol/update/info   {"mode": ..., "started": ...}

import json
import os
import pwd
import re
import shlex
import shutil
import subprocess
import time

from . import core

SCRIPT_NAME = "qbweb-update.sh"
LOG_MAX = 96 * 1024         # tail of the log sent to the page


def _dir():
    return os.path.join(core.DATA_DIR, "update")


def _path(name):
    return os.path.join(_dir(), name)


def script_path():
    return os.path.join(core.QUNIBONE_HOME, SCRIPT_NAME)


def _read(name, default=""):
    try:
        with open(_path(name)) as f:
            return f.read()
    except (IOError, OSError):
        return default


def _source():
    """Repository and branch the script updates from (its defaults)."""
    repo = branch = ""
    try:
        with open(script_path()) as f:
            text = f.read(4096)
        m = re.search(r"local REPO=(\S+)", text)
        repo = m.group(1) if m else ""
        m = re.search(r"local BRANCH=(\S+)", text)
        branch = m.group(1) if m else ""
    except (IOError, OSError):
        pass
    return repo, branch


def _alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    # pids are reused: check it is still our shell
    try:
        with open("/proc/%d/cmdline" % pid, "rb") as f:
            return SCRIPT_NAME.encode() in f.read()
    except (IOError, OSError):
        return True


def running():
    if _read("exit").strip():
        return False
    try:
        pid = int(_read("pid").strip())
    except ValueError:
        return False
    return _alive(pid)


def status():
    info = {}
    try:
        info = json.loads(_read("info", "{}"))
    except ValueError:
        pass
    log = _read("log")
    if len(log) > LOG_MAX:
        log = "...\n" + log[-LOG_MAX:].split("\n", 1)[-1]
    code = _read("exit").strip()
    run = running()
    repo, branch = _source()
    st = {
        "available": os.path.isfile(script_path()),
        "script": script_path(),
        "repo": repo, "branch": branch,
        "running": run,
        "mode": info.get("mode"),
        "started": info.get("started"),
        "finished": info.get("started") is not None and not run,
        "exit_code": int(code) if code.lstrip("-").isdigit() else None,
        "log": log,
    }
    if st["finished"] and st["exit_code"] is None:
        st["exit_code"] = -1        # the shell vanished without writing its exit code
    return st


def start(mode):
    """mode "check": only list what would change (-n); "update": do it (-y)."""
    if mode not in ("check", "update"):
        raise core.QbcError("mode must be check or update")
    script = script_path()
    if not os.path.isfile(script):
        raise core.QbcError("%s is missing. Install it once, see the README." % script)
    if running():
        raise core.QbcError("An update is already running")
    d = _dir()
    if not os.path.isdir(d):
        os.makedirs(d)
    for n in ("exit", "pid"):
        try:
            os.remove(_path(n))
        except OSError:
            pass
    with open(_path("log"), "w") as f:
        f.write("")
    with open(_path("info"), "w") as f:
        json.dump({"mode": mode, "started": time.time()}, f)

    q = shlex.quote
    home = pwd.getpwuid(os.getuid()).pw_dir
    inner = ("echo $$ > {pid}; export HOME={home}; cd {top} && "
             "bash ./{name} {flag} > {log} 2>&1 < /dev/null; echo $? > {exit}").format(
        pid=q(_path("pid")), home=q(home), top=q(core.QUNIBONE_HOME), name=SCRIPT_NAME,
        flag="-n" if mode == "check" else "-y", log=q(_path("log")), exit=q(_path("exit")))
    argv = ["/bin/bash", "-c", inner]
    if os.geteuid() == 0 and shutil.which("systemd-run") and os.path.isdir("/run/systemd/system"):
        # outside the qbcontrol service: survives "systemctl restart qbcontrol"
        argv = ["systemd-run", "--unit", "qbweb-update-%d" % int(time.time()),
                "--description", "qbcontrol: " + SCRIPT_NAME] + argv
        p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             universal_newlines=True)
        out = p.communicate(timeout=30)[0]
        rc = p.returncode
        if rc != 0:
            raise core.QbcError("systemd-run failed: " + out.strip())
    else:
        subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True, close_fds=True)
    # wait a moment for the pid file, so a status right after shows "running"
    end = time.time() + 3
    while time.time() < end and not _read("pid").strip():
        time.sleep(0.05)
    return status()
