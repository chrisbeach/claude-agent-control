"""agent-control-window, without a desktop: the extension's answers are stood in for.

    python -m pytest desktop/tests
"""
import importlib.machinery
import importlib.util
import os
import select
import subprocess
import sys
from pathlib import Path

import pytest

HELPER = Path(__file__).resolve().parents[1] / "agent-control-window"


def load():
    loader = importlib.machinery.SourceFileLoader("agent_control_window", str(HELPER))
    module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
    loader.exec_module(module)
    return module


@pytest.fixture
def acw(monkeypatch):
    module = load()
    monkeypatch.setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent")
    return module


class Extension:
    """Stands in for the extension: records calls, answers from `answer(method, args)`."""

    def __init__(self, answer):
        self.answer, self.calls = answer, []

    def __call__(self, method, *args):
        self.calls.append((method, *args))
        return self.answer(method, args)


# ---- arguments and the error contract ---------------------------------------

def test_gvariant_string_quotes_what_gdbus_would_parse(acw):
    assert acw.gvariant_string("IT'S A \\ TEST") == "'IT\\'S A \\\\ TEST'"
    assert acw.gvariant_string("A\nB\x1b") == "'A B '"


def test_highlight_is_taken_from_anywhere_among_the_arguments(acw):
    assert acw.take_highlight(["123", "--highlight", "done", "FIX CI"]) == (("done", "FIX CI"), ["123"])
    assert acw.take_highlight(["123"]) == (None, ["123"])
    with pytest.raises(acw.Failure):
        acw.take_highlight(["123", "--highlight", "purple", "X"])
    with pytest.raises(acw.Failure):
        acw.take_highlight(["123", "--highlight", "done"])


@pytest.mark.parametrize("pid", ["0", "1", "-5", "12a", "²", "", "99999999999"])
def test_raise_refuses_what_is_not_a_pid(acw, pid):
    with pytest.raises(acw.Failure, match="Usage"):
        acw.cmd_raise([pid])


def test_a_failure_is_one_stderr_line_the_hub_can_show(acw, monkeypatch, capsys):
    monkeypatch.setattr(acw.Path, "exists", lambda self: False)
    assert acw.main(["agent-control-window", "raise", "4242"]) == 1
    err = capsys.readouterr().err
    assert err == "agent-control: That session is no longer running here\n"


def test_a_session_without_a_terminal_says_so(acw, monkeypatch):
    monkeypatch.setattr(acw, "pty_of", lambda pid: None)
    with pytest.raises(acw.Failure, match="no terminal window"):
        acw.cmd_raise([str(os.getpid())])


# ---- talking to the extension ------------------------------------------------

def fake_gdbus(tmp_path, script):
    path = tmp_path / "gdbus"
    path.write_text("#!/bin/sh\n" + script)
    path.chmod(0o755)
    return str(path)


def test_call_returns_the_extensions_answer(acw, monkeypatch, tmp_path):
    monkeypatch.setattr(acw, "gdbus", lambda: fake_gdbus(tmp_path, "echo \"('raised',)\""))
    assert acw.call("FocusWindowByPid", "77") == "raised"


@pytest.mark.parametrize("stderr, reason", [
    ("Error: GDBus.Error:org.freedesktop.DBus.Error.ServiceUnknown: The name is not provided", "not running"),
    ("Error: GDBus.Error:org.freedesktop.DBus.Error.UnknownMethod: No such method", "Log out and back in"),
    ("Error: something else", "Could not reach the desktop"),
])
def test_call_says_why_the_extension_could_not_be_asked(acw, monkeypatch, tmp_path, stderr, reason):
    monkeypatch.setattr(acw, "gdbus", lambda: fake_gdbus(tmp_path, f"echo '{stderr}' >&2; exit 1"))
    with pytest.raises(acw.Failure, match=reason):
        acw.call("FocusWindowByPid", "77")


def test_without_a_session_bus_nobody_is_logged_in(acw, monkeypatch):
    monkeypatch.delenv("DBUS_SESSION_BUS_ADDRESS")
    with pytest.raises(acw.Failure, match="Nobody is logged in"):
        acw.call("FocusWindowByPid", "77")


def test_highlight_is_best_effort(acw, monkeypatch):
    def refuse(method, *args):
        raise acw.Failure("Log out and back in to finish setting up")
    monkeypatch.setattr(acw, "call", refuse)
    acw.highlight(("done", "X"))  # no exception


# ---- finding the window ------------------------------------------------------

def test_raise_asks_by_the_nearest_parent_that_owns_a_window_then_marks_it(acw, monkeypatch):
    monkeypatch.setattr(acw, "pty_of", lambda pid: "/dev/pts/9")
    monkeypatch.setattr(acw, "ancestors", lambda pid: [300, 200, 100])
    ext = Extension(lambda method, args: "raised" if args == ("200",) else
                    "highlighted" if method == "Highlight" else "notfound")
    monkeypatch.setattr(acw, "call", ext)
    assert acw.cmd_raise([str(os.getpid()), "--highlight", "waiting", "UPLOAD PROGRESS"]) == 0
    assert ext.calls == [("FocusWindowByPid", "300"), ("FocusWindowByPid", "200"),
                         ("Highlight", "'waiting'", "'UPLOAD PROGRESS'")]


def test_raise_falls_back_to_the_title_when_a_parent_owns_several_windows(acw, monkeypatch):
    monkeypatch.setattr(acw, "pty_of", lambda pid: "/dev/pts/9")
    monkeypatch.setattr(acw, "ancestors", lambda pid: [300])
    monkeypatch.setattr(acw, "call", Extension(lambda method, args: "ambiguous"))
    monkeypatch.setattr(acw, "by_title", lambda pty: "raised")
    assert acw.cmd_raise([str(os.getpid())]) == 0


@pytest.mark.parametrize("verdict, reason", [("refused", "kept the focus"), ("ambiguous", "singled out"),
                                              ("notfound", "singled out")])
def test_raise_says_why_no_window_was_raised(acw, monkeypatch, verdict, reason):
    monkeypatch.setattr(acw, "pty_of", lambda pid: "/dev/pts/9")
    monkeypatch.setattr(acw, "by_process", lambda pid: "notfound")
    monkeypatch.setattr(acw, "by_title", lambda pty: verdict)
    with pytest.raises(acw.Failure, match=reason):
        acw.cmd_raise([str(os.getpid())])


def test_the_title_marker_is_pushed_shown_and_popped(acw, monkeypatch):
    master, slave = os.openpty()
    pty = os.ttyname(slave)
    asked = []

    def answer(method, args):
        asked.append(args[0])
        return "raised"
    monkeypatch.setattr(acw, "call", Extension(answer))
    assert acw.by_title(pty) == "raised"
    written = b""
    while select.select([master], [], [], 0.2)[0]:  # the pty can hand the writes over in pieces
        written += os.read(master, 4096)
    os.close(master), os.close(slave)
    marker = asked[0].strip("'")
    assert marker.startswith("agent-control ")
    assert written == b"\x1b[22;0t\x1b]2;" + marker.encode() + b"\x07\x1b[23;0t"


def test_ancestors_of_this_process_lead_up_to_the_test_runner(acw):
    assert acw.ancestors(os.getpid())[0] == os.getppid()


def test_pty_of_reads_the_standard_streams(acw):
    master, slave = os.openpty()
    proc = subprocess.Popen(["sleep", "5"], stdin=slave, stdout=slave, stderr=slave)
    try:
        assert acw.pty_of(proc.pid) == os.ttyname(slave)
    finally:
        proc.kill(), proc.wait(), os.close(master), os.close(slave)


# ---- new ---------------------------------------------------------------------

def test_new_raises_and_marks_the_terminal_it_opened_then_waits_for_it(acw, monkeypatch, tmp_path):
    monkeypatch.setenv("CLAUDE_BIN", "/bin/true")
    started = []
    real_popen = subprocess.Popen

    def popen(argv, **kwargs):
        started.append(argv)
        proc = real_popen(argv, **kwargs)
        started.append(proc.pid)
        return proc
    monkeypatch.setattr(acw.subprocess, "Popen", popen)
    ext = Extension(lambda method, args: "highlighted" if method == "Highlight" else
                    "raised" if args == (str(started[1]),) else "notfound")
    monkeypatch.setattr(acw, "call", ext)
    status = acw.cmd_new(["--highlight", "idle", "NEW SESSION", "--terminal", "/bin/sh -c 'sleep 0.3' --",
                          str(tmp_path)])
    assert status == 0
    assert started[0][:4] == ["/bin/sh", "-c", "sleep 0.3", "--"]
    assert started[0][-3:] == ["sh", str(tmp_path), "/bin/true"]
    assert ("Highlight", "'idle'", "'NEW SESSION'") in ext.calls


def test_new_runs_claude_in_the_directory_and_keeps_its_exit_status(acw, monkeypatch, tmp_path):
    out = tmp_path / "ran"
    claude = tmp_path / "claude"
    claude.write_text(f"#!/bin/sh\npwd > {out}\nexit 0\n")
    claude.chmod(0o755)
    monkeypatch.setenv("CLAUDE_BIN", str(claude))
    monkeypatch.setattr(acw, "call", Extension(lambda method, args: "notfound"))
    status = acw.cmd_new(["--terminal", "env", str(tmp_path)])  # `env cmd...` stands in for a terminal
    assert status == 0
    assert out.read_text().strip() == str(tmp_path)


def test_new_without_claude_says_so(acw, monkeypatch, tmp_path):
    monkeypatch.delenv("CLAUDE_BIN", raising=False)
    monkeypatch.setattr(acw.shutil, "which", lambda name: None)
    monkeypatch.setattr(acw, "CLAUDE_CANDIDATES", ())
    with pytest.raises(acw.Failure, match="claude CLI was not found"):
        acw.cmd_new([str(tmp_path)])


def test_the_helper_runs_as_a_program(tmp_path):
    done = subprocess.run([sys.executable, str(HELPER)], capture_output=True, text=True)
    assert done.returncode == 2 and "raise <pid>" in done.stderr
