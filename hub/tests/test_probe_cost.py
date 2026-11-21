import json

from agent_control import probe

SID = "11111111-2222-4333-8444-555555555555"


def assistant(request, usage, sidechain=False):
    return json.dumps({"type": "assistant", "requestId": request, "isSidechain": sidechain, "sessionId": SID,
                       "message": {"model": "claude-fable-5-2", "usage": usage}}) + "\n"


def test_cost_is_read_incrementally_once_per_request_and_includes_subagents(tmp_path, monkeypatch):
    monkeypatch.setattr(probe, "CONFIG_DIR", str(tmp_path))
    project = tmp_path / "projects" / "-home-alice-dev"
    project.mkdir(parents=True)
    main = project / f"{SID}.jsonl"
    usage = {"input_tokens": 10, "cache_creation_input_tokens": 1000, "cache_read_input_tokens": 20000, "output_tokens": 100,
             "cache_creation": {"ephemeral_1h_input_tokens": 400, "ephemeral_5m_input_tokens": 600}}
    # 10 + 600*1.25 + 400*2 + 20000*0.1 + 100*5 = 4060
    main.write_text(json.dumps({"type": "user", "message": {"content": "hi"}}) + "\n" + assistant("r1", usage) + assistant("r1", usage))
    session = {"sessionId": SID}
    probe.add_cost(session)
    assert session["cost"] == 4060

    with main.open("a") as fh:  # a second request, its line not yet finished
        line = assistant("r2", {"input_tokens": 0, "output_tokens": 200})
        fh.write(line[:30])
    probe.add_cost(session)
    assert session["cost"] == 4060
    with main.open("a") as fh:
        fh.write(line[30:])
    sub = project / SID / "subagents"
    sub.mkdir(parents=True)
    (sub / "agent-a1.jsonl").write_text(assistant("r3", {"input_tokens": 1000}, sidechain=True))
    probe.add_cost(session)
    assert session["cost"] == 4060 + 1000 + 1000


def test_no_transcript_means_no_cost(tmp_path, monkeypatch):
    monkeypatch.setattr(probe, "CONFIG_DIR", str(tmp_path))
    session = {"sessionId": "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff"}
    probe.add_cost(session)
    assert "cost" not in session
