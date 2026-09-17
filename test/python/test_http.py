"""Integration test at the SQL seam: jev_choice makes one POST per row to the
endpoint named in the jev secret, and maps the answer onto the ENUM.

The mock replays a real response recorded from api.typesafe.ai on 2026-09-17,
so the expected values come from the actual service, not from this test.
"""

import json, os, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DUCKDB = os.environ.get("DUCKDB_BIN") or os.path.join(ROOT, "build/release/duckdb")
EXT = os.environ.get("JEV_EXTENSION") or os.path.join(ROOT, "build/release/extension/jev/jev.duckdb_extension")

# Recorded 2026-09-17, model jev-1.13.0, state "I want a refund for last month..."
RECORDED = {
    "model": "jev-1.13.0",
    "answers": {
        "intent": {
            "type": "choice",
            "choice": "refund",
            "confidence": 1.0,
            "probabilities": {"refund": 1.0, "bug": 0.0, "praise": 0.0},
        },
        "severity": {
            "type": "score",
            "score": 2.1,
            "confidence": 0.9,
            "legend": {
                "0": "Trivial",
                "1": "Minor",
                "2": "Normal",
                "3": "Serious",
                "4": "Critical",
            },
            "probabilities": {"0": 0.0, "1": 0.01, "2": 0.89, "3": 0.1, "4": 0.0},
        },
        "urgent": {"type": "noul", "noul": 0.6},
    },
    "usage": {"input_tokens": 412, "output_tokens": 69},
}

requests = []

DELAY = 0.0  # per-request latency the mock adds; set by a test
FAIL_FIRST = []  # HTTP statuses to answer with before succeeding; consumed in order
ALWAYS_FAIL = {}  # state text -> HTTP status; every request for that state fails
DROP_NEXT = [0]  # how many connections to drop without answering at all


class Mock(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if DELAY:
            time.sleep(DELAY)
        if DROP_NEXT[0] > 0:
            DROP_NEXT[0] -= 1
            requests.append({"path": self.path, "status": "dropped"})
            self.close_connection = True
            return
        if FAIL_FIRST or body["state"] in ALWAYS_FAIL:
            status = FAIL_FIRST.pop(0) if FAIL_FIRST else ALWAYS_FAIL[body["state"]]
            requests.append({"path": self.path, "status": status})
            self.send_response(status)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"{}")
            return
        requests.append({"path": self.path, "auth": self.headers.get("Authorization"), "body": body})
        resp = json.loads(json.dumps(RECORDED))
        if set(body["questions"]) != {"q"}:
            # jev_ask: several named questions in one request; answer each from the
            # recorded response by matching on its type
            by_type = {
                "choice": RECORDED["answers"]["intent"],
                "score": RECORDED["answers"]["severity"],
                "noul": RECORDED["answers"]["urgent"],
            }
            resp["answers"] = {name: by_type[q["type"]] for name, q in body["questions"].items()}
            out = json.dumps(resp).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)
            return
        q = body["questions"]["q"]
        if q["type"] == "choice":
            # answer with whichever option the state names, else the recorded answer
            state = body["state"].lower()
            opts = list(q["criteria"].keys())
            pick = next((o for o in opts if o in state), RECORDED["answers"]["intent"]["choice"])
            resp["answers"] = {"q": {**RECORDED["answers"]["intent"], "choice": pick}}
        elif q["type"] == "score":
            resp["answers"] = {"q": RECORDED["answers"]["severity"]}
        else:
            resp["answers"] = {"q": RECORDED["answers"]["urgent"]}
        out = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, *a):
        pass


def sql(server_url, query):
    script = f"""
LOAD '{EXT}';
CREATE SECRET (TYPE jev, API_KEY 'test-key-123', ENDPOINT '{server_url}');
{query}
"""
    r = subprocess.run([DUCKDB, "-unsigned", "-json", "-c", script], capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError(f"duckdb failed:\n{r.stderr}")
    # -json prints one array per statement that produces rows; keep the last.
    dec, pos, last, text = json.JSONDecoder(), 0, [], r.stdout.strip()
    while pos < len(text):
        obj, pos = dec.raw_decode(text, pos)
        last = obj
        while pos < len(text) and text[pos].isspace():
            pos += 1
    return last


def test_one_post_per_row(url):
    requests.clear()

    rows = sql(
        url,
        """
        CREATE TABLE t AS SELECT * FROM (VALUES
            ('I want a refund, the charge was wrong'),
            ('the export crashes, clearly a bug'),
            ('lovely product, pure praise')) v(body);
        SELECT jev_choice(body, MAP{'refund':'wants money back','bug':'something broken','praise':'a compliment'}) AS intent
        FROM t ORDER BY body;""",
    )

    assert [r["intent"] for r in rows] == ["refund", "praise", "bug"], rows
    assert len(requests) == 3, f"expected one POST per row, got {len(requests)}"
    req = requests[0]
    assert req["path"] == "/v1/systemone", req["path"]
    assert req["auth"] == "Bearer test-key-123", req["auth"]
    assert req["body"]["model"] == "jev-latest", req["body"]
    q = req["body"]["questions"]["q"]
    assert q["type"] == "choice" and q["criteria"] == {
        "refund": "wants money back",
        "bug": "something broken",
        "praise": "a compliment",
    }, q
    print("PASS one_post_per_row: 3 rows, 3 POSTs, bearer header, request shape, enum answers")


def test_same_call_in_where_and_select_is_one_request_per_row(url):
    """DuckDB evaluates a volatile function once per occurrence. With the function in
    both WHERE and SELECT that is two requests per row, and two bills. The extension
    must answer the second occurrence from what it already knows."""
    requests.clear()
    rows = sql(
        url,
        """
        CREATE TABLE t AS SELECT * FROM (VALUES
            ('I want a refund, the charge was wrong'),
            ('the export crashes, clearly a bug'),
            ('lovely product, pure praise')) v(body);
        SELECT body, jev_choice(body, MAP{'refund':'r','bug':'b','praise':'p'}) AS intent
        FROM t
        WHERE jev_choice(body, MAP{'refund':'r','bug':'b','praise':'p'}) <> 'praise'
        ORDER BY body;""",
    )
    assert [r["intent"] for r in rows] == ["refund", "bug"], rows
    assert len(requests) == 3, f"expected 3 requests for 3 rows, got {len(requests)}: paid twice"
    print("PASS where_and_select: 3 rows, 3 requests, second evaluation served from cache")


def test_rows_in_a_chunk_are_requested_concurrently(url):
    """One request per row is the API's floor. Waiting for each in turn is not.
    16 distinct rows at 200ms each: sequential is 3.2s, concurrent well under 1s."""
    global DELAY
    requests.clear()
    DELAY = 0.2
    try:
        t0 = time.time()
        rows = sql(
            url,
            """
            CREATE TABLE t AS SELECT 'ticket ' || i || ' mentions a bug' AS body FROM range(16) r(i);
            SELECT count(*) AS n FROM t WHERE jev_choice(body, MAP{'bug':'b','praise':'p'}) = 'bug';""",
        )
        elapsed = time.time() - t0
    finally:
        DELAY = 0.0
    assert rows[0]["n"] == 16, rows
    assert len(requests) == 16, len(requests)
    assert elapsed < 1.5, f"16 rows at 200ms took {elapsed:.2f}s: requests are sequential"
    print(f"PASS concurrent_chunk: 16 rows at 200ms in {elapsed:.2f}s")


def test_a_429_is_retried_with_backoff(url):
    """The API documents 429 with no quota numbers: back off and retry. One 429 must
    cost a short wait, not the query."""
    global FAIL_FIRST
    requests.clear()
    FAIL_FIRST = [429]
    rows = sql(
        url,
        """
        CREATE TABLE t AS SELECT 'a refund please' AS body;
        SELECT jev_choice(body, MAP{'refund':'r','bug':'b'}) AS intent FROM t;""",
    )
    assert rows == [{"intent": "refund"}], rows
    statuses = [r.get("status", 200) for r in requests]
    assert statuses == [429, 200], f"expected one 429 then a retry, got {statuses}"
    print("PASS retry_429: one 429, one retry, answer delivered")


def test_a_dropped_connection_is_retried(url):
    """A connection closed before any status is exactly as transient as a 503:
    a proxy recycling, a keep-alive expiring, a server restarting. The answer is
    already paid for by then, so retry rather than fail the query."""
    requests.clear()
    DROP_NEXT[0] = 1
    try:
        rows = sql(
            url,
            """SELECT jev_choice('a refund please', MAP{'refund':'r','bug':'b'}) AS intent;""",
        )
    finally:
        DROP_NEXT[0] = 0
    assert rows == [{"intent": "refund"}], rows
    kinds = [r.get("status", 200) for r in requests]
    assert kinds == ["dropped", 200], f"expected one drop then a retry, got {kinds}"
    print("PASS retry_dropped_connection: one drop, one retry, answer delivered")


def test_a_400_is_not_retried(url):
    """A 4xx other than 429 means the request is wrong. Retrying cannot help; fail once."""
    global FAIL_FIRST
    requests.clear()
    FAIL_FIRST = [400]
    try:
        sql(url, """SELECT jev_choice('x', MAP{'a':'1','b':'2'}) AS intent;""")
        raise AssertionError("query should have failed on HTTP 400")
    except AssertionError as e:
        if "HTTP 400" not in str(e):
            raise
    statuses = [r.get("status", 200) for r in requests]
    assert statuses == [400], f"expected exactly one attempt, got {statuses}"
    print("PASS no_retry_400: one attempt, query failed with the status")


def test_score_and_noul_send_the_api_shape_and_return_doubles(url):
    """Score criteria is an ordered list; noul criteria is an object with true/false.
    Both verified against the served OpenAPI schema. Values from the recorded response.
    """
    requests.clear()
    rows = sql(
        url,
        """
        SELECT jev_score('the export crashes', ['trivial','minor','normal','serious','critical']) AS severity,
               jev_noul('the export crashes', MAP{'true':'needs a reply today','false':'can wait'}) AS p_urgent;""",
    )
    assert rows == [{"severity": 2.1, "p_urgent": 0.6}], rows
    kinds = {r["body"]["questions"]["q"]["type"]: r["body"]["questions"]["q"] for r in requests}
    assert kinds["score"]["criteria"] == [
        "trivial",
        "minor",
        "normal",
        "serious",
        "critical",
    ], kinds["score"]
    assert kinds["noul"]["criteria"] == {
        "true": "needs a reply today",
        "false": "can wait",
    }, kinds["noul"]
    print("PASS score_and_noul: rubric as list, true/false as object, doubles 2.1 and 0.6")


def test_on_error_null_turns_an_exhausted_row_into_null(url):
    """Default: a row that exhausts its retries fails the query. With
    SET jev_on_error = 'null' it becomes NULL and the other rows still arrive.
    The mock fails by state, so exactly one row exhausts its budget however the
    rows are scheduled."""
    global ALWAYS_FAIL
    requests.clear()
    ALWAYS_FAIL = {"a bug report": 503}
    try:
        try:
            sql(
                url,
                """SELECT jev_choice('a bug report', MAP{'bug':'b','praise':'p'}) AS intent;""",
            )
            raise AssertionError("default must fail the query after retries are exhausted")
        except AssertionError as e:
            if "HTTP 503" not in str(e):
                raise
        attempts = sum(1 for r in requests if r.get("status") == 503)
        assert attempts == 4, f"expected the retry budget of 4 attempts, got {attempts}"
        requests.clear()
        rows = sql(
            url,
            """
            SET jev_on_error = 'null';
            CREATE TABLE t AS SELECT * FROM (VALUES ('a bug report'), ('pure praise')) v(body);
            SELECT body, jev_choice(body, MAP{'bug':'b','praise':'p'}) AS intent FROM t ORDER BY body;""",
        )
    finally:
        ALWAYS_FAIL = {}
    assert rows == [
        {"body": "a bug report", "intent": None},
        {"body": "pure praise", "intent": "praise"},
    ], rows
    print("PASS on_error_null: exhausted row is NULL after 4 attempts, the other row is answered")


def test_on_error_rejects_unknown_modes(url):
    try:
        sql(url, """SET jev_on_error = 'shrug'; SELECT 1;""")
        raise AssertionError("an unknown mode must be rejected when set")
    except AssertionError as e:
        if "jev_on_error" not in str(e):
            raise
    print("PASS on_error_validation: unknown mode rejected at SET")


def test_ask_is_one_request_with_every_question(url):
    """Jev bills per state. Three questions about one row must be one POST carrying
    three named questions, and the struct fields come back typed."""
    requests.clear()
    rows = sql(
        url,
        """
        WITH asked AS (
            SELECT jev_ask('I want a refund for last month, the charge was wrong', {
                intent:   MAP{'refund':'wants money back','bug':'something broken','praise':'a compliment'},
                severity: ['trivial','minor','normal','serious','critical'],
                urgent:   MAP{'true':'needs a reply today','false':'can wait'}
            }) AS a)
        SELECT a.intent, a.intent_confidence, a.severity, a.severity_confidence, a.urgent FROM asked;""",
    )
    assert len(requests) == 1, f"three questions must be one request, got {len(requests)}"
    qs = requests[0]["body"]["questions"]
    assert set(qs) == {"intent", "severity", "urgent"}, qs
    assert qs["intent"]["type"] == "choice" and qs["severity"]["type"] == "score" and qs["urgent"]["type"] == "noul", qs
    assert qs["severity"]["criteria"] == [
        "trivial",
        "minor",
        "normal",
        "serious",
        "critical",
    ], qs["severity"]
    a = rows[0]
    assert a == {
        "intent": "refund",
        "intent_confidence": 1.0,
        "severity": 2.1,
        "severity_confidence": 0.9,
        "urgent": 0.6,
    }, a
    print("PASS ask_one_request: 3 questions, 1 POST, typed struct from the recorded answers")


def test_usage_reports_requests_and_tokens(url):
    """A careless query over a large table is a large bill with no warning. jev_usage()
    reports what this process has spent: requests made, answers served from cache,
    and the tokens the API charged, summed from every response's usage block."""
    requests.clear()
    rows = sql(
        url,
        """
        CREATE TABLE t AS SELECT * FROM (VALUES ('a refund'), ('a bug'), ('praise')) v(body);
        CREATE TABLE scored AS
            SELECT body, jev_choice(body, MAP{'refund':'r','bug':'b','praise':'p'}) AS intent FROM t;
        -- the same three states again: served from cache, no new tokens
        CREATE TABLE again AS
            SELECT body, jev_choice(body, MAP{'refund':'r','bug':'b','praise':'p'}) AS intent FROM t;
        SELECT requests, cache_hits, input_tokens, output_tokens FROM jev_usage();""",
    )
    # every recorded response carries usage {input_tokens: 412, output_tokens: 69}
    assert rows == [
        {
            "requests": 3,
            "cache_hits": 3,
            "input_tokens": 3 * 412,
            "output_tokens": 3 * 69,
        }
    ], rows
    print("PASS usage: 3 requests, 3 cache hits, 1236 input and 207 output tokens")


class MockServer(ThreadingHTTPServer):
    # socketserver defaults to a backlog of 5. The extension opens up to 16
    # connections at once, so on a slow machine the rest are refused before the
    # accept loop drains. That is a property of this mock, not of the extension.
    request_queue_size = 64
    daemon_threads = True
    allow_reuse_address = True


def main():
    srv = MockServer(("127.0.0.1", 0), Mock)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{srv.server_port}"
    failures = 0
    for test in (
        test_one_post_per_row,
        test_same_call_in_where_and_select_is_one_request_per_row,
        test_rows_in_a_chunk_are_requested_concurrently,
        test_a_429_is_retried_with_backoff,
        test_a_dropped_connection_is_retried,
        test_a_400_is_not_retried,
        test_score_and_noul_send_the_api_shape_and_return_doubles,
        test_on_error_null_turns_an_exhausted_row_into_null,
        test_on_error_rejects_unknown_modes,
        test_ask_is_one_request_with_every_question,
        test_usage_reports_requests_and_tokens,
    ):
        try:
            test(url)
        except AssertionError as e:
            failures += 1
            print(f"FAIL {test.__name__}: {e}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
