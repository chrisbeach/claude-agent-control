import json

from agent_control import fleet

NOW = 1795049400.0
# Shaped like real `claude agents --json` rows (devbox, 2026-11-19) with the probe's used/title/activeAt added.
BUSY = {"pid": 2728159, "cwd": "/home/alice/dev", "kind": "interactive", "startedAt": 1795047600000, "sessionId": "11111111-2222-4333-8444-555555555555",
        "name": "dev-1f", "status": "busy", "used": True, "title": "Write API docs for the v2 endpoints", "activeAt": 1795049390}
# The CLI calls both of these "idle": one has finished a turn, the other has never been asked anything.
FINISHED = {"pid": 2453247, "cwd": "/home/alice/dev", "kind": "interactive", "startedAt": 1795017386642, "sessionId": "66666666-7777-4888-9999-aaaaaaaaaaaa",
            "name": "dev-71", "status": "idle", "used": True, "title": "Migrate CI to the new runners", "activeAt": 1795047683}
UNUSED = {"pid": 3304445, "cwd": "/home/alice/dev", "kind": "interactive", "startedAt": 1795049116564, "sessionId": "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff",
          "name": "dev-c2", "status": "idle", "used": False}
WAITING = {"pid": 2714904, "cwd": "/home/alice/dev/web-app", "kind": "interactive", "startedAt": 1795047900000, "sessionId": "e00cb079-aaaa-bbbb-cccc-000000000000",
           "name": "dev-de", "status": "waiting", "used": True, "waitingFor": "permission prompt", "title": "Upload progress bar"}
HEADLESS = {"pid": 2731157, "cwd": "/tmp/x", "kind": "interactive", "startedAt": 1795048211521, "sessionId": "cccccccc-dddd-4eee-8fff-000000000001",
            "name": "scratchpad-94"}


def host(name, sessions, at=NOW, home="/home/alice"):
    return fleet.HostSnapshot(name, at=at, home=home, sessions=sessions)


def build(hosts, usage=None, tracker=None, now=NOW):
    return fleet.build(hosts, tracker or fleet.Tracker(), usage, now)


def test_states_from_the_cli_fields():
    assert fleet.status_of(BUSY) == "working"
    assert fleet.status_of(FINISHED) == "done"  # at rest with a transcript: it has finished something
    assert fleet.status_of(UNUSED) == "idle"    # at rest and never asked anything
    assert fleet.status_of(WAITING) == "waiting"
    assert fleet.status_of(HEADLESS) == "running"  # alive with no status: a `claude -p` run
    assert fleet.status_of({"id": "bg1", "kind": "background", "state": "blocked"}) == "waiting"
    assert fleet.status_of({"id": "bg1", "kind": "background", "state": "done"}) == "done"
    assert fleet.status_of({"pid": 1, "status": "something-new"}) is None


def test_rows_are_ready_to_print():
    body = build([host("devbox", [BUSY])])
    row = body["sessions"][0]
    assert row["title"] == "WRITE API DOCS FOR THE V2 ENDPOINTS"
    assert row["host"] == "DEVBOX" and row["name"] == "DEV-1F"
    assert row["key"] == "devbox:2728159"
    assert set(row) == {"key", "host", "title", "name", "status", "detail", "todo", "quiet", "since", "tokens"}  # nothing the display does not print


def test_text_is_reduced_to_what_the_face_can_draw():
    assert fleet.display_text("Fix “smart” quotes — café… \U0001F680 ok", 80) == 'FIX "SMART" QUOTES - CAFE... OK'
    assert fleet.display_text("a\n\tb   c", 80) == "A B C"
    assert len(fleet.display_text("x" * 200, fleet.TITLE_CHARS)) == fleet.TITLE_CHARS


def test_a_session_without_a_title_goes_by_its_name():
    body = build([host("nas", [HEADLESS], home="/home/alice")])
    row = body["sessions"][0]
    assert row["title"] == "SCRATCHPAD-94" and row["status"] == "running"
    assert row["since"] == 1795048211  # running since it started


def test_order_and_counts():
    older = dict(FINISHED, pid=1, name="dev-00", activeAt=FINISHED["activeAt"] - 600)
    body = build([host("devbox", [UNUSED, older, FINISHED, BUSY, HEADLESS, WAITING])])
    # what needs the person first: the question, then the results (the latest first); then what is moving; the unused last
    assert [(r["status"], r["name"]) for r in body["sessions"]] == [
        ("waiting", "DEV-DE"), ("done", "DEV-71"), ("done", "DEV-00"), ("working", "DEV-1F"), ("running", "SCRATCHPAD-94"), ("idle", "DEV-C2")]
    assert body["counts"] == {"working": 2, "waiting": 1, "done": 2}  # the unused session is in none of the three
    assert body["sessions"][0]["detail"] == "PERMISSION PROMPT"
    assert body["hosts"] == [{"name": "DEVBOX", "up": True, "sessions": 6, "waiting": 1}]


def test_where_the_probe_cannot_say_a_session_at_rest_is_done_once_it_has_been_seen_working():
    # a host with no Agent SDK (nas): the rows carry no `used`, no title
    working = {k: v for k, v in BUSY.items() if k not in ("used", "title", "activeAt")}
    at_rest = dict(working, status="idle")
    state = lambda tracker, rows: [r["status"] for r in build([host("nas", rows)], tracker=tracker)["sessions"]]
    tracker = fleet.Tracker()
    assert state(tracker, [at_rest]) == ["idle"]     # nothing is known of it
    assert state(tracker, [working]) == ["working"]
    assert state(tracker, [at_rest]) == ["done"]     # the hub watched it work
    assert state(tracker, []) == []                  # it ends ...
    assert state(tracker, [at_rest]) == ["idle"]     # ... and another process under that pid starts unknown
    # and the hub's own sight of it counts even against a probe that found no transcript
    assert state(tracker, [dict(working, used=False)]) == ["working"]
    assert state(tracker, [dict(at_rest, used=False)]) == ["done"]


def test_a_quiet_host_is_off_and_its_sessions_are_withdrawn():
    stale = host("buildbox", [BUSY], at=NOW - fleet.STALE_AFTER_SECONDS - 1)
    never = fleet.HostSnapshot("laptop")
    body = build([host("devbox", [FINISHED]), stale, never])
    assert [(h["name"], h["up"], h["sessions"]) for h in body["hosts"]] == [("DEVBOX", True, 1), ("BUILDBOX", False, 0), ("LAPTOP", False, 0)]
    assert [r["host"] for r in body["sessions"]] == ["DEVBOX"]


def test_since_tracks_the_state():
    tracker = fleet.Tracker()
    first = build([host("devbox", [FINISHED, UNUSED, BUSY])], tracker=tracker)
    by_name = {r["name"]: r for r in first["sessions"]}
    assert by_name["DEV-71"]["since"] == 1795047683  # done on first sight: the transcript's last write
    assert by_name["DEV-C2"]["since"] == 1795049116  # not yet used: idle since it started
    assert by_name["DEV-1F"]["since"] == 0           # mid-turn on first sight: not known, so no figure

    finished = dict(BUSY, status="idle")
    later = build([host("devbox", [FINISHED, finished], at=NOW + 60)], tracker=tracker, now=NOW + 60)
    by_name = {r["name"]: r for r in later["sessions"]}
    assert (by_name["DEV-1F"]["status"], by_name["DEV-1F"]["since"]) == ("done", int(NOW + 60))  # the turn was seen to end: now
    assert by_name["DEV-71"]["since"] == 1795047683      # unchanged
    assert later["sessions"][0]["name"] == "DEV-1F"      # the result just in heads the list

    again = build([host("devbox", [FINISHED, finished], at=NOW + 120)], tracker=tracker, now=NOW + 120)
    assert {r["name"]: r["since"] for r in again["sessions"]}["DEV-1F"] == int(NOW + 60)


def test_revision_ignores_the_clock_but_not_the_content():
    tracker = fleet.Tracker()
    a = fleet.revision(build([host("devbox", [FINISHED])], tracker=tracker))
    b = fleet.revision(build([host("devbox", [FINISHED], at=NOW + 3)], tracker=tracker, now=NOW + 3))
    c = fleet.revision(build([host("devbox", [dict(FINISHED, status="busy")], at=NOW + 6)], tracker=tracker, now=NOW + 6))
    assert a == b != c


def test_usage_windows_are_named_and_ordered():
    body = build([host("devbox", [])], usage={"seven_day_fable": 60.2, "seven_day": 33, "mystery": 9})
    assert body["usage"] == [{"label": "WEEK", "percent": 33}, {"label": "FABLE", "percent": 60}]
    assert build([host("devbox", [])])["usage"] == []


def test_the_body_fits_the_firmware():
    many = [dict(BUSY, pid=1000 + i, name=f"dev-{i:02d}") for i in range(40)]
    body = build([host("devbox", many)])
    assert len(body["sessions"]) == fleet.MAX_SESSIONS and body["hosts"][0]["sessions"] == 40
    assert len(json.dumps(body)) < 24 * 1024  # MAX_FLEET_BODY in firmware/src/main.c


def test_a_stopped_session_carries_its_to_do_line():
    done, waiting = dict(FINISHED, replyId="r-done"), dict(WAITING, replyId="r-wait")
    busy = dict(BUSY, replyId="r-busy")  # a turn under way has nothing to do yet, whatever the probe sent
    todos = {"r-done": "Try the new login page", "r-wait": None, "r-busy": "Stale"}
    rows = {r["name"]: r for r in build_with_todos([host("devbox", [done, waiting, busy, dict(UNUSED)])], todos)["sessions"]}
    assert rows["DEV-71"]["todo"] == "TRY THE NEW LOGIN PAGE"
    assert rows["DEV-DE"]["todo"] == fleet.TODO_PENDING  # asked, not yet answered
    assert rows["DEV-1F"]["todo"] == "" and rows["DEV-C2"]["todo"] == ""
    assert not any(r["quiet"] for r in rows.values())
    quiet = build_with_todos([host("devbox", [done, waiting])], {"r-done": "Nothing - audit done", "r-wait": "Nothing - odd"})["sessions"]
    assert {r["name"]: r["quiet"] for r in quiet} == {"DEV-71": True, "DEV-DE": False}  # a waiting session always wants something
    older = dict(FINISHED, pid=1, name="dev-01", activeAt=FINISHED["activeAt"] - 600, replyId="r-old")
    order = build_with_todos([host("devbox", [done, older])], {"r-done": "Nothing - audit done", "r-old": "Pick a mockup"})["sessions"]
    assert [r["name"] for r in order] == ["DEV-01", "DEV-71"]  # the one wanting something first, though older
    assert build([host("devbox", [done])])["sessions"][0]["todo"] == ""  # no summariser: no line
    long = "Reprint with support box unticked, then judge the fin tabs and left edge"
    cut = build_with_todos([host("devbox", [done])], {"r-done": long})["sessions"][0]["todo"]
    assert cut == "REPRINT WITH SUPPORT BOX UNTICKED, THEN JUDGE THE FIN..." and len(cut) <= fleet.TODO_CHARS


def build_with_todos(hosts, todos):
    return fleet.build(hosts, fleet.Tracker(), None, NOW, todos)


def test_a_session_carries_its_token_cost():
    assert [fleet.tokens_text(c) for c in (0, 850, 45_600, 2_000_000, 2_190_000, 12_700_000)] == \
        ["0 TOK", "850 TOK", "45K TOK", "2M TOK", "2.1M TOK", "12M TOK"]
    assert fleet.tokens_text(None) == "" and fleet.tokens_text(True) == ""
    rows = {r["name"]: r for r in build([host("devbox", [dict(BUSY, cost=3_456_789), dict(UNUSED)])])["sessions"]}
    assert rows["DEV-1F"]["tokens"] == "3.4M TOK" and rows["DEV-C2"]["tokens"] == ""
    assert all(len(r["tokens"]) <= fleet.TOKENS_CHARS for r in rows.values())
