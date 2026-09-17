"""Integration test at the SQL seam: jev_choice makes one POST per row to the
endpoint named in the jev secret, and maps the answer onto the ENUM.

The mock replays a real response recorded from api.typesafe.ai on 2026-09-17,
so the expected values come from the actual service, not from this test.
"""
import json, os, subprocess, sys, threading
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

class Mock(BaseHTTPRequestHandler):
    def do_POST(self):
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


def main():
    srv = HTTPServer(("127.0.0.1", 0), Mock)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{srv.server_port}"
    failures = 0
    for test in (test_one_post_per_row, test_same_call_in_where_and_select_is_one_request_per_row):
        try:
            test(url)
        except AssertionError as e:
            failures += 1
            print(f"FAIL {test.__name__}: {e}")
    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()
