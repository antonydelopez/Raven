#!/bin/sh
# A change that breaks Raven's build is refused by the commit gate
# (odin:kit-overlay#raven-fork-gate-crit-refuses-broken).
#
# In a throwaway worktree of the fork, one compiled source file gets a line the
# compiler rejects; `aidx commit --go --no-record` there must exit non-zero and
# its refusal must say the build check (the `cmake` line of .committer.toml)
# failed ON THE BUILDER: a gate that could not reach the builder (shk exit 78,
# "NOTHING RAN") also refuses, but proves nothing about the build; and the
# compiler's own message of the injected line must be in the output, so a gate
# that refuses every tree does not pass.
# The worktree is removed on every exit. Run from anywhere inside the fork.
set -u

root=$(git rev-parse --show-toplevel) || exit 2
test -f "$root/.committer.toml" || { echo "no .committer.toml in $root" >&2; exit 2; }

wt=$(mktemp -d "${TMPDIR:-/tmp}/raven-gate-broken.XXXXXX") || exit 2
out="$wt.out"
br="gate-broken-$$"	# this run's pid: the branch is deleted only once this run made it
made=0
cleanup() {
	git -C "$root" worktree remove --force "$wt" >/dev/null 2>&1
	if [ "$made" -eq 1 ]; then git -C "$root" branch -D "$br" >/dev/null 2>&1; fi
	rm -rf "$wt" "$out"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

git -C "$root" worktree add -b "$br" "$wt" HEAD >/dev/null 2>&1 || { echo "cannot make the throwaway worktree" >&2; exit 2; }
made=1
# the gate under test may not be committed yet: it travels with the tree
cp "$root/.committer.toml" "$wt/.committer.toml"
printf '\n#error "gate-refuses-broken-build: this line is meant to break the build"\n' >> "$wt/src/core/error.c"

aidx commit --repo "$wt" --go --no-record >"$out" 2>&1
code=$?
cat "$out"

if [ "$code" -eq 0 ]; then
	echo "FAIL: the gate let a broken build through" >&2
	exit 1
fi
if ! grep -q 'check failed on the builder : cmake' "$out"; then
	echo "FAIL: the refusal (exit $code) does not name the build check" >&2
	exit 1
fi
if ! grep -q 'gate-refuses-broken-build: this line is meant to break the build' "$out"; then
	echo "FAIL: the build check failed, but not on the injected line" >&2
	exit 1
fi
echo "ok: the broken build was refused (exit $code), naming the build check"
