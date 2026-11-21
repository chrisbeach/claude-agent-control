import asyncio
import json
import os
import stat
import sys

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


def chain(*entries):
    """Transcript entries linked newest-to-oldest by parentUuid in the order given, each with a uuid."""
    out, parent = [], None
    for i, entry in enumerate(entries):
        entry = {"uuid": f"e{i}", "parentUuid": parent, **entry}
        out.append(entry)
        parent = entry["uuid"]
    return out


def msg(type_, content, **extra):
    return {"type": type_, "message": {"content": content}, **extra}


def said(text):
    return msg("assistant", [{"type": "text", "text": text}])


def test_the_last_turn_is_the_prompt_what_was_said_and_the_tool_call_it_stopped_on():
    entries = chain(
        msg("user", "an old prompt"), said("old answer"),
        msg("user", [{"type": "text", "text": "deploy it"}]),
        msg("assistant", [{"type": "thinking", "thinking": "hmm"}, {"type": "text", "text": "Tests pass."},
                          {"type": "tool_use", "name": "Bash", "input": {"command": "ls"}}]),
        msg("user", [{"type": "tool_result", "content": "ok"}]),
        {"type": "attachment", "attachment": {}},  # not a message, but the chain runs through it
        msg("user", "<system-reminder>", isMeta=True),
        msg("assistant", [{"type": "text", "text": "Pushing now."}, {"type": "tool_use", "name": "Bash", "input": {"command": "git push"}}]),
        {"type": "system", "subtype": "turn_duration"},
    )
    entries.insert(3, {**said("a subagent's word"), "uuid": "side", "parentUuid": "e2", "isSidechain": True})
    text, reply_id = probe.last_turn(entries)
    assert reply_id == "e7"
    assert text == ('USER ASKED:\ndeploy it\n\nSESSION SAID:\nTests pass.\n\nPushing now.\n\n'
                    '[tool call Bash: {"command": "git push"}]')  # only the call it stopped on, not the ls on the way
    assert probe.last_turn([]) is None


def compacted(*after):
    """A session whose last turn asked something of its person, then a compaction, then `after`."""
    before = chain(msg("user", "finish the migration"), said("Close the game, then tell me."),
                   {"type": "system", "subtype": "turn_duration"})
    boundary = {"type": "system", "subtype": "compact_boundary", "uuid": "b", "parentUuid": None, "logicalParentUuid": before[-1]["uuid"]}
    summary = {**msg("user", "This session is being continued from a previous conversation... Request 1 was completed."),
               "uuid": "s", "parentUuid": "b", "isCompactSummary": True}
    rest = [{"uuid": f"a{i}", **entry} for i, entry in enumerate(after)]
    for entry, parent in zip(rest, ["s"] + [e["uuid"] for e in rest]):
        entry["parentUuid"] = parent
    return before + [boundary, summary] + rest


def test_a_session_compacted_while_idle_still_gives_the_turn_before_the_compaction():
    text, reply_id = probe.last_turn(compacted({"type": "system", "subtype": "local_command"}))
    assert text == "USER ASKED:\nfinish the migration\n\nSESSION SAID:\nClose the game, then tell me."
    assert reply_id == "e1"  # the same turn as before the compaction, so not asked about again
    assert "continued from a previous conversation" not in text


def test_a_turn_compacted_part_way_keeps_its_prompt_and_what_was_said_on_both_sides():
    text, _ = probe.last_turn(compacted(said("Carrying on: launcher patched."), msg("user", "thanks, all good")))
    assert text == "USER ASKED:\nthanks, all good\n\nSESSION SAID:\n"
    text, reply_id = probe.last_turn(compacted(said("Carrying on: launcher patched.")))
    assert reply_id == "a0"
    assert text == ("USER ASKED:\nfinish the migration\n\nSESSION SAID:\n"
                    "Close the game, then tell me.\n\nCarrying on: launcher patched.")


def test_the_last_turn_is_read_from_the_sessions_transcript(tmp_path, monkeypatch):
    monkeypatch.setattr(probe, "CONFIG_DIR", str(tmp_path))
    monkeypatch.setattr(probe, "_turns", {})
    sid = "11111111-2222-4333-8444-555555555555"
    project = tmp_path / "projects" / "-home-alice-dev"
    project.mkdir(parents=True)
    lines = [json.dumps(e) for e in compacted()] + ['{"type": "assistant", "uuid": "half-writ']
    (project / f"{sid}.jsonl").write_text("\n".join(lines))
    session = {"sessionId": sid, "used": True, "status": "idle", "activeAt": 1}
    probe.add_last_turn(session)
    assert session["replyId"] == "e1" and session["reply"].endswith("Close the game, then tell me.")
    for unread in ({"sessionId": sid, "used": True, "status": "busy"}, {"sessionId": sid, "status": "idle"},
                   {"sessionId": "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff", "used": True, "status": "idle"}):
        probe.add_last_turn(unread)
        assert "reply" not in unread
