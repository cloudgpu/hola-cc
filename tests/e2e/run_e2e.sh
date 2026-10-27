#!/usr/bin/env bash
# End-to-end: compiled agent programs against a fake model server.
#
# Each program is compiled with a profiles.json whose default profile points
# at the fake server, then run in a clean environment: an empty HOME, no
# OPENAI_* variables, no plugin directory. Only HOLA_API_KEY is given. That
# is the self-contained claim: the binary carries its agents, prompts, skills,
# tools and model settings, and reads nothing but the key when it runs.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
holacc="$root/build/holacc"
work="$(mktemp -d)"; [ -n "${KEEP:-}" ] && echo "work: $work"
server=0
trap '[ "$server" = 0 ] || kill "$server" 2>/dev/null || true; [ -n "${KEEP:-}" ] || rm -rf "$work"' EXIT
fail=0
mkdir -p "$work/build-home/.hola" "$work/run-home" "$work/run"
printf 'one\ntwo\nthree\n' > "$work/run/sample.txt"

start_server() {   # script.json log.jsonl
    [ "$server" = 0 ] || kill "$server" 2>/dev/null || true
    port=$(( 20000 + RANDOM % 20000 ))
    python3 "$here/fake_llm.py" "$port" "$1" "$2" &
    server=$!
    for _ in $(seq 50); do curl -s "http://127.0.0.1:$port/v1/models" >/dev/null 2>&1 && break; sleep 0.1; done
    cat > "$work/build-home/.hola/profiles.json" <<JSON
{"default": "fake", "profiles": {"fake": {"model": "fake-model", "base_url": "http://127.0.0.1:$port/v1",
 "api_key": "sk-compile-time-secret", "max_tokens": 1000}}}
JSON
}

build() {   # src out
    (cd "$work" && HOME="$work/build-home" "$holacc" build "$1" -o "$2")
}

run_clean() {   # binary args...
    (cd "$work/run" && env -i PATH="$PATH" HOME="$work/run-home" TMPDIR="$work" HOLA_API_KEY=run-time-key "$@")
}

# ---- 1. tools written in the language, typed output with a repair, ask() ----
sed "s|@FILE@|$work/run/sample.txt|" "$here/review.script.json" > "$work/script.json"
start_server "$work/script.json" "$work/log.jsonl"
build "$here/review.hl" "$work/review"
out="$(run_clean "$work/review" "$work/run/sample.txt")"
expected="verdict: reject
lines: 3
finding: a.go 7 unused variable
summary: hi there"
if [ "$out" != "$expected" ]; then
    echo "FAIL review: output differs"; diff <(echo "$expected") <(echo "$out") || true; fail=1
fi
python3 - "$work/log.jsonl" <<'PY' || fail=1
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
first = reqs[0]
names = [t["function"]["name"] for t in first.get("tools", [])]
user = [m for m in first["messages"] if m["role"] == "user"][0]["content"]
system = first["messages"][0]["content"]
tool_msgs = lambda r: [m.get("content") or "" for m in r["messages"] if m.get("role") == "tool"]
checks = {
    "count_lines offered": "count_lines" in names,
    "submit_result offered": "submit_result" in names,
    "no plugin tools": not any(n in ("read_file", "write_file", "run_command") for n in names),
    "task filled": user.startswith("Review ") and "+ x := 1" in user,
    "persona in system prompt": "You review code changes strictly." in system,
    "language skill in system prompt": "## Skill: file_facts" in system and "measure a file" in system,
    "model from the embedded profile": first.get("model") == "fake-model",
    "run-time key sent": all(r.get("_auth") == "Bearer run-time-key" for r in reqs),
    "tool result returned 3": "3" in tool_msgs(reqs[1]),
    "invalid result rejected": any("lines" in t and "integer" in t for t in tool_msgs(reqs[2])),
}
bad = [k for k, v in checks.items() if not v]
for k in bad:
    print("FAIL review:", k)
sys.exit(1 if bad else 0)
PY
if grep -aq "sk-compile-time-secret" "$work/review"; then echo "FAIL: the API key was compiled into the binary"; fail=1; fi
for secret in "You review code changes strictly." "measure a file" "Review {{path}}" "count_lines"; do
    if grep -aqF "$secret" "$work/review"; then echo "FAIL: '$secret' is readable in the binary"; fail=1; fi
done
if "$work/review" --hola-agents | grep -qF "strictly"; then echo "FAIL: --hola-agents shows the persona"; fail=1; fi
if ldd "$work/review" | grep -qi hola; then echo "FAIL: the binary links a Hola shared library"; ldd "$work/review"; fail=1; fi
"$work/review" --hola-agents | python3 -c 'import json,sys; a=json.load(sys.stdin); assert [x["name"] for x in a] == ["Reviewer","Summarizer"], a' \
    || { echo "FAIL --hola-agents"; fail=1; }

# ---- 2. Hola's own tools (linked in) through `extends reader`, with `deny` ----
: > "$work/log2.jsonl"
start_server "$here/plugins.script.json" "$work/log2.jsonl"
build "$here/plugins.hl" "$work/plugins"
out="$(run_clean "$work/plugins")"
[ "$out" = "first: one" ] || { echo "FAIL plugins: got '$out'"; fail=1; }
python3 - "$work/log2.jsonl" <<'PY' || fail=1
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
names = [t["function"]["name"] for t in reqs[0].get("tools", [])]
tool_out = [m.get("content") or "" for m in reqs[1]["messages"] if m.get("role") == "tool"]
system = reqs[0]["messages"][0]["content"]
checks = {
    "read_file offered": "read_file" in names,
    "denied search_code absent": "search_code" not in names,
    "edit tools absent": "write_file" not in names,
    "read_file ran": any("one" in t and "two" in t for t in tool_out),
    "embedded skill in prompt": "Answer in one short sentence." in system,
}
bad = [k for k, v in checks.items() if not v]
for k in bad:
    print("FAIL plugins:", k, names)
sys.exit(1 if bad else 0)
PY

# ---- 3. a program without main is its agent: arguments in, JSON result out ----
: > "$work/log3.jsonl"
start_server "$here/plugins.script.json" "$work/log3.jsonl"
build "$here/cli.hl" "$work/cli"
out="$(run_clean "$work/cli" What is the first line of sample.txt)"
[ "$out" = '{"first_line":"one"}' ] || { echo "FAIL cli: got '$out'"; fail=1; }
python3 -c 'import json,sys; r=json.loads(open(sys.argv[1]).readline()); u=[m for m in r["messages"] if m["role"]=="user"][0]["content"]; sys.exit(0 if u=="What is the first line of sample.txt" else 1)' "$work/log3.jsonl" \
    || { echo "FAIL cli: arguments were not the prompt"; fail=1; }

# ---- 4. declarative tools: sh (quoted), write (confined), agent as a tool ----
: > "$work/log4.jsonl"
start_server "$here/bind.script.json" "$work/log4.jsonl"
build "$here/bind.hl" "$work/bind"
mkdir -p "$work/run/inner"
out="$(cd "$work/run/inner" && env -i PATH="$PATH" HOME="$work/run-home" TMPDIR="$work" HOLA_API_KEY=k "$work/bind" Binder do the job)"
[ "$out" = '{"result":"fine"}' ] || { echo "FAIL bind: got '$out'"; fail=1; }
[ ! -e "$work/run/inner/pwned" ] || { echo "FAIL bind: shell injection ran"; fail=1; }
[ ! -e "$work/run/escape.txt" ] || { echo "FAIL bind: write escaped the directory"; fail=1; }
[ "$(cat "$work/run/inner/ok.txt" 2>/dev/null)" = "data" ] || { echo "FAIL bind: write did not write ok.txt"; fail=1; }
python3 - "$work/log4.jsonl" <<'PY' || fail=1
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
tools = [m.get("content") or "" for m in reqs[-1]["messages"] if m.get("role") == "tool"]
checks = {
    "shout quoted": any("HI'; TOUCH PWNED; ECHO '" in t for t in tools),
    "escape refused": any("must be relative" in t for t in tools),
    "write reported": any("wrote 4 bytes" in t for t in tools),
    "agent tool returned": any(t == "helper says hi" for t in tools),
    "helper got its input": any(m.get("role") == "user" and m.get("content") == "say hi" for r in reqs for m in r["messages"]),
}
bad = [k for k, v in checks.items() if not v]
for k in bad:
    print("FAIL bind:", k, tools)
sys.exit(1 if bad else 0)
PY

[ "$fail" = 0 ] && echo "e2e: ok"
exit "$fail"
