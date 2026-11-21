import asyncio
import json
import os
import stat
import sys
from types import SimpleNamespace

from agent_control import probe, todo


def fake_claude(tmp_path, answer="Approve git push to master", status=0):
    """A stand-in `claude` that records how it was run and prints `answer`."""
    script = tmp_path / "claude"
    script.write_text(f"""#!{sys.executable}
import json, os, sys
json.dump({{"argv": sys.argv[1:], "stdin": sys.stdin.read(), "cwd": os.getcwd(),
           "thinking": os.environ.get("MAX_THINKING_TOKENS")}}, open({str(tmp_path / 'call.json')!r}, "w"))
print({answer!r})
sys.exit({status})
""")
    script.chmod(script.stat().st_mode | stat.S_IEXEC)
    return str(script)


def ask_once(tmp_path, reply_id="r1", reply="USER ASKED: push it", waiting=False, **kw):
    async def go():
        changed = asyncio.Event()
        todos = todo.Todos(fake_claude(tmp_path, **kw), tmp_path / "work", changed.set)
        todos.want(reply_id, reply, waiting)
        assert todos.get(reply_id, "absent") is todo.PENDING
        await asyncio.wait_for(changed.wait(), 10)
        todos.want(reply_id, reply, waiting)  # the same turn again: no second run
        return todos
    return asyncio.run(go())


def test_a_turn_is_asked_about_once_with_no_thinking_and_no_tools(tmp_path):
    todos = ask_once(tmp_path, waiting=True)
    call = json.loads((tmp_path / "call.json").read_text())
    assert todos.get("r1") == "Approve git push to master" and todos.calls == 1
    assert call["thinking"] == "0" and call["cwd"] == str(tmp_path / "work")
    assert call["argv"][:3] == ["-p", "--model", "haiku"] and call["argv"][call["argv"].index("--tools") + 1] == ""
    assert "WAITING" in call["stdin"] and call["stdin"].endswith("USER ASKED: push it")


def test_a_failed_run_leaves_no_line_and_is_counted(tmp_path):
    todos = ask_once(tmp_path, answer="", status=1)
    assert todos.get("r1", "absent") == "" and todos.failures == 1


def test_its_own_runs_are_known_for_a_while_after_they_end(tmp_path):
    todos = ask_once(tmp_path)
    (pid, ended), = todos.own_pids.items()
    assert ended > 0 and todos.mine(pid) and not todos.mine(pid + 1)
    todos.own_pids[pid] = ended - todo.OWN_PID_GRACE_SECONDS - 1
    assert not todos.mine(pid)


def msg(type_, content, uuid="u"):
    return SimpleNamespace(type=type_, uuid=uuid, message={"content": content})


def test_the_last_turn_is_the_prompt_what_was_said_and_the_tool_call_it_stopped_on():
    messages = [
        msg("user", "an old prompt"), msg("assistant", [{"type": "text", "text": "old answer"}]),
        msg("user", [{"type": "text", "text": "deploy it"}]),
        msg("assistant", [{"type": "thinking", "thinking": "hmm"}, {"type": "text", "text": "Tests pass."},
                          {"type": "tool_use", "name": "Bash", "input": {"command": "ls"}}]),
        msg("user", [{"type": "tool_result", "content": "ok"}]),
        msg("assistant", [{"type": "text", "text": "Pushing now."}, {"type": "tool_use", "name": "Bash", "input": {"command": "git push"}}], uuid="last"),
    ]
    text, reply_id = probe.last_turn(messages)
    assert reply_id == "last"
    assert text == ('USER ASKED:\ndeploy it\n\nSESSION SAID:\nTests pass.\n\nPushing now.\n\n'
                    '[tool call Bash: {"command": "git push"}]')  # only the call it stopped on, not the ls on the way
    assert probe.last_turn([]) is None
