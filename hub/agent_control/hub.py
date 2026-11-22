"""The agent-control hub: one small service that knows every live Claude Code session
on the configured hosts and serves them to the display.

It keeps one probe (probe.py) running per host: a child process on its own host,
`ssh <host> python3 -` on the others. Each probe prints a JSON line every few
seconds; the hub folds the lines into the display's view (fleet.py) and answers

    GET /api/fleet[?rev=<rev>&wait=<seconds>]   bearer = the device key; long poll
    POST /api/focus?key=<row key>               bearer = the device key; raise that session's window
    POST /api/new                               bearer = the device key; open a new session on the desktop
    GET /healthz                                 per-host probe state
    GET /metrics                                 Prometheus text

Run: uvicorn --factory agent_control.hub:create_app --host 0.0.0.0 --port 8767
"""
import asyncio
import contextlib
import hmac
import json
import logging
import os
import pathlib
import re
import socket
import sys
import time
import urllib.parse
import urllib.request

from fastapi import FastAPI, Header, HTTPException
from fastapi.responses import PlainTextResponse

from . import fleet, todo
from .probe import find_claude

log = logging.getLogger("agent_control")

PROBE_PATH = pathlib.Path(__file__).with_name("probe.py")
MAX_WAIT_SECONDS = 50          # under any proxy's read timeout; the display asks for 25
SWEEP_SECONDS = 2              # how often a host going quiet is noticed
RESTART_MIN_SECONDS, RESTART_MAX_SECONDS = 2, 30
USAGE_POLL_SECONDS = 60
USAGE_MAX_AGE_SECONDS = 1800   # the exporter refreshes every 5 min; older than this is not shown
# The probe's interpreter on another host: a venv holding the Agent SDK (titles) when bootstrap-host.sh made one.
REMOTE_COMMAND = 'p="$HOME/.local/share/agent-control/venv/bin/python"; [ -x "$p" ] || p=python3; exec "$p" -u - --interval {interval}'
# A tap on the display raises the session's terminal window on its own desktop, by running the configured focus
# command (AGENT_CONTROL_FOCUS_COMMAND) on the session's host with {id} (the session id) and {pid} (its process)
# filled in. How a window can be raised depends on the desktop, so the hub ships no command of its own.
SESSION_ID = re.compile(r"\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\Z")
# The display's "+" opens a new session on one desktop: launch.py, piped to that host's python like the probe,
# starts a desktop launcher (a .desktop entry whose command opens a terminal running `claude`) there.
LAUNCH_PATH = pathlib.Path(__file__).with_name("launch.py")
LAUNCH_COMMAND = "python3 - {launcher}"
LAUNCHER_ID = re.compile(r"\A[A-Za-z0-9._-]+\Z")
# A focus or launch command that fails may explain why in one stderr line starting with this; the display prints it.
MESSAGE_PREFIX = "agent-control: "
COMMAND_TIMEOUT_SECONDS = 10   # a switch or a launch that has not answered by now is given up on
MESSAGE_CHARS = 96             # the firmware's buffer for the line it shows
SSH_OPTIONS = ("-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2")
# A host's probe connection doubles as its SSH master (ControlMaster), so a tap's focus or launch command goes down
# it instead of opening a connection of its own: no handshake, about 0.2 s sooner on the desk. No ControlPersist: the
# master is the probe's own ssh and goes when it does; while a probe is down, a command connects on its own.
CONTROL_DIR = "~/.cache/agent-control/ssh"
UNIX_SOCKET_PATH_MAX = 103   # sun_path is 104 bytes on macOS (108 on Linux), with the NUL; ssh refuses a longer ControlPath
CONTROL_NAME_CHARS = 40      # ssh's %C: a SHA-1, in hex


def control_path(directory):
    """Where the hub's SSH masters listen, "<directory>/%C"; "" (every command connects on its own) when there is no
    directory, or a socket in it would be too long for ssh, which would then refuse to connect at all."""
    if not directory:
        return ""
    path = pathlib.Path(directory).expanduser()
    if len(str(path)) + 1 + CONTROL_NAME_CHARS > UNIX_SOCKET_PATH_MAX:
        log.warning("%s is too long a path for SSH control sockets; every command connects on its own", path)
        return ""
    return str(path / "%C")


class Hub:
    def __init__(self, host_names, device_key, interval=3.0, prometheus="", new_session_host="", new_session_launcher="",
                 claude="", todo_dir="~/.cache/agent-control/todo", focus_command="", usage_labels=None,
                 control_dir=CONTROL_DIR):
        if not device_key:
            raise ValueError("a device key is required")
        self.hosts = [fleet.HostSnapshot(name) for name in host_names]
        self.device_key = device_key
        self.interval = interval
        self.prometheus = prometheus.rstrip("/")
        self.usage_labels = fleet.USAGE_LABELS if usage_labels is None else usage_labels
        self.new_session_host, self.new_session_launcher = new_session_host, new_session_launcher
        self.focus_command = focus_command
        self.control_path = control_path(control_dir)
        self.tracker = fleet.Tracker()
        # The to-do line of a stopped session, made with this host's `claude -p`; no binary named = no lines.
        self.todos = todo.Todos(claude, todo_dir, self.publish) if claude else None
        self.local_name = socket.gethostname().split(".")[0]
        self.usage_values, self.usage_at = None, 0.0
        self.body, self.rev = None, ""
        self.changed = asyncio.Event()
        self.last_display_poll = 0.0
        self.heartbeat = 0.0        # when the sweep loop last went round
        self.usage_fetched_at = 0.0  # when Prometheus last answered the usage query
        self.task_restarts = {}      # background loop -> times it has died and been started again
        self.publish()

    # ---- state ----

    def usage(self, now):
        return self.usage_values if self.usage_values and now - self.usage_at <= USAGE_MAX_AGE_SECONDS else None

    def publish(self):
        """Rebuilds the view; wakes the long polls when it changed."""
        now = time.time()
        body = fleet.build(self.hosts, self.tracker, self.usage(now), now, self.todos, self.usage_labels)
        rev = fleet.revision(body)
        if rev != self.rev:
            self.body, self.rev = body, rev
            waiting, self.changed = self.changed, asyncio.Event()
            waiting.set()

    def ingest(self, host, report):
        """One probe line for `host`. A report carrying an error leaves the host's last good time alone,
        so it ages into "off" instead of showing an empty, healthy-looking host."""
        host.error = str(report.get("error") or "")
        if not host.error:
            host.at = time.time()
            host.home = str(report.get("home") or "")
            host.sessions = [s for s in report.get("sessions") or [] if isinstance(s, dict) and not self.own_run(host, s)]
            if self.todos:
                for s in host.sessions:
                    self.todos.want(s.get("replyId"), s.get("reply"), waiting=s.get("status") == "waiting")
        self.publish()

    def own_run(self, host, session):
        """A `claude -p` the hub itself started to write a to-do line: not something to show."""
        return bool(self.todos) and host.name == self.local_name and self.todos.mine(session.get("pid"))

    # ---- probes ----

    def ssh(self, host_name, command, master=False):
        """The ssh command line that runs `command` on another host. The probe's (`master`) becomes the host's SSH
        master when none is listening; the others go down the master when one is, and connect on their own when not."""
        options = list(SSH_OPTIONS)
        if self.control_path:
            pathlib.Path(self.control_path).parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            options += ["-o", f"ControlPath={self.control_path}", "-o", f"ControlMaster={'auto' if master else 'no'}"]
        return ["ssh", *options, host_name, command]

    async def _spawn(self, host):
        if host.name == self.local_name:
            return await asyncio.create_subprocess_exec(
                sys.executable, "-u", str(PROBE_PATH), "--interval", str(self.interval),
                stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
        with PROBE_PATH.open("rb") as script:  # ssh hands the file to the far python as its program
            return await asyncio.create_subprocess_exec(
                *self.ssh(host.name, REMOTE_COMMAND.format(interval=self.interval), master=True),
                stdin=script, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)

    async def watch(self, host):
        delay = RESTART_MIN_SECONDS
        while True:
            proc = None
            try:
                proc = await self._spawn(host)
                async for line in proc.stdout:
                    try:
                        report = json.loads(line)
                    except ValueError:
                        continue  # a login banner or the like
                    if isinstance(report, dict):
                        self.ingest(host, report)
                        delay = RESTART_MIN_SECONDS
                host.error = f"probe exited ({await proc.wait()})"
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                host.error = f"probe failed: {exc}"
            finally:
                if proc and proc.returncode is None:
                    with contextlib.suppress(ProcessLookupError):
                        proc.kill()
            log.warning("%s: %s; retrying in %d s", host.name, host.error, delay)
            await asyncio.sleep(delay)
            delay = min(delay * 2, RESTART_MAX_SECONDS)

    async def sweep(self):
        while True:
            await asyncio.sleep(SWEEP_SECONDS)
            self.publish()
            self.heartbeat = time.time()

    # ---- the background work as a whole ----

    def jobs(self):
        """The loops that must run for as long as the hub does, by name."""
        jobs = {f"probe:{h.name}": (lambda h=h: self.watch(h)) for h in self.hosts}
        jobs["sweep"] = self.sweep
        if self.prometheus:
            jobs["usage"] = self.poll_usage
        return jobs

    async def keep(self, name, job):
        """Runs one background loop for ever. None of them is meant to return or raise; one that does is
        logged, counted (agent_control_task_restarts_total, which the alert watches) and started again."""
        while True:
            try:
                await job()
                log.error("%s returned; starting it again", name)
            except asyncio.CancelledError:
                raise
            except Exception:
                log.exception("%s died; starting it again", name)
            self.task_restarts[name] += 1
            await asyncio.sleep(RESTART_MIN_SECONDS)

    def start(self):
        jobs = self.jobs()
        self.task_restarts = dict.fromkeys(jobs, 0)  # exported at 0 from the start, so the first restart shows as an increase
        return [asyncio.create_task(self.keep(name, job)) for name, job in jobs.items()]

    # ---- the plan's allowance, from an exporter's series in Prometheus (see the README) ----

    def _query(self, expr):
        url = f"{self.prometheus}/api/v1/query?" + urllib.parse.urlencode({"query": expr})
        with urllib.request.urlopen(url, timeout=6) as response:
            return json.load(response)["data"]["result"]

    def _fetch_usage(self):
        fetched = [float(r["value"][1]) for r in self._query("max(claude_usage_last_fetch_timestamp_seconds)")]
        windows = {r["metric"].get("window", ""): float(r["value"][1])
                   for r in self._query("max by (window) (claude_allowance_used_percent)")}
        return windows, (fetched[0] if fetched else 0.0)

    async def poll_usage(self):
        while True:
            try:
                self.usage_values, self.usage_at = await asyncio.to_thread(self._fetch_usage)
                self.usage_fetched_at = time.time()
            except Exception as exc:  # the figures age out on their own
                log.warning("usage: %s", exc)
            self.publish()
            await asyncio.sleep(USAGE_POLL_SECONDS)

    # ---- answers ----

    async def fleet_answer(self, rev, wait):
        if rev and rev == self.rev and wait > 0:
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(self.changed.wait(), min(wait, MAX_WAIT_SECONDS))
        self.last_display_poll = time.time()
        return {"rev": self.rev, "now": int(time.time()), **self.body}

    async def run_on(self, host_name, command, stdin=asyncio.subprocess.DEVNULL):
        """Runs a shell command on a host: here when it is the hub's own, over SSH otherwise. Returns
        (exit status, stderr); (None, "") when it has not finished within COMMAND_TIMEOUT_SECONDS."""
        argv = ["sh", "-c", command] if host_name == self.local_name else self.ssh(host_name, command)
        proc = await asyncio.create_subprocess_exec(*argv, stdin=stdin, stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE)
        try:
            _, err = await asyncio.wait_for(proc.communicate(), COMMAND_TIMEOUT_SECONDS)
        except asyncio.TimeoutError:
            with contextlib.suppress(ProcessLookupError):
                proc.kill()
            return None, ""
        return proc.returncode, err.decode(errors="replace")

    def refusal(self, host_name, status, err, prefix, otherwise):
        """Why a command on a host did not do its job, as a line for the display: the command's own words for
        the person at the desk ("<prefix><summary> — <more>" on stderr) when it left any, else `otherwise`."""
        where = fleet.display_text(host_name, fleet.HOST_CHARS)
        if status is None:
            return f"NO ANSWER FROM {where}"
        if status == 255 and host_name != self.local_name:
            return f"CANNOT REACH {where}"
        said = [line[len(prefix):] for line in err.splitlines() if line.startswith(prefix)]
        return (fleet.display_text(said[-1].split(" \u2014 ")[0], MESSAGE_CHARS) if said else "") or otherwise

    async def focus(self, key):
        """Raises the window of the session shown under `key`, on the host it runs on. Always an answer the
        display can print: {"ok": bool, "message": capitals}."""
        now = time.time()
        found = next(((h, s) for h in self.hosts if h.up(now) for s in h.sessions if fleet.session_key(h.name, s) == key), None)
        if found is None:
            return {"ok": False, "message": "THAT SESSION HAS ENDED"}
        host, raw = found
        if not self.focus_command:
            return {"ok": False, "message": "WINDOW SWITCHING IS NOT SET UP ON THE HUB"}
        where = fleet.display_text(host.name, fleet.HOST_CHARS)
        session_id = str(raw.get("sessionId") or "").lower()
        pid = raw.get("pid")
        if not SESSION_ID.match(session_id) or not (isinstance(pid, int) and pid > 0):  # both go into a shell command
            return {"ok": False, "message": "THAT SESSION HAS NO ID TO SWITCH TO"}
        command = self.focus_command.replace("{id}", session_id).replace("{pid}", str(pid))  # not format(): shells use braces
        status, err = await self.run_on(host.name, command)
        if status == 0:
            return {"ok": True, "message": f"WINDOW RAISED ON {where}"}
        if status == 127:
            return {"ok": False, "message": f"THE FOCUS COMMAND IS NOT INSTALLED ON {where}"}
        message = self.refusal(host.name, status, err, MESSAGE_PREFIX, f"COULD NOT RAISE THE WINDOW ON {where}")
        log.info("focus %s: exit %s: %s", key, status, message)
        return {"ok": False, "message": message}

    async def new_session(self):
        """Opens a new Claude Code session on the desktop run.sh names (the display's "+"). Answers as focus does."""
        host_name, launcher = self.new_session_host, self.new_session_launcher
        if not host_name or not LAUNCHER_ID.match(launcher):  # the launcher's name goes into a shell command
            return {"ok": False, "message": "NO DESKTOP IS SET UP FOR NEW SESSIONS"}
        where = fleet.display_text(host_name, fleet.HOST_CHARS)
        with LAUNCH_PATH.open("rb") as script:  # the far python reads its program from stdin
            status, err = await self.run_on(host_name, LAUNCH_COMMAND.format(launcher=launcher), stdin=script)
        if status == 0:
            log.info("new session opened on %s", host_name)
            return {"ok": True, "message": f"NEW SESSION OPENED ON {where}"}
        message = self.refusal(host_name, status, err, MESSAGE_PREFIX, f"COULD NOT OPEN A SESSION ON {where}")
        log.info("new session on %s: exit %s: %s", host_name, status, message)
        return {"ok": False, "message": message}

    def health(self):
        now = time.time()
        return {"ok": True, "rev": self.rev, "hosts": {
            h.name: {"up": h.up(now), "age_seconds": round(now - h.at, 1) if h.at else None, "sessions": len(h.sessions), "error": h.error}
            for h in self.hosts}}

    def metrics(self):
        now = time.time()
        # The probed machine is `probed_host`: Prometheus gives every series of a job the `host` it was scraped from.
        lines = ["# HELP agent_control_heartbeat_timestamp_seconds When the hub last went round its sweep loop.",
                 "# TYPE agent_control_heartbeat_timestamp_seconds gauge",
                 f"agent_control_heartbeat_timestamp_seconds {self.heartbeat:.0f}",
                 "# HELP agent_control_task_restarts_total Times a background loop has died and been started again.",
                 "# TYPE agent_control_task_restarts_total counter"]
        lines += [f'agent_control_task_restarts_total{{task="{name}"}} {count}' for name, count in self.task_restarts.items()]
        lines += ["# HELP agent_control_host_up 1 while the host's probe is reporting.", "# TYPE agent_control_host_up gauge"]
        lines += [f'agent_control_host_up{{probed_host="{h.name}"}} {int(h.up(now))}' for h in self.hosts]
        lines += ["# HELP agent_control_probe_last_report_timestamp_seconds When the host's probe last reported (0: never).",
                  "# TYPE agent_control_probe_last_report_timestamp_seconds gauge"]
        lines += [f'agent_control_probe_last_report_timestamp_seconds{{probed_host="{h.name}"}} {h.at:.0f}' for h in self.hosts]
        lines += ["# HELP agent_control_sessions Live Claude Code sessions by host and state.", "# TYPE agent_control_sessions gauge"]
        for h in self.hosts:
            states = [self.tracker.status(fleet.session_key(h.name, s), s) for s in h.sessions] if h.up(now) else []
            lines += [f'agent_control_sessions{{probed_host="{h.name}",state="{state}"}} {states.count(state)}' for state in fleet.ORDER]
        if self.prometheus:
            lines += ["# HELP agent_control_usage_last_fetch_timestamp_seconds When Prometheus last answered the allowance query (0: never).",
                      "# TYPE agent_control_usage_last_fetch_timestamp_seconds gauge",
                      f"agent_control_usage_last_fetch_timestamp_seconds {self.usage_fetched_at:.0f}"]
        lines += ["# HELP agent_control_display_last_poll_timestamp_seconds When the display last asked for the fleet (0: never).",
                  "# TYPE agent_control_display_last_poll_timestamp_seconds gauge",
                  f"agent_control_display_last_poll_timestamp_seconds {self.last_display_poll:.0f}"]
        if self.todos:
            lines += ["# HELP agent_control_todo_calls_total claude -p runs made to write a stopped session's to-do line.",
                      "# TYPE agent_control_todo_calls_total counter", f"agent_control_todo_calls_total {self.todos.calls}",
                      "# HELP agent_control_todo_failures_total Of those, runs that gave no line.",
                      "# TYPE agent_control_todo_failures_total counter", f"agent_control_todo_failures_total {self.todos.failures}"]
        return "\n".join(lines) + "\n"


def usage_labels_from(text):
    """AGENT_CONTROL_USAGE_WINDOWS, "seven_day=WK,five_hour=5H": the allowance windows to show, in order, and their
    labels. Unset: fleet.USAGE_LABELS."""
    if text is None:
        return None
    pairs = [part.split("=", 1) for part in text.split(",") if "=" in part]
    return {window.strip(): label.strip()[:fleet.USAGE_LABEL_CHARS] for window, label in pairs if window.strip() and label.strip()}


def claude_binary():
    """AGENT_CONTROL_CLAUDE when set (empty: no to-do lines); otherwise this host's claude CLI, if it can be found."""
    configured = os.environ.get("AGENT_CONTROL_CLAUDE")
    return configured if configured is not None else (find_claude() or "")


def read_device_key():
    key = os.environ.get("AGENT_CONTROL_DEVICE_KEY", "").strip()
    if key:
        return key
    path = pathlib.Path(os.environ.get("AGENT_CONTROL_DEVICE_KEY_FILE", "~/.config/agent-control/device_key")).expanduser()
    return path.read_text().strip()


def create_app(hub=None):
    """`hub` is for tests; left out, the hub is built from the environment and its probes are started."""
    owned = hub is None

    @contextlib.asynccontextmanager
    async def lifespan(app):
        nonlocal hub
        tasks = []
        if owned:
            names = [n.strip() for n in os.environ.get("AGENT_CONTROL_HOSTS", socket.gethostname().split(".")[0]).split(",") if n.strip()]
            hub = Hub(names, read_device_key(), float(os.environ.get("AGENT_CONTROL_INTERVAL", "3")),
                      os.environ.get("AGENT_CONTROL_PROMETHEUS", ""),
                      os.environ.get("AGENT_CONTROL_NEW_SESSION_HOST", ""), os.environ.get("AGENT_CONTROL_NEW_SESSION_LAUNCHER", ""),
                      claude_binary(), focus_command=os.environ.get("AGENT_CONTROL_FOCUS_COMMAND", ""),
                      usage_labels=usage_labels_from(os.environ.get("AGENT_CONTROL_USAGE_WINDOWS")))
            tasks = hub.start()
            log.info("watching %s", ", ".join(names))
        app.state.hub = hub
        yield
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

    app = FastAPI(title="agent-control", lifespan=lifespan)

    def device(authorization):
        hub_ = app.state.hub
        if not hmac.compare_digest(authorization.encode(), f"Bearer {hub_.device_key}".encode()):
            raise HTTPException(401, "bad or missing bearer token")
        return hub_

    @app.get("/api/fleet")
    async def api_fleet(rev: str = "", wait: float = 0, authorization: str = Header("")):
        return await device(authorization).fleet_answer(rev, wait)

    @app.post("/api/focus")
    async def api_focus(key: str, authorization: str = Header("")):
        return await device(authorization).focus(key)

    @app.post("/api/new")
    async def api_new(authorization: str = Header("")):
        return await device(authorization).new_session()

    @app.get("/healthz")
    async def healthz():
        return app.state.hub.health()

    @app.get("/metrics", response_class=PlainTextResponse)
    async def metrics():
        return app.state.hub.metrics()

    return app
