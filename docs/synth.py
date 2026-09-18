"""Build an asciinema cast from real captured output.

No pseudo-terminals, no interactive REPL: driving the DuckDB CLI through nested
ptys stalled five seconds per keystroke on its terminal-colour probe. Here every
frame is written deliberately, so the pacing is chosen rather than whatever the
terminal happened to do. Everything shown below a prompt is output the extension
actually produced against the live API.
"""

import json
import re
import time

from steps import STEPS

ESC = chr(27)
W, H = 104, 34
CPS = 55.0  # typing speed, characters per second
LINE_GAP = 0.10  # pause between the lines of one statement
THINK = 0.45  # after Enter, before the answer appears

PROMPT = f"{ESC}[1;36mD {ESC}[0m"
CONT = f"{ESC}[1;36m  {ESC}[0m"
DIM = f"{ESC}[90m"
RED = f"{ESC}[31m"
OFF = f"{ESC}[0m"
ANSI = re.compile(re.escape(ESC) + r"\[[0-9;]*m")

events = []
clock = 0.0


def emit(data, dt=0.0):
    global clock
    clock += dt
    events.append([round(clock, 3), "o", data])


def type_sql(text):
    for i, line in enumerate(text.split("\n")):
        emit(PROMPT if i == 0 else CONT)
        for ch in line:
            emit(ch, 1.0 / CPS)
        emit("\r\n", LINE_GAP)


def main():
    global clock
    emit(f"{ESC}[2J{ESC}[H")
    emit(f"{DIM}-- duckdb-jev: ask a question about every row, get a real SQL type back{OFF}\r\n", 0.6)
    emit("\r\n", 0.5)

    answers = json.load(open("outputs.json"))
    for step, answer in zip(STEPS, answers):
        sql, hold = step[0], step[1]
        type_sql(sql)
        body = ANSI.sub("", answer).strip("\n")
        failed = "Error:" in body
        painted = (RED + body + OFF) if failed else body
        emit(painted.replace("\n", "\r\n") + "\r\n\r\n", THINK)
        clock += hold

    emit(f"{PROMPT}{DIM}.quit{OFF}\r\n", 0.4)
    clock += 1.0

    header = {
        "version": 2,
        "width": W,
        "height": H,
        "timestamp": int(time.time()),
        "env": {"TERM": "xterm-256color", "SHELL": "/bin/zsh"},
    }
    with open("demo.cast", "w") as f:
        f.write(json.dumps(header) + "\n")
        for e in events:
            f.write(json.dumps(e) + "\n")
    print(f"  cast: {len(events)} events, {clock:.1f}s")


main()
