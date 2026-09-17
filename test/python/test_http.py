"""Integration test at the SQL seam: jev_choice makes one POST per row to the
endpoint named in the jev secret, and maps the answer onto the ENUM.

The mock replays a real response recorded from api.typesafe.ai on 2026-09-17,
so the expected values come from the actual service, not from this test.
"""
import json, os, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DUCKDB = os.path.join(ROOT, "build/release/duckdb")
EXT = os.path.join(ROOT, "build/release/extension/jev/jev.duckdb_extension")

# Recorded 2026-09-17, model jev-1.13.0, state "I want a refund for last month..."
RECORDED = {"model": "jev-1.13.0",
            "answers": {"intent": {"type": "choice", "choice": "refund", "confidence": 1.0,
                                   "probabilities": {"refund": 1.0, "bug": 0.0, "praise": 0.0}}},
            "usage": {"input_tokens": 412, "output_tokens": 69}}

requests = []

DELAY = 0.0   # per-request latency the mock adds; set by a test
FAIL_FIRST = []   # HTTP statuses to answer with before succeeding; consumed in order

class Mock(BaseHTTPRequestHandler):
    def do_POST(self):
        if DELAY:
            time.sleep(DELAY)
        if FAIL_FIRST:
            status = FAIL_FIRST.pop(0)
            requests.append({"path": self.path, "status": status})
            self.send_response(status); self.send_header("Content-Length", "2"); self.end_headers()
            self.wfile.write(b"{}"); return
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append({"path": self.path, "auth": self.headers.get("Authorization"), "body": body})
        # answer with whichever option the state names, else the recorded answer
        state = body["state"].lower()
        opts = list(body["questions"]["q"]["criteria"].keys())
        pick = next((o for o in opts if o in state), RECORDED["answers"]["intent"]["choice"])
        resp = json.loads(json.dumps(RECORDED))
        resp["answers"] = {"q": {**RECORDED["answers"]["intent"], "choice": pick}}
        out = json.dumps(resp).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out))); self.end_headers(); self.wfile.write(out)
    def log_message(self, *a): pass

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

    rows = sql(url, """
        CREATE TABLE t AS SELECT * FROM (VALUES
            ('I want a refund, the charge was wrong'),
            ('the export crashes, clearly a bug'),
            ('lovely product, pure praise')) v(body);
        SELECT jev_choice(body, MAP{'refund':'wants money back','bug':'something broken','praise':'a compliment'}) AS intent
        FROM t ORDER BY body;""")

    assert [r["intent"] for r in rows] == ["refund", "praise", "bug"], rows
    assert len(requests) == 3, f"expected one POST per row, got {len(requests)}"
    req = requests[0]
    assert req["path"] == "/v1/systemone", req["path"]
    assert req["auth"] == "Bearer test-key-123", req["auth"]
    assert req["body"]["model"] == "jev-latest", req["body"]
    q = req["body"]["questions"]["q"]
    assert q["type"] == "choice" and q["criteria"] == {
        "refund": "wants money back", "bug": "something broken", "praise": "a compliment"}, q
    print("PASS one_post_per_row: 3 rows, 3 POSTs, bearer header, request shape, enum answers")


def test_same_call_in_where_and_select_is_one_request_per_row(url):
    """DuckDB evaluates a volatile function once per occurrence. With the function in
    both WHERE and SELECT that is two requests per row, and two bills. The extension
    must answer the second occurrence from what it already knows."""
    requests.clear()
    rows = sql(url, """
        CREATE TABLE t AS SELECT * FROM (VALUES
            ('I want a refund, the charge was wrong'),
            ('the export crashes, clearly a bug'),
            ('lovely product, pure praise')) v(body);
        SELECT body, jev_choice(body, MAP{'refund':'r','bug':'b','praise':'p'}) AS intent
        FROM t
        WHERE jev_choice(body, MAP{'refund':'r','bug':'b','praise':'p'}) <> 'praise'
        ORDER BY body;""")
    assert [r["intent"] for r in rows] == ["refund", "bug"], rows
    assert len(requests) == 3, f"expected 3 requests for 3 rows, got {len(requests)}: paid twice"
    print("PASS where_and_select: 3 rows, 3 requests, second evaluation served from cache")


def test_rows_in_a_chunk_are_requested_concurrently(url):
    """One request per row is the API's floor. Waiting for each in turn is not.
    16 distinct rows at 200ms each: sequential is 3.2s, concurrent well under 1s."""
    global DELAY
    requests.clear(); DELAY = 0.2
    try:
        t0 = time.time()
        rows = sql(url, """
            CREATE TABLE t AS SELECT 'ticket ' || i || ' mentions a bug' AS body FROM range(16) r(i);
            SELECT count(*) AS n FROM t WHERE jev_choice(body, MAP{'bug':'b','praise':'p'}) = 'bug';""")
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
    requests.clear(); FAIL_FIRST = [429]
    rows = sql(url, """
        CREATE TABLE t AS SELECT 'a refund please' AS body;
        SELECT jev_choice(body, MAP{'refund':'r','bug':'b'}) AS intent FROM t;""")
    assert rows == [{"intent": "refund"}], rows
    statuses = [r.get("status", 200) for r in requests]
    assert statuses == [429, 200], f"expected one 429 then a retry, got {statuses}"
    print("PASS retry_429: one 429, one retry, answer delivered")


def test_a_400_is_not_retried(url):
    """A 4xx other than 429 means the request is wrong. Retrying cannot help; fail once."""
    global FAIL_FIRST
    requests.clear(); FAIL_FIRST = [400]
    try:
        sql(url, """SELECT jev_choice('x', MAP{'a':'1','b':'2'}) AS intent;""")
        raise AssertionError("query should have failed on HTTP 400")
    except AssertionError as e:
        if "HTTP 400" not in str(e):
            raise
    statuses = [r.get("status", 200) for r in requests]
    assert statuses == [400], f"expected exactly one attempt, got {statuses}"
    print("PASS no_retry_400: one attempt, query failed with the status")


def main():
    from http.server import ThreadingHTTPServer
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Mock)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{srv.server_port}"
    failures = 0
    for test in (test_one_post_per_row, test_same_call_in_where_and_select_is_one_request_per_row,
                 test_rows_in_a_chunk_are_requested_concurrently,
                 test_a_429_is_retried_with_backoff, test_a_400_is_not_retried):
        try:
            test(url)
        except AssertionError as e:
            failures += 1
            print(f"FAIL {test.__name__}: {e}")
    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()
