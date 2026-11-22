"""Turns the hosts' probe snapshots into what the display shows. Pure: no I/O, no clock of its own.

The display is small and its font is one Latin face in capitals, so everything it
prints is prepared here: upper-cased, reduced to characters the face has, cut to
the firmware's buffer sizes, sorted, counted.
"""
import hashlib
import json
import unicodedata
from dataclasses import dataclass, field

MAX_SESSIONS = 24      # the firmware's AC_MAX_SESSIONS
MAX_USAGE = 3          # the firmware's AC_MAX_USAGE
TITLE_CHARS = 72       # buffers in firmware/shared/model.h, less the terminator and some slack
NAME_CHARS = 20
DETAIL_CHARS = 36
TODO_CHARS = 58       # what fits beside the pill in the row's 20 px face
HOST_CHARS = 10
TOKENS_CHARS = 10      # "12.3M TOK"; the firmware's buffer holds 15
USAGE_LABEL_CHARS = 4  # a ring gauge's label; the firmware's buffer holds 11, the ring's width fits about 3
STALE_AFTER_SECONDS = 12  # a host that has not reported for this long is shown as off

WORKING, WAITING, IDLE, RUNNING, DONE, FAILED, STOPPED = "working", "waiting", "idle", "running", "done", "failed", "stopped"
# List order: what needs the person first (a question to answer, then a result to look at), then what is
# moving, then what has nothing to show.
ORDER = {WAITING: 0, FAILED: 1, DONE: 2, WORKING: 3, RUNNING: 4, IDLE: 5, STOPPED: 6}
# `claude agents --json`: `status` while the process is alive, `state` for a background session. The CLI's
# "idle" is any session at rest; status_of splits it into done (it has finished something) and idle (never used).
FROM_STATUS = {"busy": WORKING, "waiting": WAITING, "idle": IDLE}
FROM_STATE = {"working": WORKING, "blocked": WAITING, "done": DONE, "failed": FAILED, "stopped": STOPPED}
# A stopped session's line of what is left for the person to do, while it is being made.
TODO_PENDING = "SUMMARISING..."
# Allowance windows of the Claude plan, in display order: a ring gauge each on the display, under
# these labels (AGENT_CONTROL_USAGE_WINDOWS overrides them). A window not listed here is not shown.
USAGE_LABELS = {"seven_day": "WK", "seven_day_fable": "FB", "five_hour": "5H"}

_PUNCTUATION = {"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": "-", "…": "...",
                " ": " ", "•": "-"}


def display_text(text, limit):
    """Capitals in plain ASCII, single-spaced, at most `limit` characters."""
    text = "".join(_PUNCTUATION.get(ch, ch) for ch in str(text or ""))
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    text = " ".join("".join(ch if ch.isprintable() else " " for ch in text).split())
    return text.upper()[:limit].rstrip()


def status_of(raw, worked=False):
    """One of the display's states, or None for a session the list says nothing recognisable about.
    A session at rest is done when it has been used (the probe's `used`: it has a transcript) or when the hub
    has itself seen it mid-turn (`worked`, which is all there is for a host whose probe cannot say); a session
    at rest that has never been asked anything is idle."""
    if raw.get("status") in FROM_STATUS:
        status = FROM_STATUS[raw["status"]]
        return DONE if status == IDLE and (raw.get("used") or worked) else status
    if raw.get("state") in FROM_STATE:
        return FROM_STATE[raw["state"]]
    if raw.get("status") is None and raw.get("state") is None and raw.get("pid"):
        return RUNNING  # a headless `claude -p` run: listed while alive, with no status
    return None


def session_key(host_name, raw):
    """A row's identity: the process where there is one (two processes can share a session id)."""
    return f"{host_name}:{raw.get('pid') or raw.get('id') or raw.get('sessionId')}"[:31]


@dataclass
class HostSnapshot:
    name: str
    at: float = 0.0            # hub clock when the last good report arrived; 0 = never
    home: str = ""
    sessions: list = field(default_factory=list)
    error: str = ""

    def up(self, now):
        return self.at > 0 and now - self.at <= STALE_AFTER_SECONDS


class Tracker:
    """Remembers what the list itself does not say of each session: when it entered its current state, and
    that it has been seen mid-turn."""

    def __init__(self):
        self._seen = {}       # key -> (status, since)
        self._worked = set()  # keys seen working or waiting

    def status(self, key, raw):
        """status_of, with this tracker's memory of the session having been mid-turn."""
        status = status_of(raw, key in self._worked)
        if status in (WORKING, WAITING):
            self._worked.add(key)
        return status

    def since(self, key, status, raw, now):
        known = self._seen.get(key)
        if known and known[0] == status:
            return known[1]
        if known is None and status in (DONE, FAILED, STOPPED) and raw.get("activeAt"):
            since = int(raw["activeAt"])  # first sight of a session at rest: its transcript's last write
        elif known is None and status in (RUNNING, IDLE):
            since = int(raw.get("startedAt") or 0) // 1000  # a headless run, or a session not yet used: since it started
        elif known is None:
            since = 0  # first sight mid-turn: when it began is not known, so no figure is shown
        else:
            since = int(now)
        self._seen[key] = (status, since)
        return since

    def forget_missing(self, live_keys):
        for key in [k for k in self._seen if k not in live_keys]:
            del self._seen[key]
        self._worked &= live_keys


def todo_line(status, raw, todos):
    """What is left for the person to do in a session that has stopped (waiting on them, or done): `todos` maps the
    probe's `replyId` to the line, to None while it is being made; a turn it has nothing for gets no line."""
    if status not in (WAITING, DONE, FAILED) or not raw.get("replyId") or todos is None:
        return ""
    line = todos.get(raw["replyId"], "")
    if line is None:
        return TODO_PENDING
    text = display_text(line, 10 * TODO_CHARS)
    if len(text) <= TODO_CHARS:
        return text
    cut = text[:TODO_CHARS - 3].rsplit(" ", 1)[0] or text[:TODO_CHARS - 3]  # end on a whole word
    return cut.rstrip(" ,;:-") + "..."


def tokens_text(cost):
    """A session's cost (the probe's `cost`, in input-token equivalents) as the display prints it beside its time:
    "850 TOK", "45K TOK", "2.1M TOK", "12M TOK"; empty when the probe had no figure."""
    if not isinstance(cost, (int, float)) or isinstance(cost, bool) or cost < 0:
        return ""
    if cost < 1000:
        return f"{int(cost)} TOK"
    if cost < 1_000_000:
        return f"{int(cost // 1000)}K TOK"
    if cost < 10_000_000:
        return f"{cost // 100_000 / 10:g}M TOK"
    return f"{int(cost // 1_000_000)}M TOK"


def quiet(status, todo):
    """A finished session whose to-do line says nothing is asked of the person (the line's "Nothing - <outcome>"
    form): the display dims it."""
    return status == DONE and todo.startswith("NOTHING")


def row_title(raw):
    """What a session's row is headed with: its title, else its name."""
    return display_text(raw.get("title") or "", TITLE_CHARS) or display_text(raw.get("name") or "", NAME_CHARS) or "UNTITLED"


def build(hosts, tracker, usage, now, todos=None, usage_labels=USAGE_LABELS):
    """The /api/fleet body without `rev` and `now`. `hosts` is the configured list of HostSnapshot, in display order;
    `usage` is {window: percent} or None; `todos` as todo_line takes it; `usage_labels` {window: label}, in order."""
    rows, host_rows, live_keys = [], [], set()
    for host in hosts:
        label = display_text(host.name, HOST_CHARS)
        up = host.up(now)
        mine = []
        for raw in host.sessions if up else []:
            key = session_key(host.name, raw)
            status = tracker.status(key, raw)
            if status is None:
                continue
            live_keys.add(key)
            name = display_text(raw.get("name") or "", NAME_CHARS)
            todo = todo_line(status, raw, todos)
            mine.append({
                "key": key,
                "host": label,
                "title": row_title(raw),
                "name": name,  # the display goes by the title; kept for logs and tests
                "status": status,
                "detail": display_text(raw.get("waitingFor") or "", DETAIL_CHARS) if status == WAITING else "",
                "todo": todo,
                "quiet": quiet(status, todo),
                "tokens": tokens_text(raw.get("cost")),
                "since": tracker.since(key, status, raw, now),
            })
        rows.extend(mine)
        host_rows.append({"name": label, "up": up, "sessions": len(mine), "waiting": sum(r["status"] == WAITING for r in mine)})
    tracker.forget_missing(live_keys)

    # Longest-waiting first among those blocked on the person; most recently changed first among the rest,
    # so the result that has just come in heads the finished ones.
    # A finished session with nothing asked of the person goes after the finished ones that do want something.
    rows.sort(key=lambda r: (ORDER[r["status"]], r["quiet"], r["since"] if r["status"] == WAITING else -r["since"], r["key"]))
    counts = {  # the display's three figures; unused and stopped sessions are in none of them
        "working": sum(r["status"] in (WORKING, RUNNING) for r in rows),
        "waiting": sum(r["status"] == WAITING for r in rows),
        "done": sum(r["status"] in (DONE, FAILED) for r in rows),
    }
    return {
        "counts": counts,
        "usage": [{"label": label, "percent": int(round(usage[window]))} for window, label in usage_labels.items()
                  if usage and window in usage][:MAX_USAGE],
        "hosts": host_rows,
        "sessions": rows[:MAX_SESSIONS],
    }


def revision(body):
    return hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()[:12]
