"""launch.py against stand-in `systemctl` and `systemd-run`: what it starts, and what it says when it cannot."""
import os
import subprocess
import sys

from agent_control import hub as hub_module

LAUNCHER = "claude-terminal"
ENTRY = """[Desktop Entry]
Type=Application
Name=Claude
Exec=ptyxis -s --gapplication-app-id=claude-terminal --title="Claude Code" -- /home/alice/bin/claude-terminal.sh %U
[Desktop Action other]
Exec=not-this-one
"""
# systemctl: `is-active graphical-session.target` answers $SESSION, `is-active <unit>` answers $UNIT_STATE.
SYSTEMCTL = """#!/bin/sh
[ "$1 $2" = "--user is-active" ] || exit 3
[ -n "$XDG_RUNTIME_DIR" ] || exit 4
case "$3" in graphical-session.target) echo "$SESSION";; *) echo "$UNIT_STATE";; esac
"""
# systemd-run: records its arguments one per line, or fails as the real one does.
SYSTEMD_RUN = """#!/bin/sh
[ -z "$RUN_FAILS" ] || { echo "Failed to start transient service unit: $RUN_FAILS" >&2; exit 1; }
printf '%s\\n' "$@" > "$RECORD"
"""


def launch(tmp_path, launcher=LAUNCHER, entry=ENTRY, **env):
    apps, bin_dir = tmp_path / "share" / "applications", tmp_path / "bin"
    apps.mkdir(parents=True, exist_ok=True)
    bin_dir.mkdir(exist_ok=True)
    if entry:
        (apps / f"{LAUNCHER}.desktop").write_text(entry)
    for name, script in (("systemctl", SYSTEMCTL), ("systemd-run", SYSTEMD_RUN)):
        (bin_dir / name).write_text(script)
        (bin_dir / name).chmod(0o755)
    settings = {"PATH": f"{bin_dir}:/usr/bin:/bin", "HOME": str(tmp_path), "XDG_DATA_HOME": str(tmp_path / "share"),
                "XDG_DATA_DIRS": str(tmp_path / "nowhere"), "RECORD": str(tmp_path / "ran"), "SESSION": "active", "UNIT_STATE": "active", **env}
    # as the hub runs it: the script on stdin. No XDG_RUNTIME_DIR, as under a bare environment.
    with hub_module.LAUNCH_PATH.open("rb") as script:
        return subprocess.run([sys.executable, "-", launcher], stdin=script, capture_output=True, text=True, env=settings)


def test_it_starts_the_launchers_command_as_a_user_service(tmp_path):
    done = launch(tmp_path)
    assert done.returncode == 0 and done.stderr == ""
    ran = (tmp_path / "ran").read_text().splitlines()
    unit = done.stdout.strip()
    assert unit.startswith("claude-terminal-") and unit.endswith(".service") and unit in ran
    assert ran[:2] == ["--user", "--collect"] and "Type=exec" in ran and "KillMode=process" in ran
    # the Exec line, quoting honoured and the %U field code dropped; a later section's Exec is not the launcher's
    assert ran[ran.index("--") + 1:] == ["ptyxis", "-s", "--gapplication-app-id=claude-terminal", "--title=Claude Code", "--",
                                         "/home/alice/bin/claude-terminal.sh"]


def test_it_says_why_in_one_line_when_it_cannot(tmp_path):
    host = os.uname().nodename.split(".")[0]
    cases = [
        (dict(entry=""), f"There is no Claude launcher on {host}"),
        (dict(entry="[Desktop Entry]\nName=No command\n"), f"There is no Claude launcher on {host}"),
        (dict(launcher="../../etc/passwd"), "That is not a launcher's name"),
        (dict(SESSION="inactive"), f"Nobody is logged in at {host}'s desktop"),
        (dict(RUN_FAILS="Unit name is not valid."), f"The terminal could not be started on {host} — Failed to start transient service unit: Unit name is not valid."),
        (dict(UNIT_STATE="inactive"), f"The terminal did not stay open on {host}"),
    ]
    for number, (settings, said) in enumerate(cases):
        done = launch(tmp_path / str(number), **settings)
        assert (done.returncode, done.stdout, done.stderr) == (1, "", f"agent-control: {said}\n"), settings
        if "SESSION" in settings or "entry" in settings or "launcher" in settings:
            assert not (tmp_path / str(number) / "ran").exists()  # nothing was started
