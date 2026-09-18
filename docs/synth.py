"""Build an asciinema cast from real captured output.

No pseudo-terminals, no interactive REPL: driving the DuckDB CLI through nested
ptys stalled five seconds per keystroke on its terminal-colour probe. Here every
frame is written deliberately, so the pacing is chosen rather than whatever the
terminal happened to do. Everything shown below a prompt is output the extension
actually produced against the live API.

The SQL is coloured with Pygments, the same highlighter editors use, so keywords,
strings and functions read differently. Table borders are dimmed and headers
brightened so the eye lands on the data rather than the box.
"""

import json
import re
import time

from pygments import highlight
from pygments.formatters import Terminal256Formatter
from pygments.lexers import SqlLexer

from steps import STEPS

ESC = chr(27)
W, H = 104, 34
CPS = 55.0  # typing speed, characters per second
LINE_GAP = 0.10  # pause between the lines of one statement
THINK = 0.45  # after Enter, before the answer appears

PROMPT = f"{ESC}[1;36mD {ESC}[0m"
CONT = f"{ESC}[1;36m  {ESC}[0m"
DIM = f"{ESC}[38;5;240m"  # table borders, and the comment at the top
HEAD = f"{ESC}[1;38;5;252m"  # column names
TYPE = f"{ESC}[38;5;245m"  # the type row under them
RED = f"{ESC}[1;38;5;203m"
OFF = f"{ESC}[0m"

ANSI = re.compile(re.escape(ESC) + r"\[[0-9;]*m")
BORDER = re.compile(r"^[┌├└─┬┼┴┐┤┘\s]+$")
LEXER = SqlLexer()
FORMATTER = Terminal256Formatter(style="monokai")

events = []
clock = 0.0


def emit(data, dt=0.0):
    global clock
    clock += dt
    events.append([round(clock, 3), "o", data])


def colour_sql(text):
    """Highlight, then drop the trailing newline Pygments adds."""
    return highlight(text, LEXER, FORMATTER).rstrip("\n")


def type_line(line):
    """Type one already-coloured line: escapes land instantly, characters do not."""
    pos = 0
    for m in ANSI.finditer(line):
        for ch in line[pos:m.start()]:
            emit(ch, 1.0 / CPS)
        emit(m.group(0))
        pos = m.end()
    for ch in line[pos:]:
        emit(ch, 1.0 / CPS)


def type_sql(text):
    coloured = colour_sql(text).split("\n")
    for i, line in enumerate(coloured):
        emit(PROMPT if i == 0 else CONT)
        type_line(line)
        emit(f"{OFF}\r\n", LINE_GAP)


def paint_table(body):
    """Dim the box, brighten the header, so the data is what you look at."""
    lines = body.split("\n")
    content = 0
    painted = []
    for line in lines:
        if BORDER.match(line):
            painted.append(DIM + line + OFF)
            continue
        if line.startswith("│"):
            content += 1
            style = HEAD if content == 1 else TYPE if content == 2 else ""
            # keep the vertical rules dim even inside a content row
            cells = line.split("│")
            joined = (DIM + "│" + OFF).join(style + c + OFF if style else c for c in cells)
            painted.append(joined)
            continue
        painted.append(line)
    return "\n".join(painted)


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
        painted = RED + body + OFF if "Error:" in body else paint_table(body)
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
