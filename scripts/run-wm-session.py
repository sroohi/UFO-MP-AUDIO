#!/usr/bin/env python3
"""Own and reap the isolated Linux test desktop's WM and private D-Bus tree."""
import ctypes
import os
import signal
import subprocess
import time

# D-Bus services may daemonize. Adopt those descendants so a container's PID 1
# does not inherit unreaped service processes after the test desktop closes.
libc = ctypes.CDLL(None, use_errno=True)
if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
    raise OSError(ctypes.get_errno(), "Cannot supervise the test desktop's children")
os.setsid()
stopping = False

def stop(_signum, _frame):
    global stopping
    stopping = True

for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
    signal.signal(sig, stop)

main = subprocess.Popen(["dbus-run-session", "--", "xfwm4", "--replace", "--compositor=off"])
main_status = None
shutdown_started = None
while True:
    if stopping and shutdown_started is None:
        shutdown_started = time.monotonic()
        os.killpg(os.getpgrp(), signal.SIGTERM)
    children_remain = True
    while True:
        try:
            pid, status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            children_remain = False
            break
        if pid == 0:
            break
        if pid == main.pid:
            main_status = os.waitstatus_to_exitcode(status)
            stopping = True
    if not children_remain:
        break
    if shutdown_started is not None and time.monotonic() - shutdown_started > 2:
        # Escalate only within this supervisor's isolated process group.
        members = subprocess.check_output(["ps", "-eo", "pid=,pgid="], text=True)
        for member in members.splitlines():
            pid, group = map(int, member.split())
            if group == os.getpgrp() and pid != os.getpid():
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
    time.sleep(0.02)
raise SystemExit(0 if main_status in (0, -signal.SIGTERM) else (main_status or 1))
