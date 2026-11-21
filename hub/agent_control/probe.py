#!/usr/bin/env python3
"""Lists the Claude Code sessions alive on this host, as one JSON object per line.

    python3 probe.py                 one line, then exit
    python3 probe.py --interval 3    a line every 3 seconds until stdout closes

The session list is `claude agents --json`, the CLI's documented machine interface.
Whether a session has been used yet, and its title, come from the Agent SDK's
`get_session_info` when that package is importable; without it sessions are listed
under their own names only and nothing is said about use. For a session at rest or
waiting on the person, the SDK's `get_session_messages` also gives its last turn
(`reply`, `replyId`), which the hub boils down to the line of what is left to do.
Every session also carries `cost`: what its transcript (and its subagents') records
of the tokens it has spent, weighted by price (see TOKEN_WEIGHTS).

Standard library only (plus the optional SDK), in one file: the hub runs it locally
with its own interpreter and on other hosts by piping it to `python3 -` over SSH,
so nothing has to be installed there for the list itself.
"""
import argparse
import glob
import json
import os
import shutil
import socket
import subprocess
import sys
import time

CLAUDE_TIMEOUT_SECONDS = 15
# A non-login SSH shell has a bare PATH; these are where the native installer puts the CLI.
CLAUDE_CANDIDATES = ("~/.local/bin/claude", "~/.claude/local/claude", "/usr/local/bin/claude")

try:  # optional: titles, and whether a session has been used
    from claude_agent_sdk import get_session_info
except Exception:  # not installed, or too old a Python for it
    get_session_info = None
try:  # optional: the last turn of a session that has stopped, for the hub's to-do line
    from claude_agent_sdk import get_session_messages
except Exception:
    get_session_messages = None

TURN_CHARS = 6000    # of what the session said in its last turn, the end is kept
PROMPT_CHARS = 600   # of the prompt that started the turn
TOOL_CHARS = 600     # of a tool call the session is waiting to make
AT_REST = ("idle", "waiting")
# The cost of a session in input-token equivalents: each kind of token weighted by its price relative to an
# uncached input token, so cache reads (a tenth) do not swamp the figure and output (five times) is not lost in it.
TOKEN_WEIGHTS = {"input_tokens": 1.0, "cache_5m": 1.25, "cache_1h": 2.0, "cache_read_input_tokens": 0.1, "output_tokens": 5.0}
CONFIG_DIR = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.expanduser("~/.claude")
_costs = {}          # sessionId -> SessionCost: transcripts are read once, then only what has been added
_turns = {}          # sessionId -> ((activeAt, status), (reply, replyId)): a transcript is read again only once it changes


def find_claude():
    override = os.environ.get("CLAUDE_BIN")
    if override:
        return override
    found = shutil.which("claude")
    if found:
        return found
    for candidate in CLAUDE_CANDIDATES:
        path = os.path.expanduser(candidate)
        if os.access(path, os.X_OK):
            return path
    return None


def list_sessions(claude):
    """The parsed `claude agents --json` array; raises on any failure."""
    done = subprocess.run([claude, "agents", "--json"], capture_output=True, text=True, timeout=CLAUDE_TIMEOUT_SECONDS,
                          stdin=subprocess.DEVNULL)
    if done.returncode != 0:
        raise RuntimeError(f"claude agents --json exited {done.returncode}: {done.stderr.strip()[:200]}")
    sessions = json.loads(done.stdout)
    if not isinstance(sessions, list):
        raise RuntimeError("claude agents --json did not print an array")
    return sessions


def add_transcript_facts(session):
    """Adds what the SDK reads from the session's transcript: `used` (there is one: something has been asked of
    the session; a new session has none until its first prompt), and for a used session `title` and `activeAt`
    (epoch seconds of the transcript's last write). Where the SDK cannot be asked, nothing is added."""
    if get_session_info is None or not session.get("sessionId"):
        return
    try:  # under the session's directory first; then wherever it is, should that directory not be its project's
        info = get_session_info(session["sessionId"], directory=session.get("cwd")) or get_session_info(session["sessionId"])
    except Exception:
        return
    session["used"] = info is not None
    if info is None:
        return
    title = info.custom_title or info.summary or info.first_prompt
    if title:
        session["title"] = title
    if info.last_modified:
        session["activeAt"] = int(info.last_modified) // 1000


def request_cost(usage):
    """One API request's usage block, weighted by TOKEN_WEIGHTS."""
    written = usage.get("cache_creation_input_tokens") or 0
    hour = (usage.get("cache_creation") or {}).get("ephemeral_1h_input_tokens") or 0
    counts = {"input_tokens": usage.get("input_tokens") or 0, "cache_5m": max(0, written - hour), "cache_1h": hour,
              "cache_read_input_tokens": usage.get("cache_read_input_tokens") or 0, "output_tokens": usage.get("output_tokens") or 0}
    return sum(TOKEN_WEIGHTS[k] * v for k, v in counts.items())


class SessionCost:
    """The running cost of one session over its transcript and its subagents' transcripts. Each file is read from
    where the last call stopped, whole lines only; a request written over several lines (one per content block,
    each repeating the usage) is counted once, by its request id."""

    def __init__(self, session_id):
        self.session_id = session_id
        self.offsets = {}   # path -> bytes consumed
        self.requests = set()
        self.total = 0.0

    def paths(self):
        found = glob.glob(os.path.join(glob.escape(CONFIG_DIR), "projects", "*", glob.escape(self.session_id) + ".jsonl"))
        for main in found[:1]:
            yield main
            yield from sorted(glob.glob(os.path.join(glob.escape(main[:-len(".jsonl")]), "subagents", "*.jsonl")))

    def update(self):
        for path in self.paths():
            try:
                with open(path, "rb") as fh:
                    fh.seek(self.offsets.get(path, 0))
                    data = fh.read()
            except OSError:
                continue
            end = data.rfind(b"\n") + 1  # a line still being written is read next time
            self.offsets[path] = self.offsets.get(path, 0) + end
            for line in data[:end].splitlines():
                if b'"usage"' not in line:
                    continue
                try:
                    entry = json.loads(line)
                except ValueError:
                    continue
                usage = (entry.get("message") or {}).get("usage") if isinstance(entry, dict) else None
                if entry.get("type") != "assistant" or not isinstance(usage, dict):
                    continue
                request = entry.get("requestId") or (entry.get("message") or {}).get("id") or entry.get("uuid")
                if request in self.requests:
                    continue
                self.requests.add(request)
                self.total += request_cost(usage)
        return int(self.total)


def add_cost(session):
    """Adds `cost` (see TOKEN_WEIGHTS) to a session whose transcript can be found."""
    sid = session.get("sessionId")
    if not sid:
        return
    tally = _costs.setdefault(sid, SessionCost(sid))
    try:
        cost = tally.update()
    except Exception:
        return
    if tally.offsets:
        session["cost"] = cost


def _blocks(content):
    if isinstance(content, str):
        return [{"type": "text", "text": content}]
    return [b for b in content or [] if isinstance(b, dict)]


def _is_prompt(message):
    """A user message the person wrote, as opposed to a tool's result handed back to the model."""
    blocks = _blocks((message.message or {}).get("content"))
    return any(b.get("type") == "text" for b in blocks) and not any(b.get("type") == "tool_result" for b in blocks)


def last_turn(messages):
    """(text, id of the last message) for the latest turn: the prompt that started it, everything the session
    said since, and the tool calls of its final message (what a permission prompt or a question is about).
    None when there is no turn."""
    if not messages:
        return None
    start = next((i for i in range(len(messages) - 1, -1, -1) if messages[i].type == "user" and _is_prompt(messages[i])), -1)
    said = []
    for i, m in enumerate(messages[start + 1:], start + 1):
        if m.type != "assistant":
            continue
        for b in _blocks((m.message or {}).get("content")):
            if b.get("type") == "text" and b.get("text", "").strip():
                said.append(b["text"].strip())
            elif b.get("type") == "tool_use" and i == len(messages) - 1:
                said.append(f"[tool call {b.get('name')}: {json.dumps(b.get('input'), ensure_ascii=False)[:TOOL_CHARS]}]")
    text = "\n\n".join(said)[-TURN_CHARS:]
    if start >= 0:
        prompt = "\n".join(b.get("text", "") for b in _blocks(messages[start].message.get("content")) if b.get("type") == "text")
        text = f"USER ASKED:\n{prompt.strip()[:PROMPT_CHARS]}\n\nSESSION SAID:\n{text}"
    return text, messages[-1].uuid


def add_last_turn(session):
    """Adds `reply` and `replyId` to a used session that has stopped: at rest, or waiting on the person."""
    if get_session_messages is None or not session.get("used") or session.get("status") not in AT_REST:
        return
    sid, mark = session["sessionId"], (session.get("activeAt"), session.get("status"))
    known = _turns.get(sid)
    if known is None or known[0] != mark:
        try:
            messages = get_session_messages(sid, directory=session.get("cwd")) or get_session_messages(sid)
        except Exception:
            return
        known = _turns[sid] = (mark, last_turn(messages))
    if known[1]:
        session["reply"], session["replyId"] = known[1]


def snapshot():
    out = {"host": socket.gethostname().split(".")[0], "at": int(time.time()), "home": os.path.expanduser("~"),
           "titles": get_session_info is not None, "sessions": [], "error": None}
    claude = find_claude()
    if not claude:
        out["error"] = "claude CLI not found"
        return out
    try:
        out["sessions"] = [s for s in list_sessions(claude) if isinstance(s, dict)]
    except Exception as exc:  # the hub shows the host as not answering; say why
        out["error"] = str(exc)[:300]
        return out
    for session in out["sessions"]:
        add_transcript_facts(session)
        add_last_turn(session)
        add_cost(session)
    live = {s.get("sessionId") for s in out["sessions"]}
    for cache in (_turns, _costs):
        for sid in [k for k in cache if k not in live]:
            del cache[sid]
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--interval", type=float, default=0, help="seconds between lines; 0 prints one line and exits")
    args = parser.parse_args(argv)
    while True:
        started = time.monotonic()
        try:
            sys.stdout.write(json.dumps(snapshot(), separators=(",", ":")) + "\n")
            sys.stdout.flush()
        except BrokenPipeError:  # the hub went away
            return 0
        if args.interval <= 0:
            return 0
        time.sleep(max(0.2, args.interval - (time.monotonic() - started)))


if __name__ == "__main__":
    sys.exit(main())
