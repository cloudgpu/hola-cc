# The Hola Agent Language

Reference manual for the language accepted by `holacc` 0.1.0.

This manual describes the language as implemented. Where the implementation
has a limit or does something that may be surprising, the text says so. Target
platform: x86_64 GNU/Linux.

---

## Contents

1. Overview
2. Lexical structure
3. Program structure
4. Agents
5. Skills
6. Tools
7. Extending agents
8. Running agents
9. Programs without `main`
10. Types
11. Declarations and scope
12. Expressions
13. Statements
14. Functions
15. Built-in functions
16. Runtime errors and exit status
17. Model configuration
18. What a compiled program contains
19. The `holacc` command
20. Environment variables
21. Implementation limits
22. Grammar

---

## 1. Overview

A program in this language declares agents. An agent is a set of instructions for
a language model (persona, goal, task), a set of tools the model may call, and typed
input and output. When an agent runs, the model decides which tools to call and in
what order. The program does not specify that order.

Tools are declared, not programmed: each tool is bound to an action (run a command,
fetch a URL, show text, read or write a file, run another agent). Skills group
instructions and tools so that several agents can share them. An agent may extend
other agents.

The language also contains a small imperative subset (functions, structs, `if`,
`for`; sections 10 to 15). An agent program does not need it. It exists for
programs that call agents from their own code, and for tools that no declarative
binding covers.

`holacc` translates a program to LLVM IR and links it, using `clang`, with a runtime
library and the Hola ADK into a single executable.

---

## 2. Lexical structure

### 2.1 Source text

Source files are UTF-8 text. By convention they have the extension `.hl`. Bytes
outside ASCII may appear only in comments and string literals.

### 2.2 Comments

`//` starts a comment that runs to the end of the line. `/*` starts a comment that
runs to the next `*/`; such comments do not nest.

**Doc comments.** One or more consecutive `//` lines placed directly above a `func`,
`agent`, `skill` or `tool` declaration form that declaration's doc comment. The
leading `// ` is removed from each line. A doc comment is used as a description:
for an agent, it is shown by `--help` and `--hola-agents`; for a tool, it is the
tool description the model sees.

### 2.3 Statement termination

A newline ends a statement when the last token on the line is one of: an
identifier, a literal, `true`, `false`, `return`, `break`, `continue`, `++`, `--`,
`)`, `]` or `}`. In that case the newline is read as `;`. The end of the file is
treated the same way. This is the same rule Go uses. Consequences:

- An `else` must be on the same line as the `}` before it.
- A list that spans lines (composite literal fields, array elements) must end each
  line with `,`, or the last element must be followed directly by the closing `}`
  on the same line. A trailing `;` inserted before `}` is accepted.
- `;` may also be written explicitly.

### 2.4 Identifiers

An identifier is a letter or `_` followed by letters, digits and `_`. Case is
significant. The identifier `_` is the blank identifier; see 11.4.

### 2.5 Keywords

```
abstract  agent   break   continue  else    extends  false  for
func      if      import  override  package range    return skill
struct    tool    true    type      var
```

The agent settings (`persona`, `goal`, `task`, `profile`, `skills`, `tools`, `deny`,
`max_turns`, `input`, `output`, `instructions`) are not keywords. They have their
meaning only inside an agent or skill body and may be used as ordinary
identifiers elsewhere.

### 2.6 Composite literals and blocks

An identifier written immediately before `{`, with no space between, is the type
name of a composite literal (`Point{x: 1}`). An identifier followed by a space and
then `{` is an ordinary identifier followed by the start of a block
(`if done {`). Write composite literals without the space and blocks with it. The
keywords `struct`, `else` and the like are not affected: `struct{` and `else{` are
read as the keyword followed by `{`.

### 2.7 Literals

- **Integer literals**: decimal digits (`42`) or `0x` followed by hexadecimal
  digits (`0xff`). The value must fit in a signed 64-bit integer.
- **Floating literals**: digits, `.`, digits, with an optional exponent (`2.5`,
  `1.0e-3`), or digits with an exponent (`1e6`). A leading or trailing `.` alone
  (`.5`, `5.`) is not accepted.
- **Interpreted strings**: text between `"` characters, on one line. The escapes
  `\n`, `\t`, `\r`, `\0`, `\\` and `\"` are recognized. Any other backslash
  sequence is kept as written.
- **Raw strings**: text between backquotes. A raw string may span lines and has no
  escapes; it cannot contain a backquote.
- `true` and `false` are the boolean values.

### 2.8 Operators and punctuation

```
+   -   *   /   %   <   >   =   !   (   )   {   }   [   ]   ,   .   ;   :
==  !=  <=  >=  &&  ||  ++  --  +=  -=  *=  /=  :=
```

---

## 3. Program structure

### 3.1 Files and imports

A program is one source file plus the files it imports, recursively.

```go
package main          // optional; ignored
import "lib/util.hl"  // path relative to the importing file, or absolute
```

Each file is loaded at most once, however many times it is imported. All files
share one namespace. Imports may appear anywhere at the top level.

### 3.2 Top-level declarations

The top level contains, in any order:

| Declaration | Section |
|-------------|---------|
| `agent` and `abstract agent` | 4 |
| `skill` | 5 |
| `tool` | 6 |
| `type Name struct { ... }` | 10.5 |
| `func` | 14 |

A name may be declared only once among agents, skills and structs. Functions and
top-level tools have separate namespaces of their own. Declarations may refer to
names declared later in the program.

### 3.3 Entry point

If the program declares `func main()`, execution starts there. Otherwise, if it
declares at least one agent that is not abstract, the program runs an agent from
its command line (section 9). Otherwise it is an error.

### 3.4 The prelude

Every program implicitly contains these declarations:

```go
// A software engineer with Hola's file, search, edit, shell and git tools.
abstract agent coder {
    persona `You are a careful software engineer working in the user's project. ...`
    tools ["fs", "search", "edit", "exec", "git"]
}

// Looks at files and searches the project, but changes nothing.
abstract agent reader {
    persona `You read and search the project to answer questions. You do not change files.`
    tools ["fs", "search", "git"]
}
```

---

## 4. Agents

### 4.1 Declaration

```go
// Description.
[abstract] agent Name [extends Parent, Parent2, ...] {
    setting
    ...
}
```

Each setting occupies its own line (or is separated by `;`).

| Setting | Value | Meaning |
|---------|-------|---------|
| `persona` | string or `file("path")` | Who the agent is. Start of the system prompt. |
| `goal` | string or `file("path")` | What counts as done. Added to the system prompt under `## Goal`. |
| `task` | string or `file("path")` | Template for the first user message (4.3). |
| `profile` | string | Name of a model profile (17). |
| `skills` | list | Skills the agent uses (5). |
| `tools` | list | Tools the agent may call (6.4). |
| `deny` | list of strings | Tools removed from the agent, by name or tool set. |
| `max_turns` | integer ≥ 0 | Maximum number of model turns for one run; 0 means the Hola default. |
| `input { ... }` | field list | The agent's input type (4.2). |
| `output { ... }` | field list | The agent's output type (4.2). |
| `tool ...` | tool declaration | A tool belonging to the agent (6). |
| `func ...` | function | A tool implemented as a function (6.6). |

`file("path")` reads the file at compile time, relative to the source file, and
uses its contents as the string. A missing file is an error.

A setting may be preceded by `override`; see 7.3.

A list is written `[a, b, ...]`. Its elements are strings or identifiers, as
each setting allows. A trailing comma is permitted.

### 4.2 Input and output

```go
input {
    diff string "the unified diff to review"
    severity int
}
output {
    verdict string "approve or reject"
    findings []Finding
}
```

Each line declares a field: a name, a type (10) and an optional description
string. The descriptions become part of the JSON Schema shown to the model.

An agent's input type is named `Name.Input`, and its output type `Name.Output`.
Both are struct types (10.5).

If an agent has no input fields, neither declared nor inherited, its input is
`{ prompt string "The request" }`.

If an agent declares no output and inherits none, it has no output type, and
running it yields the model's final text as a `string`.

### 4.3 The task template

The first user message of a run is built as follows:

- If `task` is set, each `{{field}}` in it is replaced by the value of that input
  field: strings as they are, other values as JSON. `{{input}}` is replaced by the
  whole input as JSON. Spaces inside the braces are ignored. A placeholder that
  names no field is left as written.
- If `task` is not set and the input has exactly one field, of type `string`, the
  message is that field's value.
- Otherwise the message is the text `Input:` followed by the input as JSON in a
  fenced code block.

### 4.4 The system prompt

The system prompt of a run consists of, in order:

1. The persona (with an inherited persona first; see 7). If there is none, the
   text `You are <Name>, an AI agent.`
2. `## Goal` and the goal, if set.
3. For each skill, `## Skill: <name>` and its instructions.
4. If the agent has an output: an instruction to finish by calling
   `submit_result` once.

### 4.5 Abstract agents

An abstract agent can only be extended or used as a parent. Running one, by any
of the means in section 8, is a compile-time error. Abstract agents are not
listed by `--hola-agents` and cannot be selected on the command line.

---

## 5. Skills

### 5.1 Declaration

```go
// Description.
skill name {
    instructions "text or a raw string"
    tools [ ... ]
    deny [ ... ]
    skills [ ... ]
    tool ...
    func ...
}
```

A skill accepts only the settings `instructions`, `tools`, `deny`, `skills`,
`tool` and `func`. Any other setting is an error.

### 5.2 Use

An agent uses a skill by naming it in `skills`:

```go
agent Writer {
    skills [git_facts, "release-notes"]
}
```

- An **identifier** names a skill declared in the program. The agent receives the
  skill's instructions (in its system prompt), its tools and functions, its
  `tools` and `deny` entries, and the skills it lists in turn.
- A **string** names a skill file: `<name>/SKILL.md` in the `skills` directory
  next to the source file, in `./skills`, in `./.hola/skills`, or in
  `~/.hola/skills`, searched in that order at compile time. The file's text,
  without YAML front matter, is compiled into the program. A missing skill file is
  an error.

A skill is not an agent: it cannot be run, extended or bound as a tool.

---

## 6. Tools

### 6.1 Declared tools

```go
// Description shown to the model.
tool name(param type, ...) = action "template" ["template2"]
tool name = agent AgentName
```

A tool may be declared at the top level, inside a skill or inside an agent. The
parameters form the JSON Schema of the tool's arguments; all parameters are
required. In a template, `{{param}}` stands for the argument the model passed.
Parameters of a templated tool must be of type `int`, `float`, `bool` or
`string`. A placeholder that is not a parameter is a compile-time error.

The value returned to the model is text. A failed action is reported to the model
as an error result, and the model may react to it. Output longer than 64 KiB is
cut, with a note saying so.

### 6.2 Actions

**`sh "command"`** runs the command with `/bin/sh`, with standard error merged
into standard output. Every argument is inserted as one single-quoted shell word,
so an argument cannot end the command or start another one. The template itself
is not restricted. The result is the command's output. A non-zero exit status
makes the result an error, starting with `exit status N`.

**`http "url"`** performs a GET request with `curl` (redirects followed, 30-second
limit). Arguments are percent-encoded. The result is the response body followed by
a line `[HTTP <status>]`.

**`print "text"`** writes the text and a newline to standard output, where the
user sees it. The result is `shown to the user`.

**`read "path"`** returns the contents of the file.

**`write "path" "content"`** writes the content to the file, replacing it. The
result is `wrote N bytes to <path>`.

For `read` and `write`, the path after substitution must be relative and must not
contain a `..` component; otherwise the call fails. Paths are relative to the
program's working directory.

**`agent Name`** runs the named agent (8). The tool's parameters are the agent's
input fields; the declaration has no parameter list of its own. The result is the
agent's output as JSON, or its final text. The agent must not be abstract.

### 6.3 Hola tools

The Hola ADK provides these tools. They are named by strings, either one tool or a
whole tool set:

| Tool set | Tools |
|----------|-------|
| `fs` | `read_file`, `list_dir` |
| `search` | `grep`, `search_code` |
| `edit` | `write_file`, `edit_file`, `search_replace`, `apply_patch` |
| `exec` | `run_command`, `job_status`, `job_stop` |
| `plan` | `create_plan`, `list_tasks`, `mark_task_done`, `next_task` |
| `snapshots` | `list_snapshots`, `rollback_snapshot` |
| `interact` | `ask_user` |
| `git` | `git_status`, `git_diff`, `git_log` |

A name that matches neither a tool nor a set produces a warning when the agent
runs, not an error.

### 6.4 The tool list of an agent

The tools available to the model in a run are:

1. the Hola tools named in `tools` (directly, through a skill, or inherited);
2. the declared tools and functions of the agent, its skills and its parents, and
   the top-level tools listed by identifier in `tools`;
3. minus every tool whose name or tool set appears in `deny`;
4. plus `submit_result` if the agent has an output (8.3).

In `tools`, a string names a Hola tool or tool set, and an identifier names a
top-level `tool` declaration. `deny` takes strings only.

When two tools in one agent have the same name, the one added later replaces the
earlier one (see 7.2 for the order).

### 6.5 Agents as tools

A tool bound with `agent` lets one agent hand work to another. The inner agent
runs with its own model profile, prompt and tools. When it finishes, its result
becomes the outer agent's tool result.

### 6.6 Functions as tools

A `func` declared inside an agent or skill is a tool implemented in the imperative
subset. Its parameters may be of any type. Its result is returned to the model as
text: a string as it is, any other value as JSON; a function with no result
returns `done`. Functions declared at the top level are not tools.

---

## 7. Extending agents

### 7.1 Declaration

```go
agent Child extends A, B {
    ...
}
```

The parents must be agents (abstract or not), not skills. An agent may not extend
itself, directly or through a cycle; that is an error.

### 7.2 Merging

The settings of a child agent are computed by merging the parents from left to
right and then the child's own settings:

| Setting | Rule |
|---------|------|
| `persona` | All personas are concatenated in order (parents, then the child), separated by a blank line. A persona reached through two paths appears once. |
| `skills`, `tools`, `deny` | Union, in the order first seen. |
| tools and functions | Union. A tool with the same name as an earlier one replaces it. |
| `profile`, `goal`, `max_turns` | The last value set wins (the child's, if it sets one). |
| `input` | Union of the fields. The same field name with different types in two places is an error. Default `prompt` inputs (4.2) are not inherited. |
| `output` | The child's, if declared. Otherwise the parents' output, if they all agree on one; if two parents have different outputs, it is an error. |
| `task` | The child's, if set. Otherwise the parents' task, if they agree; if they differ, it is an error. |

A child that inherits an output shares the parent's output type: `Child.Output`
and `Parent.Output` are the same type.

### 7.3 `override`

`override` before a setting replaces the inherited value instead of merging:

```go
agent Quiet extends Reviewer {
    override persona "You review code. You report only certain bugs."
    override tools ["fs"]
}
```

It applies to `persona`, `skills`, `tools`, `deny` and `input`. It has no effect
on settings that are already replaced rather than merged (`output`, `task`,
`profile`, `goal`, `max_turns`). It is an error on `func` and `tool`: a tool with
the name of an inherited one replaces it without `override`.

---

## 8. Running agents

### 8.1 Expressions

```go
r := Reviewer{diff: d}.run()     // r has type Reviewer.Output (or string)
s := Chat.ask("hello")           // only for an agent whose input is one string field
```

`Name{field: value, ...}` constructs a value of type `Name.Input`; it is the same
as `Name.Input{...}`. Calling `.run()` on a value of an input type runs that
agent. `Name.ask(text)` constructs the input from its one string field and runs
the agent.

Running an agent blocks until the run ends.

### 8.2 A run

1. The model profile is resolved (17).
2. The tool list is built (6.4).
3. The system prompt (4.4) and the first message (4.3) are sent to the model.
4. Hola's agent loop runs: the model's tool calls are executed and their results
   returned, until the model replies without calling a tool or `max_turns` is
   reached.

### 8.3 Results

If the agent has an output, the run must produce it through `submit_result`.
That tool takes the output fields as its arguments. Each call is checked against
the output type: a missing field, a value of the wrong JSON type or a non-integral
number for an `int` field is reported back to the model as an error naming the
field, for example `/lines: must be an integer`. The model may then call it again.

If the loop ends without a valid result, the model is reminded to call
`submit_result`, at most twice. If there is still no valid result, the program
prints `agent <Name>: finished without a valid result` and exits with status 3.

If the agent has no output, the result is the model's final text.

### 8.4 Failure

If the model cannot be reached or the provider returns an error, the program
prints `agent <Name>: the model call failed: <reason>` and exits with status 3.
Running an agent has no error value; a failure ends the program.

---

## 9. Programs without `main`

A program that has agents and no `func main` uses its command line to run one of
them.

- If the program has one agent that is not abstract, that agent runs. If it has
  several, the first argument is the name of the one to run.
- If the agent's input is a single `string` field, the remaining arguments,
  joined by spaces, are its value. If there are no arguments and standard input
  is not a terminal, standard input is read instead.
- Otherwise each argument has the form `field=value`. The value is converted to
  the field's type: numbers and `true`/`false`/`yes`/`no`/`1`/`0` as such, arrays
  and structs as JSON. Every field must be given.
- `-h` or `--help` prints a usage line for each agent and exits with status 2.
  Unknown or missing arguments do the same, after a message.
- The result is printed on standard output: the output value as one line of
  JSON, or the final text.

Every compiled program, with or without `main`, also accepts `--hola-agents` as
its only argument. It prints a JSON array describing each agent that is not
abstract (name, description, tools with their parameter schemas, and input and
output schemas) and exits with status 0. It does not print personas, goals,
tasks, skills or profiles.

---

## 10. Types

### 10.1 Basic types

| Type | Values | Zero value |
|------|--------|------------|
| `int` | signed 64-bit integers; arithmetic wraps on overflow | `0` |
| `float` | IEEE 754 double precision | `0` |
| `bool` | `true`, `false` | `false` |
| `string` | immutable sequences of bytes | `""` |

Strings cannot contain the byte 0; a string ends at the first 0 byte.

### 10.2 Array types

`[]T` is a resizable array of values of type `T`. Arrays are reference values:
assigning an array or passing it to a function shares the same array. The zero
value is an empty array. An array is extended with `append` (15).

### 10.3 Struct types

A struct type is declared at the top level:

```go
type Point struct {
    x int
    y int
    label string "optional description"
}
```

Field names must be unique within the struct. Struct values are references, as
arrays are. A struct-typed field that has not been assigned is nil; reading a
field through a nil struct is a run-time error (16).

Agent input and output types (4.2) are struct types.

### 10.4 Type identity

Two types are identical if they are the same basic type, if they are arrays of
identical element types, or if they are the same declared struct. Structs with
the same fields but different names are different types.

### 10.5 Conversions and assignability

A value can be assigned to a variable, field, parameter or array element only if
its type is identical to the target type, with one exception: an integer
constant (a literal, possibly negated) can be used where a `float` is expected.
All other conversions are explicit (`int`, `float`, `str`; see 15).

---

## 11. Declarations and scope

### 11.1 Variables

```go
var count int            // zero value
var count int = 3
var count = 3            // type from the value
count := 3               // short form
```

`:=` and `var` declare a new variable in the current block. Declaring a name that
is already declared in the same block is an error. A variable declared in an inner
block hides one of the same name in an outer block.

A `var` without a value gets the zero value of its type. For a struct type that
is a new struct whose fields have their zero values.

### 11.2 Blocks

Each `{ ... }` is a block. The body of an `if`, `else` or `for` is a block. The
init statement of a `for` and the variables of a `for ... range` belong to an
implicit block around the loop.

### 11.3 Name resolution

In an expression, a name refers, in this order, to: a variable in scope, then an
agent (in `Name.ask(...)`). In a call, a name that is not a variable refers to a
function, then to a built-in function. A function may not have the name of a
built-in function.

### 11.4 The blank identifier

`_` may be used as the name of a `for ... range` variable, as the left side of
`:=`, and as the left side of `=`. The value is evaluated and discarded.

---

## 12. Expressions

### 12.1 Operators

From highest to lowest precedence. Binary operators at the same level are
left-associative.

| Precedence | Operators |
|------------|-----------|
| 6 | unary `-` and `!` |
| 5 | `*` `/` `%` |
| 4 | `+` `-` |
| 3 | `==` `!=` `<` `<=` `>` `>=` |
| 2 | `&&` |
| 1 | `\|\|` |

Postfix operations (calls, field selection, indexing) bind tighter than any
operator.

### 12.2 Arithmetic

Both operands must have the same type, except that an integer constant may be
combined with a `float` (10.5).

- `int`: `+ - * / %`. Integer division truncates toward zero. Division or
  remainder by zero is a run-time error.
- `float`: `+ - * / %`, following IEEE 754 (`%` is the floating-point remainder).
- `string`: `+` concatenates.

### 12.3 Comparison

`==` and `!=` apply to `int`, `float`, `bool` and `string`. `<`, `<=`, `>`, `>=`
apply to `int`, `float` and `string`. Strings compare byte by byte. Arrays and
structs cannot be compared. The result is `bool`.

### 12.4 Logical operators

`&&` and `||` take `bool` operands and evaluate the right operand only if needed.
`!` negates a `bool`.

### 12.5 Field selection and indexing

`s.f` is field `f` of struct `s`. `a[i]` is element `i` of array `a`, counted
from 0; an index outside `[0, len(a))` is a run-time error. `s[i]` on a string is
the byte at index `i` as an `int`.

### 12.6 Composite literals

```go
Point{x: 1, y: 2}
Reviewer{diff: d}                  // Reviewer.Input
[]int{1, 2, 3}
[][]string{}
```

In a struct literal, fields are named; each field may be given once; fields not
given have their zero value, except struct-typed fields, which are nil. An array
literal lists its elements in order.

### 12.7 Calls

`f(a, b)` calls a function. The number of arguments must equal the number of
parameters, and each argument must be assignable to its parameter. Arguments are
evaluated left to right.

A call to a function with no result can only be used as a statement.

---

## 13. Statements

### 13.1 Simple statements

```go
x = e         a[i] = e        s.f = e
x += e        x -= e          x *= e         x /= e
x++           x--
f(args)
```

`+=` also concatenates strings. `++` and `--` apply to `int` variables, fields and
elements. An expression used as a statement must be a call.

### 13.2 `if`

```go
if cond {
    ...
} else if cond {
    ...
} else {
    ...
}
```

The condition must be a `bool`.

### 13.3 `for`

```go
for { ... }                          // forever
for cond { ... }
for init; cond; post { ... }         // each part optional
for i := range xs { ... }            // indexes
for i, v := range xs { ... }         // indexes and elements
```

`range` accepts an array or a string. For a string, the elements are bytes as
`int`. The length is checked before each iteration.

### 13.4 `break`, `continue`, `return`

`break` ends the innermost loop; `continue` starts its next iteration. `return`
leaves the function, with a value if the function has a result type.

---

## 14. Functions

```go
// Description.
func name(a int, b string) string {
    ...
}
```

A function has zero or more parameters and at most one result type. Parameters
are local variables initialized with the arguments. Functions may call each other
in any order, including recursively.

A function with a result type that reaches the end of its body without `return`
stops the program with a run-time error.

`func main()` takes no parameters and returns nothing.

---

## 15. Built-in functions

| Function | Result | Description |
|----------|--------|-------------|
| `print(v, ...)` | — | Writes the values with no separator. |
| `println(v, ...)` | — | Writes the values separated by spaces, then a newline. |
| `len(x)` | `int` | Length of a string (bytes) or array. |
| `append(a, v, ...)` | `[]T` | Adds values to the end of `a` and returns the array. Write `a = append(a, v)`. |
| `str(v)` | `string` | Text form of a value; arrays and structs as JSON. |
| `int(v)` | `int` | From `float` (truncated), `string` (decimal; 0 if invalid), `bool`. |
| `float(v)` | `float` | From `int` or `string` (0 if invalid). |
| `json(v)` | `string` | Any value as JSON. |
| `split(s, sep)` | `[]string` | Pieces of `s` between occurrences of `sep`; with an empty `sep`, single bytes. |
| `join(a, sep)` | `string` | Joins a `[]string`. |
| `trim(s)` | `string` | Removes leading and trailing white space. |
| `upper(s)`, `lower(s)` | `string` | ASCII case conversion. |
| `replace(s, old, new)` | `string` | Replaces every occurrence. |
| `substr(s, a, b)` | `string` | Bytes `a` to `b-1`; the range is clamped to the string. |
| `contains(s, t)`, `has_prefix(s, t)`, `has_suffix(s, t)` | `bool` | Substring tests. |
| `index_of(s, t)` | `int` | First position of `t` in `s`, or -1. |
| `sh(cmd)` | `string` | Runs a shell command and returns its standard output. |
| `exit_code()` | `int` | Exit status of the last `sh`, or -1 if it did not exit normally. |
| `read_file(path)` | `string` | File contents, or `""` if the file cannot be read. |
| `write_file(path, s)` | `bool` | Writes the file; `true` on success. |
| `file_exists(path)` | `bool` | Whether the path exists. |
| `getenv(name)` | `string` | Environment variable, or `""`. |
| `args()` | `[]string` | Command-line arguments, without the program name. |
| `exit(code)` | — | Ends the program with the given status. |
| `panic(v)` | — | Ends the program with a run-time error (16). |
| `sleep_ms(n)` | — | Waits `n` milliseconds. |
| `now_ms()` | `int` | Current time in milliseconds since the Unix epoch. |

`print` and `println` write strings as they are, numbers and booleans in their
usual form, and arrays and structs as JSON. Floats are written in the shortest
form that reads back as the same value (`0.1`, `3`, `1e+21`).

The `sh`, `read_file` and `write_file` built-ins, unlike tool actions (6.2), are
not restricted in any way: they are code written by the programmer, not
actions requested by a model.

---

## 16. Runtime errors and exit status

A run-time error prints `<file>:<line>: panic: <message>` on standard error and
ends the program with status 2. Run-time errors are: an array or string index out
of range, integer division by zero, a field access through a nil struct, a
function that ends without returning its result, and `panic`.

| Status | Meaning |
|--------|---------|
| 0 | normal end, or `--hola-agents` |
| 2 | run-time error, or a command-line usage error (section 9) |
| 3 | an agent failed (8.3, 8.4), or its model configuration is missing |
| other | the value passed to `exit` |

Memory is not reclaimed while a program runs. It is released when the program
exits.

---

## 17. Model configuration

### 17.1 At compile time

For each agent that is not abstract, `holacc` looks up its profile: the
`profile` setting, or if there is none, the `default` entry. It reads
`.hola/profiles.json` in the current directory if present, otherwise
`~/.hola/profiles.json`. These are Hola's profile files. The profile's settings
(model, endpoint, API mode, token limits, reasoning level and the rest) are
compiled into the program.

Credentials are not compiled in. Every string-valued entry whose name contains
`key`, `token`, `secret`, `password` or `auth`, at any depth, is removed.

A named profile that does not exist is a compile-time error, except for the
names `codex` and `claude`, which Hola provides itself and which are resolved at
run time.

### 17.2 At run time

The API key is taken from `HOLA_API_KEY`. If that is not set, it is taken from the
profile of the same name in the machine's own profile files. If neither exists,
no key is sent; this is correct for local model servers.

Setting `HOLA_PROFILE=<name>` makes all agents use that profile from the
machine's profile files instead of the compiled-in settings.

A program compiled when no profile file existed takes its model from
`OPENAI_BASE_URL`, `OPENAI_API_KEY` and `HOLA_MODEL` (or `OPENAI_MODEL`,
`OLLAMA_BASE_URL`, `OLLAMA_MODEL`).

---

## 18. What a compiled program contains

A compiled program is a single x86_64 ELF executable. It contains:

- the program's code, agents, prompts, skill texts, tool declarations and
  profile settings;
- the runtime, Hola's agent loop and providers, and the Hola tools (6.3), linked
  statically.

It is linked dynamically against system libraries only: the C library, libcurl,
libsqlite3 and their dependencies. When it runs, it reads no definition files,
plugin files, skill files or profile files, apart from the cases in 17.2.

Every string constant of the program is stored encrypted with ChaCha20 under a
key chosen at random for each build, and is decrypted in memory before the
program starts. The executable is stripped of symbol and debug information, and
does not export symbols unless it was built with `--allow-plugins`. These
measures prevent the program's text from being read directly from the file. They
do not protect it against someone who runs the program under a debugger, or who
reads the requests it sends to the model.

---

## 19. The `holacc` command

```
holacc build FILE [-o OUT] [-O0|-O1|-O2|-O3] [--emit-llvm] [--allow-plugins] [-v]
holacc run FILE [-- ARGS...]
holacc check FILE
holacc agents FILE
holacc doctor
holacc version
```

- **`build`** compiles FILE to an executable, by default named after FILE
  without its extension. `-O` sets the optimization level (default `-O2`).
  `--emit-llvm` writes the LLVM IR (`.ll`) instead of an executable.
  `--allow-plugins` exports symbols so that extra Hola plugins can be loaded at
  run time (20). `-v` prints the commands run.
- **`run`** builds into a temporary directory, runs the program with ARGS, removes
  it and exits with the program's status.
- **`check`** parses and checks FILE without generating code.
- **`agents`** prints every agent after `extends` is applied, as JSON, including
  personas and tasks. It is meant for the program's author.
- **`doctor`** checks that the runtime, the Hola ADK, `clang` and the libcurl and
  sqlite3 libraries are available. It exits with status 1 if something is
  missing.
- **`version`** prints the toolchain version.

Diagnostics have the form `<file>:<line>: error: <message>`, followed by a count
of errors. `holacc` exits with status 1 if there were errors and 2 for a usage
error.

`holacc` requires `clang` to compile LLVM IR and link. The runtime and the ADK are
found from the environment (20), else from `../lib/holacc` and `../lib/hola-adk`
relative to the `holacc` executable, else from the build tree it was built in.

---

## 20. Environment variables

### 20.1 Read by `holacc`

| Variable | Meaning |
|----------|---------|
| `HOLACC_RUNTIME_DIR` | Directory of `libholart.a`, `libholart_agent.a`, `libcjson.a`. |
| `HOLA_ADK_DIR` | Directory of the Hola ADK. |
| `CLANG` | The compiler driver to use (default `clang`). |
| `HOLACC_TMPDIR` | Directory for `run`'s temporary files (default `/tmp`). |
| `HOLACC_KEEP_LL` | If set, keep the `.ll` file next to the executable. |
| `HOLACC_DEBUG` | If set, do not strip the executable. |
| `HOME` | Location of `~/.hola` (profiles, skills). |

### 20.2 Read by compiled programs

| Variable | Meaning |
|----------|---------|
| `HOLA_API_KEY` | API key for the compiled-in profile. |
| `HOLA_PROFILE` | Use this profile from the machine instead of the compiled-in one. |
| `OPENAI_BASE_URL`, `OPENAI_API_KEY`, `HOLA_MODEL` | Model, when the program was compiled without a profile. |
| `HOLACC_VERBOSE` | If set to a value other than `0`, print each run and tool call on standard error. |
| `HOLACC_STREAM` | `1` asks the provider for streamed responses. |
| `HOLACC_SESSION_DB` | File for Hola's session database (default: in memory). |
| `HOLA_PLUGIN_DIR`, `HOLACC_PLUGINS` | Load the named Hola plugins (comma-separated) from the directory. Needs `--allow-plugins`. |
| `TMPDIR` | Temporary directory (used briefly while loading the profile). |

Hola's own variables (for example `HOLA_REQUESTS_PER_MINUTE`) also apply, since
the agent loop is Hola's.

---

## 21. Implementation limits

- Target: x86_64 GNU/Linux only.
- There are no maps, methods, interfaces, closures, multiple return values,
  concurrency or error values.
- Memory is not reclaimed while a program runs.
- Running an agent cannot fail recoverably; a failed run ends the program.
- Tool output is limited to 64 KiB per call.
- Agents of one program run one at a time, except that an agent may run another
  agent through a tool.

---

## 22. Grammar

The grammar below uses EBNF: `[ x ]` is optional, `{ x }` is zero or more
repetitions, `|` separates alternatives. `;` stands for both explicit and
inserted semicolons (2.3). `IDENT{` is an identifier immediately followed by `{`
(2.6).

```ebnf
Program       = { ";" } { TopLevel ";" { ";" } } .
TopLevel      = "package" IDENT
              | "import" STRING
              | FuncDecl
              | "type" IDENT "struct" "{" FieldSeq "}"
              | AgentDecl
              | SkillDecl
              | ToolDecl .

FuncDecl      = "func" IDENT "(" [ Params ] ")" ( "{" | TypeLBrace ) StmtList [ Stmt ] "}" .
Params        = Param { "," Param } .
Param         = IDENT Type .
Type          = IDENT | IDENT "." IDENT | "[" "]" Type .
TypeLBrace    = Type "{" .               (* a result type and the body's "{" *)

FieldSeq      = { Field | ";" } .
Field         = IDENT Type [ STRING ] .

AgentDecl     = [ "abstract" ] "agent" IDENT [ "extends" IDENT { "," IDENT } ] "{" AgentItems "}" .
SkillDecl     = "skill" IDENT "{" AgentItems "}" .
AgentItems    = { [ AgentItem ] ";" } .
AgentItem     = IDENT Value                (* persona "..." , max_turns 10, tools [...] *)
              | IDENT "{" FieldSeq "}"     (* input { ... } , output { ... } *)
              | FuncDecl
              | ToolDecl
              | "override" AgentItem .
Value         = STRING | INT | FLOAT | "true" | "false" | IDENT
              | IDENT "(" STRING ")"       (* file("path") *)
              | "[" [ ListItem { "," ListItem } [ "," ] ] "]" .
ListItem      = STRING | IDENT .

ToolDecl      = "tool" IDENT "(" [ Params ] ")" "=" IDENT STRING [ STRING ]
              | "tool" IDENT "=" "agent" IDENT .

StmtList      = { [ Stmt ] ";" } .
Block         = "{" StmtList [ Stmt ] "}" .
Stmt          = SimpleStmt
              | "var" IDENT Type [ "=" Expr ]
              | "var" IDENT "=" Expr
              | IfStmt
              | ForStmt
              | "return" [ Expr ]
              | "break" | "continue"
              | Block .
SimpleStmt    = Expr
              | IDENT ":=" Expr
              | Expr ( "=" | "+=" | "-=" | "*=" | "/=" ) Expr
              | Expr ( "++" | "--" ) .
IfStmt        = "if" Expr Block [ "else" ( Block | IfStmt ) ] .
ForStmt       = "for" Block
              | "for" Expr Block
              | "for" [ SimpleStmt ] ";" [ Expr ] ";" [ SimpleStmt ] Block
              | "for" IDENT [ "," IDENT ] ":=" "range" Expr Block .

Expr          = Unary | Expr BinaryOp Expr .
BinaryOp      = "||" | "&&" | "==" | "!=" | "<" | "<=" | ">" | ">="
              | "+" | "-" | "*" | "/" | "%" .
Unary         = Primary | "-" Unary | "!" Unary .
Primary       = INT | FLOAT | STRING | "true" | "false" | IDENT
              | "(" Expr ")"
              | Primary "(" [ Expr { "," Expr } [ "," ] ] ")"
              | Primary "." IDENT
              | Primary "[" Expr "]"
              | IDENT{ [ KeyValues ] "}"
              | IDENT "." IDENT{ [ KeyValues ] "}"
              | "[" "]" TypeLBrace [ Exprs ] "}" .
KeyValues     = IDENT ":" Expr { "," IDENT ":" Expr } [ "," ] .
Exprs         = Expr { "," Expr } [ "," ] .
```
