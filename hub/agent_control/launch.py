#!/usr/bin/env python3
"""Opens a new Claude Code session on this computer's desktop, as a click on its launcher would.

    python3 launch.py <desktop id>        e.g. claude-terminal (for claude-terminal.desktop)

The launcher is a .desktop entry whose command opens a terminal running `claude`.
The caller here is outside the desktop session (the hub, usually over SSH), so that
command is started as a transient service of the user's systemd manager, not as a
child: it gets the desktop session's environment (the display, the session bus) and
none of the caller's (SSH_CONNECTION and the rest), and it outlives the connection.

Exit 0 once the terminal is running; otherwise 1 with one line on stderr,
`agent-control: <why>`, which the hub passes on to the display.

Standard library only, in one file: the hub runs it directly on its own host and
pipes it to `python3 -` over SSH on another, so nothing has to be installed there.
"""
import os
import re
import secrets
import shlex
import socket
import subprocess
import sys
import time

PREFIX = "agent-control: "
DESKTOP_ID = re.compile(r"\A[A-Za-z0-9._-]+\Z")
FIELD_CODE = re.compile(r"\A%[A-Za-z]\Z")  # %u, %F ...: where a launcher would put files; there are none
COMMAND_TIMEOUT_SECONDS = 5
SETTLE_SECONDS = 1.0  # a terminal that cannot open its window is gone again well within this


class Failure(Exception):
    """Something the person at the display should be told."""


def desktop_file(desktop_id):
    """The entry's path, searched as the desktop itself does: the user's own directory first."""
    data_home = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    data_dirs = (os.environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share").split(":")
    for directory in [data_home, *data_dirs]:
        path = os.path.join(directory, "applications", desktop_id + ".desktop")
        if os.path.isfile(path):
            return path
    return None


def command_of(path):
    """The entry's Exec line as an argument list, without its field codes; None when it has none."""
    section = ""
    with open(path, encoding="utf-8", errors="replace") as entry:
        for line in entry:
            line = line.strip()
            if line.startswith("["):
                section = line
            elif section == "[Desktop Entry]" and line.startswith("Exec="):
                argv = [arg.replace("%%", "%") for arg in shlex.split(line[len("Exec="):]) if not FIELD_CODE.match(arg)]
                return argv or None
    return None


def systemd(*argv):
    """One call to the user's systemd manager; None when it cannot be made at all."""
    env = dict(os.environ)
    env.setdefault("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")  # where the manager listens; unset under cron or env -i
    try:
        return subprocess.run(argv, capture_output=True, text=True, timeout=COMMAND_TIMEOUT_SECONDS, env=env,
                              stdin=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired):
        return None


def launch(desktop_id):
    """Starts the entry's command in the desktop session; returns the transient unit's name."""
    host = socket.gethostname().split(".")[0]
    if not DESKTOP_ID.match(desktop_id):
        raise Failure("That is not a launcher's name")
    path = desktop_file(desktop_id)
    argv = command_of(path) if path else None
    if not argv:
        raise Failure(f"There is no Claude launcher on {host}")
    session = systemd("systemctl", "--user", "is-active", "graphical-session.target")
    if session is None or session.stdout.strip() != "active":
        raise Failure(f"Nobody is logged in at {host}'s desktop")
    unit = f"claude-terminal-{secrets.token_hex(4)}.service"
    # Type=exec: systemd-run fails if the command cannot be executed. KillMode=process: closing the
    # terminal ends the terminal alone, as it does for one opened from the desktop.
    started = systemd("systemd-run", "--user", "--collect", "--quiet", "--unit", unit,
                      "--description", f"Claude Code terminal ({desktop_id}), opened from the agent-control display",
                      "-p", "Type=exec", "-p", "KillMode=process", "--", *argv)
    if started is None or started.returncode != 0:
        why = (started.stderr.strip().splitlines() or [""])[-1] if started else ""
        raise Failure(f"The terminal could not be started on {host}" + (f" — {why}" if why else ""))
    time.sleep(SETTLE_SECONDS)
    state = systemd("systemctl", "--user", "is-active", unit)
    if state is None or state.stdout.strip() != "active":
        raise Failure(f"The terminal did not stay open on {host}")
    return unit


def main(argv=None):
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1:
        print(f"{PREFIX}usage: launch.py <desktop id>", file=sys.stderr)
        return 2
    try:
        print(launch(args[0]))
    except Failure as failure:
        print(f"{PREFIX}{failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
