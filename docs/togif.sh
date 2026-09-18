#!/bin/zsh
# asciinema cast -> a GIF small enough for a GitHub README.
# Idle gaps are trimmed, frame rate capped, and the whole thing sped up slightly:
# nobody wants to watch real typing speed.
S=/private/tmp/claude-501/-Users-joost-Development-mono/a2c4f35d-f11f-4e45-8f4c-6995835e9506/scratchpad
OUT=${1:-$S/demo.gif}
agg --idle-time-limit 1.2 \
    --speed 1.35 \
    --fps-cap 12 \
    --font-size 15 \
    --theme asciinema \
    --last-frame-duration 4 \
    "$S/demo.cast" "$OUT"
ls -la "$OUT" | awk '{printf "  %.1f MB  %s\n", $5/1048576, $9}'
