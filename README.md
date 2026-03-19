# boots

A terminal-based robot platform. Define robots as `.robot` files — describe their hardware, embed or reference their source code in C, C++, or Python, and interact with them through a live REPL.

```
git clone https://github.com/bootsplatform/boots.git
cd boots
gcc -O2 -o boots boots.c -lm
```

---

## How it works

A `.robot` file is a plain text file that defines everything about a robot: its name, hardware specs, ASCII art, the commands it exposes, and the actual source code that runs when you talk to it. Running `boots init` compiles that code and caches the binary. Running `boots interact` opens a REPL where every command you type is forwarded as `argv[1]` to the compiled robot program.

---

## Commands

```
boots init     <file.robot>    Compile and register a robot
boots interact <file.robot>    Open interactive session
boots info     <file.robot>    Show robot specs and file layout
boots list                     List all initialized robots
boots remove   <robot_name>    Remove a robot from cache
boots example                  Generate a working multi-file example
boots help                     Show usage
```

---

## .robot file format

```ini
name        = MyRobot
version     = 1.0
author      = You
description = What this robot does
language    = c

[hardware]
engine      = ...
sensors     = ...
actuators   = ...
power       = ...
comm        = ...

[art]
  (ASCII art lines here)

[commands]
greet
scan
move <direction>

[code]
  (single-file C / C++ / Python program)
```

Every command typed in the REPL is passed to the robot's compiled binary as `argv[1]`, with any further words as `argv[2]`, `argv[3]`, etc. Your robot's `main()` reads `argv[1]` and dispatches accordingly.

---

## Multi-file projects

Robots can be real multi-file projects, not just single scripts. There are three ways to do it.

**1. `srcdir` — auto-collect from a directory**

Point boots at a folder and it will find and compile every source file in it:

```ini
srcdir = ./src
```

Headers (`.h`, `.hpp`) are copied into the build directory automatically so `#include "myheader.h"` works.

**2. `[files]` — explicit file list**

List files explicitly, relative to the `.robot` file:

```ini
[files]
src/main.c
src/sensors.c
lib/motors.c
```

**3. `[code:<filename>]` — inline multiple files**

Embed an entire multi-file project inside the `.robot` file itself, no external files needed:

```ini
[code:main.c]
#include "utils.h"
int main(int argc, char **argv) { ... }

[code:utils.h]
void helper(void);

[code:utils.c]
void helper(void) { ... }
```

You can mix these approaches. Extra compiler/linker flags go on the `flags` field:

```ini
flags = -lpthread -lssl
```

For Python multi-file projects, set which file is the entry point:

```ini
main = app.py
```

---

## Example: single-file robot

```ini
name        = ARIA
version     = 1.0
language    = c

[art]
  .------.
 /  O  O  \
|  ------  |
|   ARIA   |
 \________/

[commands]
greet
time

[code]
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (!strcmp(argv[1], "greet")) {
        printf("Hello. I am ARIA.\n");
    } else if (!strcmp(argv[1], "time")) {
        time_t t = time(NULL);
        printf("%s", ctime(&t));
    }
    return 0;
}
```

```
boots init ARIA.robot
boots interact ARIA.robot
[ARIA] > greet
Hello. I am ARIA.
[ARIA] > time
2026-03-19 07:00:00
```

---

## Example: multi-file Python robot

```ini
name     = PYBOT
language = python
srcdir   = ./pybot_src
main     = main.py
```

```
pybot_src/
  main.py       <- entry point (receives sys.argv[1] as the command)
  sensors.py
  utils.py
```

boots copies all `.py` files into the cache directory and runs `python3 main.py <command> [args]` on every REPL input.

---

## Built-in REPL commands

These are always available regardless of what the robot defines:

| Command  | Description                        |
|----------|------------------------------------|
| `help`   | List the robot's declared commands |
| `status` | Show robot name, version, uptime   |
| `exit`   | Disconnect from the robot          |

---

## Cache layout

Everything boots compiles lives under `.boots/cache/` in your working directory. Each robot gets its own subdirectory:

```
.boots/
  cache/
    ARIA/
      ARIA_robot        <- compiled binary
      main.c            <- source files used
      ...
    PYBOT/
      main.py
      sensors.py
      ...
```

`boots remove <name>` deletes the entire robot directory from cache. Source files outside `.boots/` are never touched.

---

## Requirements

- GCC or G++ for C/C++ robots
- Python 3 for Python robots
- A POSIX-compatible system (Linux, macOS, Termux on Android)

No libraries beyond the C standard library and POSIX are required to build boots itself.

---

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

```
boots  Copyright (C) 2026  bootsplatform
This program comes with ABSOLUTELY NO WARRANTY.
This is free software, and you are welcome to redistribute it
under the terms of the GNU GPL v3.
```
