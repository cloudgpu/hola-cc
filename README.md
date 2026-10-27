# hola-cc

hola-cc is the compiler and runtime for the Hola agent language. The compiler,
`holacc`, translates a program that declares agents, skills and tools into a
native executable. The executable contains Hola's agent loop. When an agent
runs, the language model chooses which of the agent's tools to call; the program
does not describe that sequence.

The language is described in [doc/language.md](doc/language.md). This file
covers building, installing and using the toolchain.

Version 0.1.0. Supported platform: x86_64 GNU/Linux.

## Example

```go
// Reads the BTC price and reports moves.
skill market_watch {
    instructions "Read prices only with btc_price. Wait between checks."

    // The BTC/USD spot price from Coinbase, as JSON.
    tool btc_price() = http "https://api.coinbase.com/v2/prices/BTC-USD/spot"
    // Waits the given number of seconds.
    tool wait(seconds int) = sh "sleep {{seconds}}"
    // Shows an alert to the user.
    tool alert(message string) = print "ALERT {{message}}"
}

// Watches the BTC price.
agent BtcMonitor {
    persona "You monitor a market price. You do not give trading advice."
    goal "Check the price `checks` times, `interval_seconds` apart, and alert on moves of `alert_percent` or more."
    skills [market_watch]
    input {
        checks int
        interval_seconds int
        alert_percent float
    }
    output {
        alerts int
        summary string
    }
}
```

```
$ holacc build btc_monitor.hl
$ ./btc_monitor checks=3 interval_seconds=60 alert_percent=0.5
{"alerts":0,"summary":"..."}
```

The complete example is `examples/btc_monitor.hl`.

## Requirements

To build hola-cc:

- a C11 compiler, `make`, `flex` and `bison`;
- the Hola ADK, to build the agent runtime (see "The Hola ADK" below).

To compile programs with `holacc`:

- `clang`, LLVM 15 or later;
- the libcurl and SQLite 3 development libraries.

On Debian and Ubuntu:

```
apt install clang libcurl4-openssl-dev libsqlite3-dev
```

On Arch Linux:

```
pacman -S clang curl sqlite
```

`holacc doctor` reports which of these are missing.

## Building

```
make HOLA_ADK_DIR=/path/to/hola-adk
```

This builds `build/holacc` and the runtime libraries. Without `HOLA_ADK_DIR`,
`make` builds the compiler and the core runtime only; such a build compiles
programs that do not declare agents.

To run the tests:

```
make test HOLA_ADK_DIR=/path/to/hola-adk
```

`tests/run_lang.sh` compiles and runs the programs in `tests/lang`, and checks
the expected errors of the files that start with `// expect:`.
`tests/e2e/run_e2e.sh` runs agent programs against a local test server that
imitates an OpenAI-compatible API, in an environment with no Hola configuration.

## Installing

```
make install PREFIX=/usr/local HOLA_ADK_DIR=/path/to/hola-adk
```

`PREFIX` defaults to `~/.local`. The installed layout is:

```
PREFIX/bin/holacc
PREFIX/lib/holacc/                     runtime libraries
PREFIX/lib/hola-adk/                   the Hola ADK
PREFIX/share/hola-agent-toolchain/     README, LICENSE, examples
```

`holacc` locates the runtime and the ADK relative to its own executable, so no
environment variables are needed after installation. `make uninstall` removes
these files.

### Distribution archive

```
make dist HOLA_ADK_DIR=/path/to/hola-adk
```

writes `dist/hola-agent-toolchain-VERSION-linux-x86_64.tar.gz`. The archive
contains the layout above and an `install.sh`:

```
tar xzf hola-agent-toolchain-0.1.0-linux-x86_64.tar.gz
./hola-agent-toolchain-0.1.0-linux-x86_64/install.sh [PREFIX]
```

`tests/dist_test.sh` unpacks the archive, installs it to a temporary prefix and
builds programs with it.

## Usage

```
holacc build FILE [-o OUT] [-O0|-O1|-O2|-O3] [--emit-llvm] [--allow-plugins] [-v]
holacc run FILE [-- ARGS...]
holacc check FILE
holacc agents FILE
holacc doctor
holacc version
```

`build` produces an executable, `run` builds and runs a program, `check` only
checks it, and `agents` prints its agents, after `extends` is applied, as JSON.
Section 19 of the manual describes each command and option.

A compiled program that has no `func main` runs one of its agents with the
command line as input. `--help` lists the inputs. `--hola-agents` prints the
interface of each agent as JSON.

At run time, a program reads its API key from `HOLA_API_KEY`, or from the
profile of the same name in `~/.hola/profiles.json`. The model settings
themselves are compiled into the program (manual, section 17).

## Source layout

```
src/lexer.l, src/parser.y   lexical analyzer and grammar (flex, bison)
src/gen.c                   declarations, type checking, agent resolution, LLVM IR output
src/main.c                  the holacc driver
src/ast.c, src/strbuf.c     syntax tree and string buffer
lib/prelude.hl              declarations included in every program (coder, reader)
runtime/hl_rt.h             interface between generated code and the runtime
runtime/rt_core.c           strings, arrays, JSON, files, processes, program entry
runtime/rt_agent.c          running agents with the Hola ADK; tool actions; command-line input
runtime/chacha20.h          string encryption (ChaCha20)
third_party/cjson           cJSON
examples/                   example programs
doc/language.md             the language reference manual
```

`holacc` writes LLVM IR as text and calls `clang` to compile it and link it with
`libholart.a` and, for programs that declare agents, `libholart_agent.a` and the
Hola ADK.

## The Hola ADK

The agent runtime is built against the Hola ADK, which contains Hola's public
headers and two static libraries:

- `libhola.a`: the agent loop, model providers, profiles and tool registry;
- `libhola_tools.a`: Hola's file, search, edit, shell and git tools.

The ADK is produced from the Hola source tree by `scripts/build-adk.sh`. hola-cc
does not use any other part of Hola.

| Component | Uses Hola |
|-----------|-----------|
| `holacc` | no |
| core runtime (`rt_core.c`) | no |
| agent runtime (`rt_agent.c`) | yes, through the ADK |

## Compiled programs

A compiled program is one executable, linked dynamically only against the C
library, libcurl and libsqlite3. It reads no definition, plugin, skill or profile
files when it runs.

The program's strings, including prompts and skill texts, are stored encrypted
and decrypted when the program starts. The executable is stripped. This prevents
the text from being read directly from the file; it does not protect it from a
debugger or from someone who reads the requests sent to the model. API keys are
never compiled in.

## Limitations

- x86_64 GNU/Linux only.
- No maps, methods, interfaces, closures, multiple return values or concurrency.
- Memory is not reclaimed while a program runs.
- A failed agent run ends the program (exit status 3).
- Compiling programs requires `clang` on the build machine.

## License

GPL-3.0-or-later. See `LICENSE`. cJSON is distributed under the MIT license (see
`third_party/cjson`). A compiled agent program contains the Hola ADK and is
covered by its license.
