#!/bin/sh
# The per-timeline marker option (odin:kit-overlay#raven-timeline-option):
#   default       a timeline that leaves the option alone drags a marker and
#                 deletes it with Delete
#   per-timeline  two timelines in one window, the option on for one: it does
#                 neither, the other still does both
# Each is a function of tests/test_tools.c driven by injected input; this builds
# test_tools (in build/gate, as the gate does) and requires that function to have
# RUN and the whole binary to exit 0. Run from anywhere inside the fork.
set -u
case "${1:-}" in
	default) fn=test_timeline_markers_default ;;
	per-timeline) fn=test_timeline_markers_per_timeline ;;
	*) echo "usage: $0 default|per-timeline" >&2; exit 2 ;;
esac
root=$(git rev-parse --show-toplevel) || exit 2
cd "$root" || exit 2
cmake -S . -B build/gate -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 2
cmake --build build/gate --target test_tools >/dev/null || exit 2
out=$(mktemp "${TMPDIR:-/tmp}/marker-option.XXXXXX") || exit 2
trap 'rm -f "$out"' EXIT
(cd build/gate && bin/test_tools) >"$out" 2>&1
code=$?
if [ "$code" -ge 128 ]; then echo "test_tools died with signal exit $code" >&2; exit 1; fi
# this probe's own result: its function ran, and no check failed inside it
# (test_tools prints each test's name, then its FAIL lines, before the next name)
if ! grep -qx "$fn" "$out"; then echo "$fn did not run" >&2; tail -20 "$out" >&2; exit 1; fi
awk -v fn="$fn" '$0 == fn { on = 1; next } /^test_/ { on = 0 } on && /FAIL/ { bad = 1; print > "/dev/stderr" } END { exit bad }' "$out" || { echo "$fn failed" >&2; exit 1; }
echo "$fn: ran, no check failed in it (test_tools exit $code)"
