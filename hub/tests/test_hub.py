import asyncio
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import time

import httpx

from agent_control import hub as hub_module
from agent_control.hub import Hub, create_app

KEY = "test-key"
AUTH = {"Authorization": f"Bearer {KEY}"}
REPORT = {"host": "devbox", "at": 1, "home": "/home/alice", "titles": True, "error": None, "sessions": [
    {"pid": 11, "cwd": "/home/alice/dev", "kind": "interactive", "startedAt": 1795047600000, "sessionId": "aaaaaaaa-1", "name": "dev-1f",
     "status": "busy", "title": "Agent control"}]}


def run(scenario):
    """Runs `scenario(hub, client)` against an app wired to a hub with no probes."""
    async def go():
        hub = Hub(["devbox", "buildbox"], KEY)
        app = create_app(hub)
        app.state.hub = hub  # ASGITransport does not run the lifespan
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://hub") as client:
            return await scenario(hub, client)
    return asyncio.run(go())


def test_the_fleet_needs_the_device_key():
    async def scenario(hub, client):
        assert (await client.get("/api/fleet")).status_code == 401
        assert (await client.get("/api/fleet", headers={"Authorization": "Bearer wrong"})).status_code == 401
        return (await client.get("/api/fleet", headers=AUTH)).json()
    body = run(scenario)
    assert body["sessions"] == [] and [h["up"] for h in body["hosts"]] == [False, False]
    assert abs(body["now"] - time.time()) < 5 and body["rev"]


def test_a_probe_line_shows_up():
    async def scenario(hub, client):
        hub.ingest(hub.hosts[0], REPORT)
        return (await client.get("/api/fleet", headers=AUTH)).json()
    body = run(scenario)
    assert body["sessions"][0]["title"] == "AGENT CONTROL" and body["counts"]["working"] == 1
    assert body["hosts"][0] == {"name": "DEVBOX", "up": True, "sessions": 1, "waiting": 0}


def test_long_poll_answers_at_once_on_a_change_and_holds_otherwise():
    async def scenario(hub, client):
        hub.ingest(hub.hosts[0], REPORT)
        rev = hub.rev

        started = time.monotonic()
        held = (await client.get("/api/fleet", params={"rev": rev, "wait": 0.4}, headers=AUTH)).json()
        held_for = time.monotonic() - started

        async def change():
            await asyncio.sleep(0.1)
            hub.ingest(hub.hosts[0], dict(REPORT, sessions=[dict(REPORT["sessions"][0], status="waiting", waitingFor="permission prompt")]))
        started = time.monotonic()
        changer = asyncio.create_task(change())
        woken = (await client.get("/api/fleet", params={"rev": rev, "wait": 20}, headers=AUTH)).json()
        woken_after = time.monotonic() - started
        await changer

        stale = (await client.get("/api/fleet", params={"rev": "not-the-rev", "wait": 20}, headers=AUTH)).json()
        return rev, held, held_for, woken, woken_after, stale
    rev, held, held_for, woken, woken_after, stale = run(scenario)
    assert held["rev"] == rev and 0.35 < held_for < 2
    assert woken["rev"] != rev and woken_after < 2 and woken["sessions"][0]["detail"] == "PERMISSION PROMPT"
    assert stale["rev"] == woken["rev"]  # an unknown rev is answered straight away


def test_a_probe_error_does_not_look_like_an_empty_host():
    async def scenario(hub, client):
        hub.ingest(hub.hosts[0], REPORT)
        hub.ingest(hub.hosts[0], {"host": "devbox", "error": "claude CLI not found", "sessions": []})
        return (await client.get("/api/fleet", headers=AUTH)).json(), (await client.get("/healthz")).json()
    body, health = run(scenario)
    assert body["hosts"][0]["sessions"] == 1  # the last good list stands until it goes stale
    assert health["hosts"]["devbox"]["error"] == "claude CLI not found"


def test_metrics_count_states_per_host():
    async def scenario(hub, client):
        hub.ingest(hub.hosts[0], REPORT)
        return (await client.get("/metrics")).text
    text = run(scenario)
    assert 'agent_control_host_up{probed_host="devbox"} 1' in text and 'agent_control_host_up{probed_host="buildbox"} 0' in text
    assert 'agent_control_sessions{probed_host="devbox",state="working"} 1' in text
    assert "agent_control_heartbeat_timestamp_seconds 0" in text  # no sweep has run in this test


def test_a_background_loop_that_dies_is_counted_and_started_again(monkeypatch):
    monkeypatch.setattr(hub_module, "RESTART_MIN_SECONDS", 0.01)
    monkeypatch.setattr(hub_module, "SWEEP_SECONDS", 0.01)

    async def scenario():
        hub = Hub(["devbox"], KEY, prometheus="http://prometheus.invalid")
        runs = []

        async def flaky():  # dies twice, then stays up
            runs.append(len(runs))
            if len(runs) <= 2:
                raise RuntimeError("boom")
            await asyncio.sleep(3600)

        async def parked():
            await asyncio.sleep(3600)

        monkeypatch.setattr(hub, "watch", lambda host: flaky())
        monkeypatch.setattr(hub, "poll_usage", parked)
        tasks = hub.start()
        await asyncio.sleep(0.3)
        text = hub.metrics()
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        return hub, len(runs), text
    hub, runs, text = asyncio.run(scenario())
    assert runs == 3 and hub.task_restarts == {"probe:devbox": 2, "sweep": 0, "usage": 0}
    assert 'agent_control_task_restarts_total{task="probe:devbox"} 2' in text and 'agent_control_task_restarts_total{task="sweep"} 0' in text
    assert abs(hub.heartbeat - time.time()) < 5  # the sweep kept going round meanwhile
    assert "agent_control_usage_last_fetch_timestamp_seconds 0" in text


def fake_focus(tmp_path, script):
    """A stand-in focus command; returns the AGENT_CONTROL_FOCUS_COMMAND that runs it."""
    tool = tmp_path / "focus"
    tool.write_text("#!/bin/sh\n" + script)
    tool.chmod(0o755)
    return f"{tool} --session {{id}} --pid {{pid}}"


SID = "11111111-2222-4333-8444-555555555555"
SWITCHABLE = dict(REPORT, sessions=[dict(REPORT["sessions"][0], sessionId=SID)])


def focus(command, report=SWITCHABLE, key="devbox:11", headers=AUTH):
    async def scenario(hub, client):
        hub.local_name = "devbox"  # run the command here, not over ssh
        hub.focus_command = command
        hub.ingest(hub.hosts[0], report)
        return await client.post("/api/focus", params={"key": key}, headers=headers)
    return run(scenario)


def test_a_tap_runs_the_focus_command_for_that_session(tmp_path):
    command = fake_focus(tmp_path, f'echo "$@" > "{tmp_path}/called"\n')
    assert focus(command).json() == {"ok": True, "message": "WINDOW RAISED ON DEVBOX"}
    assert (tmp_path / "called").read_text().split() == ["--session", SID, "--pid", "11"]


def test_a_refusal_is_passed_on_in_the_commands_own_words(tmp_path):
    command = fake_focus(tmp_path, 'echo "some noise" >&2\necho "agent-control: Log out and back in once \u2014 the extension loads at login." >&2\nexit 1\n')
    assert focus(command).json() == {"ok": False, "message": "LOG OUT AND BACK IN ONCE"}
    command = fake_focus(tmp_path, 'echo "some noise" >&2\nexit 1\n')  # nothing said in the agreed form: the hub's own words
    assert focus(command).json() == {"ok": False, "message": "COULD NOT RAISE THE WINDOW ON DEVBOX"}


def test_focus_says_when_it_is_not_set_up_the_command_is_missing_or_the_session_is_gone(tmp_path):
    assert focus("").json() == {"ok": False, "message": "WINDOW SWITCHING IS NOT SET UP ON THE HUB"}
    assert focus(f"{tmp_path}/missing {{id}}").json() == {"ok": False, "message": "THE FOCUS COMMAND IS NOT INSTALLED ON DEVBOX"}
    command = fake_focus(tmp_path, "exit 0\n")
    assert focus(command, key="devbox:999").json() == {"ok": False, "message": "THAT SESSION HAS ENDED"}
    assert focus(command, headers={}).status_code == 401


def test_only_a_session_id_and_a_pid_reach_the_shell(tmp_path):
    command = fake_focus(tmp_path, f'echo "$@" > "{tmp_path}/called"\n')
    hostile = dict(REPORT, sessions=[dict(REPORT["sessions"][0], sessionId="x; touch /tmp/owned")])
    assert focus(command, report=hostile).json()["ok"] is False
    hostile = dict(REPORT, sessions=[dict(SWITCHABLE["sessions"][0], pid="11; touch /tmp/owned")])
    assert focus(command, report=hostile, key="devbox:11; touch /tmp/owned"[:31]).json()["ok"] is False
    assert not (tmp_path / "called").exists()


def test_a_tap_can_hand_the_rows_state_and_title_to_the_focus_command(tmp_path):
    tool = tmp_path / "focus"
    tool.write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" > "{tmp_path}/called"\n')
    tool.chmod(0o755)
    assert focus(f"{tool} --highlight {{state}} {{title}} {{id}}").json()["ok"] is True
    assert (tmp_path / "called").read_text().splitlines() == ["--highlight", "working", "AGENT CONTROL", SID]


def test_a_title_reaches_the_focus_command_as_one_word_and_is_never_run(tmp_path, monkeypatch):
    tool = tmp_path / "focus"
    tool.write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" > "{tmp_path}/called"\n')
    tool.chmod(0o755)
    title = "it's $(touch owned) `touch owned2` {id} \\ \"q\""
    hostile = dict(SWITCHABLE, sessions=[dict(SWITCHABLE["sessions"][0], title=title)])
    monkeypatch.chdir(tmp_path)  # where a command substitution would leave its file
    assert focus(f"{tool} {{title}}", report=hostile).json()["ok"] is True
    assert (tmp_path / "called").read_text().splitlines() == [title.upper()]  # {id} in a title stays as it is
    assert not (tmp_path / "owned").exists() and not (tmp_path / "owned2").exists()


def test_usage_windows_can_be_chosen_and_relabelled():
    assert hub_module.usage_labels_from(None) is None
    assert hub_module.usage_labels_from("seven_day=WK, seven_day_opus = OPUS1 ,junk,=X") == {"seven_day": "WK", "seven_day_opus": "OPUS"}


LAUNCHER = "claude-terminal"


def new_session(host="devbox", launcher=LAUNCHER, headers=AUTH):
    async def scenario(hub, client):
        hub.local_name = "devbox"  # run the launch here, not over ssh
        hub.new_session_host, hub.new_session_launcher = host, launcher
        return await client.post("/api/new", headers=headers)
    return run(scenario)


def fake_python(tmp_path, monkeypatch, script):
    """Puts a stand-in `python3` first on PATH: what the hub hands launch.py to."""
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir(exist_ok=True)
    tool = bin_dir / "python3"
    tool.write_text("#!/bin/sh\n" + script)
    tool.chmod(0o755)
    monkeypatch.setenv("PATH", f"{bin_dir}:/usr/bin:/bin")


def test_the_plus_runs_the_launcher_script_on_the_desktop(tmp_path, monkeypatch):
    fake_python(tmp_path, monkeypatch, f'echo "$@" > "{tmp_path}/args"\ncat > "{tmp_path}/program"\n')
    assert new_session().json() == {"ok": True, "message": "NEW SESSION OPENED ON DEVBOX"}
    assert (tmp_path / "args").read_text().split() == ["-", LAUNCHER]
    assert (tmp_path / "program").read_bytes() == hub_module.LAUNCH_PATH.read_bytes()  # the script arrives on stdin


def test_a_launch_that_fails_is_passed_on_in_the_scripts_own_words(tmp_path, monkeypatch):
    fake_python(tmp_path, monkeypatch, 'echo "agent-control: Nobody is logged in at devbox\'s desktop" >&2\nexit 1\n')
    assert new_session().json() == {"ok": False, "message": "NOBODY IS LOGGED IN AT DEVBOX'S DESKTOP"}
    fake_python(tmp_path, monkeypatch, "exit 1\n")  # nothing said: the hub's own words
    assert new_session().json() == {"ok": False, "message": "COULD NOT OPEN A SESSION ON DEVBOX"}


def test_the_plus_needs_the_device_key_and_a_desktop_to_open_on(tmp_path, monkeypatch):
    fake_python(tmp_path, monkeypatch, f'touch "{tmp_path}/called"\n')
    assert new_session(headers={}).status_code == 401
    assert new_session(host="").json() == {"ok": False, "message": "NO DESKTOP IS SET UP FOR NEW SESSIONS"}
    assert new_session(launcher="x; touch /tmp/owned").json()["ok"] is False  # only a launcher's name reaches the shell
    assert not (tmp_path / "called").exists()


def test_a_command_that_never_answers_is_given_up_on(tmp_path, monkeypatch):
    monkeypatch.setattr(hub_module, "COMMAND_TIMEOUT_SECONDS", 0.3)
    fake_python(tmp_path, monkeypatch, "exec sleep 30\n")
    started = time.monotonic()
    assert new_session().json() == {"ok": False, "message": "NO ANSWER FROM DEVBOX"}
    assert time.monotonic() - started < 5


def test_usage_ages_out(monkeypatch):
    hub = asyncio.run(_make_hub())
    hub.usage_values, hub.usage_at = {"seven_day": 33.0}, time.time()
    assert hub.usage(time.time()) == {"seven_day": 33.0}
    assert hub.usage(time.time() + hub_module.USAGE_MAX_AGE_SECONDS + 1) is None


async def _make_hub():
    return Hub(["devbox"], KEY)


def test_the_probe_reads_the_cli(tmp_path):
    """probe.py against a stand-in `claude` that prints what the real one printed on devbox."""
    fake = tmp_path / "claude"
    rows = [{"pid": 2453247, "cwd": "/home/alice/dev", "kind": "interactive", "startedAt": 1795017386642,
             "sessionId": "66666666-7777-4888-9999-aaaaaaaaaaaa", "name": "dev-71", "status": "idle"}]
    fake.write_text("#!/bin/sh\n[ \"$1 $2\" = 'agents --json' ] || exit 3\ncat <<'EOF'\n" + json.dumps(rows) + "\nEOF\n")
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    # an empty config directory: the Agent SDK finds no transcript there, as for a session nobody has used yet
    env = {**os.environ, "CLAUDE_BIN": str(fake), "CLAUDE_CONFIG_DIR": str(tmp_path / "config")}
    out = subprocess.run([sys.executable, str(hub_module.PROBE_PATH)], capture_output=True, text=True, env=env)
    report = json.loads(out.stdout)
    assert report["error"] is None and report["sessions"][0]["name"] == "dev-71" and report["home"]
    assert report["titles"] and report["sessions"][0]["used"] is False and "title" not in report["sessions"][0]

    broken = subprocess.run([sys.executable, str(hub_module.PROBE_PATH)], capture_output=True, text=True, env={**os.environ, "CLAUDE_BIN": "/bin/false"})
    assert "exited 1" in json.loads(broken.stdout)["error"]


def test_a_stopped_session_gets_its_to_do_line_and_the_hubs_own_runs_are_not_shown(tmp_path):
    async def go():
        hub = Hub(["devbox"], KEY, claude="unused", todo_dir=str(tmp_path))
        hub.local_name = "devbox"
        asked = []

        async def ask(prompt):
            asked.append(prompt)
            return "Pick mockup A, B or C"
        hub.todos.ask = ask
        hub.todos.own_pids[99] = 0  # a claude -p of the hub's own, as the probe lists it
        stopped = dict(REPORT["sessions"][0], status="idle", used=True, replyId="r1", reply="USER ASKED: mock it")
        own = {"pid": 99, "cwd": str(tmp_path), "kind": "interactive", "startedAt": 1, "sessionId": "bbbbbbbb-2", "name": "x-1"}
        hub.ingest(hub.hosts[0], dict(REPORT, sessions=[stopped, own]))
        first = hub.body["sessions"]
        await asyncio.sleep(0.05)
        return first, hub.body["sessions"], asked
    first, later, asked = asyncio.run(go())
    assert [r["name"] for r in first] == ["DEV-1F"]
    assert first[0]["todo"] == "SUMMARISING..." and later[0]["todo"] == "PICK MOCKUP A, B OR C"
    assert len(asked) == 1


def short_dir():
    """A temporary directory short enough to hold SSH control sockets (pytest's tmp_path may not be)."""
    return tempfile.TemporaryDirectory(prefix="ac", dir="/tmp")


def test_a_hosts_commands_go_down_its_probes_ssh_connection():
    with short_dir() as tmp:
        hub = Hub(["devbox"], KEY, control_dir=f"{tmp}/ssh")
        probe, command = hub.ssh("devbox", "python3 -", master=True), hub.ssh("devbox", "true")
        socket = f"ControlPath={tmp}/ssh/%C"
        assert probe[0] == "ssh" and probe[-2:] == ["devbox", "python3 -"]
        assert socket in probe and "ControlMaster=auto" in probe  # the probe's ssh becomes the master
        assert socket in command and "ControlMaster=no" in command  # a command uses it, never becomes one
        assert not any("ControlPersist" in arg for arg in probe + command)  # the master goes with the probe
        assert stat.S_IMODE(os.stat(f"{tmp}/ssh").st_mode) == 0o700


def test_no_multiplexing_where_the_socket_path_would_be_too_long_for_ssh():
    with short_dir() as tmp:
        fits = tmp + "/" + "d" * (hub_module.UNIX_SOCKET_PATH_MAX - len(tmp) - 2 - hub_module.CONTROL_NAME_CHARS)
        assert len(fits + "/" + "c" * hub_module.CONTROL_NAME_CHARS) == hub_module.UNIX_SOCKET_PATH_MAX
        assert hub_module.control_path(fits) == fits + "/%C"
        deep = pathlib.Path(fits + "d")  # one character more: ssh would refuse it
        assert hub_module.control_path(str(deep)) == ""
    hub = Hub(["devbox"], KEY, control_dir="")
    assert not any(arg.startswith("Control") for arg in hub.ssh("devbox", "true", master=True))
    assert not deep.exists()
