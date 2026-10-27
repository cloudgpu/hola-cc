#!/usr/bin/env bash
# Language tests: each tests/lang/*.hl either runs and must print its .out
# file, or starts with `// expect: <text>` and must fail to compile with
# that text in the error.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
holacc="$here/../build/holacc"
pass=0; fail=0
for src in "$here"/lang/*.hl; do
    name="$(basename "$src" .hl)"
    expect="$(head -1 "$src" | sed -n 's|^// expect: ||p')"
    if [ -n "$expect" ]; then
        err="$("$holacc" check "$src" 2>&1)"; rc=$?
        if [ $rc -ne 0 ] && grep -qF -- "$expect" <<<"$err"; then pass=$((pass+1));
        else echo "FAIL $name: expected an error containing '$expect', got (rc=$rc):"; echo "$err"; fail=$((fail+1)); fi
        continue
    fi
    [ -f "$here/lang/$name.out" ] || { echo "SKIP $name (no .out)"; continue; }
    out="$("$holacc" run "$src" 2>&1)"
    if [ "$out" == "$(cat "$here/lang/$name.out")" ]; then pass=$((pass+1));
    else echo "FAIL $name:"; diff <(cat "$here/lang/$name.out") <(echo "$out"); fail=$((fail+1)); fi
done
echo "lang: $pass passed, $fail failed"
[ $fail -eq 0 ]
