# The demo GIF

`demo.gif` is built from real output, not a mock-up. Every table in it came from
the extension talking to `api.typesafe.ai`.

```sh
export TYPESAFE_API_KEY=...
python3 capture.py   # runs the queries, writes outputs.json
python3 synth.py     # builds demo.cast from those outputs
./togif.sh           # agg: demo.cast -> demo.gif
```

`steps.py` is the script of the demo: the SQL shown, how long to hold each result,
and whether a step is meant to fail.

Everything that can share one DuckDB session does, because the answer cache and the
usage counters are process-wide. That is what makes the last two steps meaningful:
the same question asked again adds cache hits and no tokens. A statement meant to
fail aborts a batch script, so those run in their own session and are spliced back
into place.

The cast is synthesised rather than screen-recorded. Driving the DuckDB CLI through
a pseudo-terminal stalls about five seconds per keystroke, because the client probes
the terminal for its background colour and the probe never answers. `-dark-mode`
avoids it, but building the frames directly also makes the pacing a choice rather
than an accident.
