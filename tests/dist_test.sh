#!/usr/bin/env bash
# The toolchain tarball, as a user gets it: unpack, install to a fresh prefix,
# and build programs with no HOLACC_* / HOLA_ADK_DIR in the environment.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
tarball="$(ls -t "$here"/dist/hola-agent-toolchain-*.tar.gz | head -1)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tar -C "$work" -xzf "$tarball"
"$work"/hola-agent-toolchain-*/install.sh "$work/prefix" > "$work/install.log"
holacc="$work/prefix/bin/holacc"
clean() { env -u HOLACC_RUNTIME_DIR -u HOLA_ADK_DIR "$@"; }
fail=0
clean "$holacc" doctor > "$work/doctor.txt" || { cat "$work/doctor.txt"; fail=1; }
grep -q "$work/prefix/lib/holacc" "$work/doctor.txt" || { echo "FAIL dist: runtime not found beside holacc"; cat "$work/doctor.txt"; fail=1; }
grep -q "$work/prefix/lib/hola-adk" "$work/doctor.txt" || { echo "FAIL dist: ADK not found beside holacc"; fail=1; }
out="$(clean "$holacc" run "$here/tests/lang/structs.hl")"
[ "$out" = "$(cat "$here/tests/lang/structs.out")" ] || { echo "FAIL dist: structs.hl"; fail=1; }
cp -R "$work/prefix/share/hola-agent-toolchain/examples" "$work/examples"
(cd "$work" && HOME="$work" clean "$holacc" build examples/btc_monitor.hl -o btc) || { echo "FAIL dist: build agent example"; fail=1; }
"$work/btc" --hola-agents | grep -q '"name":.*"BtcMonitor"' || { echo "FAIL dist: agent binary"; fail=1; }
[ "$fail" = 0 ] && echo "dist: ok"
exit "$fail"
