"""Run the demo queries for real and capture each output.

Everything that can share a session does, because the answer cache and the usage
counters are process-wide: the last two steps only mean anything that way. A
statement meant to fail aborts a batch script, so those run alone and are spliced
back into place.
"""
import json, os, subprocess, sys, tempfile
from steps import STEPS

ROOT = "/Users/joost/Development/duckdb-jev"
DUCKDB = f"{ROOT}/build/release/duckdb"
MARK = "@@@STEP@@@"

PRELUDE = [
    "LOAD 'build/release/extension/jev/jev.duckdb_extension';",
    f"CREATE SECRET (TYPE jev, API_KEY '{os.environ['TYPESAFE_API_KEY']}');",
    """CREATE TABLE tickets AS SELECT * FROM (VALUES
  (1, 'I want a refund for last month, the charge was wrong'),
  (2, 'The export button crashes on files over 100MB'),
  (3, 'Great product, the new dashboard is lovely'),
  (4, 'Charged twice this month, please reverse one'),
  (5, 'Login loops forever on Safari, cannot get in')) t(id, body);""",
]

def fails(step):
    return len(step) > 2 and step[2]

def run(script):
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
        f.write(script + "\n.quit\n"); path = f.name
    try:
        r = subprocess.run([DUCKDB, "-unsigned", "-dark-mode", "-batch", "-init", path],
                           cwd=ROOT, capture_output=True, text=True,
                           timeout=240, stdin=subprocess.DEVNULL)
        return r.stdout + r.stderr
    finally:
        os.unlink(path)

# one session for everything that succeeds
lines = list(PRELUDE)
for step in STEPS:
    if not fails(step):
        lines += [f".print {MARK}", step[0]]
lines.append(f".print {MARK}")
combined = run("\n".join(lines))
chunks = combined.split(MARK)[1:]

ok_steps = [s for s in STEPS if not fails(s)]
if len(chunks) < len(ok_steps):
    print(f"CAPTURE FAILED: {len(chunks)} chunks for {len(ok_steps)} steps", file=sys.stderr)
    print(combined[:1200], file=sys.stderr)
    sys.exit(1)

ok = [c.strip("\n") for c in chunks[:len(ok_steps)]]
out, i = [], 0
for step in STEPS:
    if fails(step):
        raw = run("\n".join(PRELUDE) + "\n" + step[0])
        # keep only the error itself: the prelude's output and the CLI's
        # "Encountered errors while executing init file" are noise here
        err = [l for l in raw.splitlines()
               if ("Error:" in l and "Encountered errors" not in l)]
        out.append("\n".join(err) if err else raw.strip("\n"))
    else:
        out.append(ok[i]); i += 1

json.dump(out, open("outputs.json", "w"))
for n, o in enumerate(out, 1):
    head = next((l for l in o.splitlines() if l.strip()), "(empty)")
    print(f"  step {n}: {len(o.splitlines()):2d} lines  {head[:58]}")
