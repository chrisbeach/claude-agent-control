"""The line of what is left for the person to do, for each session that has stopped.

The probe sends a stopped session's last turn (`reply`, keyed by `replyId`, the id of
its last message). Each new turn is put once to a small, fast model through the
host's own `claude -p` (the Claude plan, no API key), which answers in one short
line: "Approve git push to master", "Pick mockup A, B or C", "Nothing - straps
rebuilt". The answer is kept per replyId, so a turn costs one call however long
the session then sits there.

A `claude -p` run is itself a live session as far as `claude agents --json` is
concerned; the pids of these runs are kept in `own_pids` (and for a little while
after they end, as a probe line can be a few seconds old) so the hub can leave
them off the display.
"""
import asyncio
import contextlib
import logging
import os
import pathlib
import time
from collections import OrderedDict

log = logging.getLogger("agent_control")

PENDING = None           # asked, no answer yet
KEEP_TURNS = 256         # answers remembered; a turn that has dropped out is asked again if it is still shown
CONCURRENCY = 2
TIMEOUT_SECONDS = 60
OWN_PID_GRACE_SECONDS = 20
MODEL = "haiku"
SYSTEM_PROMPT = (
    "You read the last turn of a Claude Code session and tell its user, in ONE line of at most 50 characters (a display shows it; shorter is better), "
    "what is outstanding for THEM to do now: an action, a decision, a question to answer, something to approve, "
    "run, check or test. Start with a verb (e.g. \"Approve git push to master\", \"Pick mockup A, B or C\", "
    "\"Run the migration on staging\", \"Try the new login page\"). If nothing is asked of the user, write "
    "\"Nothing - \" and the outcome in a few words. Reply with that line only: no quotes, no markdown."
)
WAITING_NOTE = ("The session is now stopped WAITING for the user: a permission prompt for its last tool call, "
                "or a question it asked. Say what they must answer or approve.")


class Todos:
    def __init__(self, claude, workdir, on_change, model=MODEL):
        self.claude = os.path.expanduser(claude)
        self.workdir = pathlib.Path(os.path.expanduser(workdir))
        self.on_change = on_change
        self.model = model
        self.answers = OrderedDict()    # replyId -> line, "" (no answer to be had) or PENDING
        self.own_pids = {}              # pid of a claude -p run -> when it ended (0 while it runs)
        self.calls = 0
        self.failures = 0
        self._slots = asyncio.Semaphore(CONCURRENCY)

    def get(self, reply_id, default=""):
        return self.answers.get(reply_id, default)

    def mine(self, pid):
        ended = self.own_pids.get(pid)
        return ended is not None and (ended == 0 or time.time() - ended < OWN_PID_GRACE_SECONDS)

    def want(self, reply_id, reply, waiting=False):
        """Asks about a turn not yet asked about. Returns at once; on_change runs when the answer is in."""
        if not reply_id or not reply or reply_id in self.answers:
            if reply_id in self.answers:
                self.answers.move_to_end(reply_id)
            return
        self.answers[reply_id] = PENDING
        while len(self.answers) > KEEP_TURNS:
            self.answers.popitem(last=False)
        asyncio.get_running_loop().create_task(self._ask(reply_id, reply, waiting))

    async def _ask(self, reply_id, reply, waiting):
        prompt = (WAITING_NOTE + "\n\n" if waiting else "") + "The session's last turn:\n\n" + reply
        try:
            async with self._slots:
                line = await self.ask(prompt)
        except Exception as exc:
            self.failures += 1
            log.warning("to-do line for %s: %s", reply_id, exc)
            line = ""
        if reply_id in self.answers:
            self.answers[reply_id] = line
        self.on_change()

    async def ask(self, prompt):
        """One claude -p run: the first line of its answer."""
        self.workdir.mkdir(parents=True, exist_ok=True)  # an empty project: no CLAUDE.md, no memory
        self.calls += 1
        env = {**os.environ, "MAX_THINKING_TOKENS": "0"}
        proc = await asyncio.create_subprocess_exec(
            self.claude, "-p", "--model", self.model, "--no-session-persistence", "--strict-mcp-config",
            "--tools", "", "--system-prompt", SYSTEM_PROMPT,
            cwd=str(self.workdir), env=env,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        self.own_pids[proc.pid] = 0
        try:
            out, err = await asyncio.wait_for(proc.communicate(prompt.encode()), TIMEOUT_SECONDS)
        except asyncio.TimeoutError:
            with contextlib.suppress(ProcessLookupError):
                proc.kill()
            await proc.wait()
            raise RuntimeError(f"claude -p gave no answer in {TIMEOUT_SECONDS} s")
        finally:
            self.own_pids[proc.pid] = time.time()
            self._forget_old_pids()
        if proc.returncode != 0:
            raise RuntimeError(f"claude -p exited {proc.returncode}: {err.decode(errors='replace').strip()[:200]}")
        lines = [l.strip().strip('"') for l in out.decode(errors="replace").splitlines() if l.strip()]
        if not lines:
            raise RuntimeError("claude -p answered nothing")
        return lines[0]

    def _forget_old_pids(self):
        now = time.time()
        for pid in [p for p, ended in self.own_pids.items() if ended and now - ended > OWN_PID_GRACE_SECONDS]:
            del self.own_pids[pid]
