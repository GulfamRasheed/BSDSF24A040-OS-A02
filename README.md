# BSDSF24A040-OS-A02
Operating Systems Programming Assignment 02

Operating Systems · Programming Assignment 02

A from-scratch reimplementation of the Unix `ls` command in C, built up
across seven tagged versions.

---

## Build

```bash
make                    # builds src/ls-v1.0.0.c (the current working file)
make SRC=versions/ls-v1.2.0.c    # rebuilds any specific tagged version
make clean
```

Requires `gcc` and a POSIX system (Linux/WSL). Compiles with
**`-std=c11 -D_GNU_SOURCE -Wall -Wextra -O2` and zero warnings** across all
seven versions.

> The instructor's starter repository ships **no Makefile**, so this one is
> written from scratch as part of Feature-1.

## Base version

The starting point is the instructor's
[`Programming-Assignments/PA-02/lsv1.0.0.c`](https://github.com/arifpucit/OS-Codes).
Its structure is preserved throughout — same function name `do_ls()`, same
multi-directory argument loop in `main()`, same dotfile filtering. Each
feature adds to it without restructuring it.

Two deliberate changes to the base:

1. `extern int errno;` is dropped from v1.1.0 onward — `<errno.h>` already
   declares it, and the redeclaration is redundant.
2. `getopt()` needs a cast: the base declares `main(int, char const *argv[])`,
   which makes `argv` a `const char **`, but `getopt` is declared as
   `int getopt(int, char *const *, const char *)`. The two are incompatible
   pointer types, so the call site casts to `(char *const *)`.

## Usage

```bash
./bin/ls                # default: "down then across" columns
./bin/ls -l             # long listing
./bin/ls -x             # horizontal ("across") columns
./bin/ls -R             # recursive
./bin/ls -lR src        # flags combine
./bin/ls dir1 dir2      # several directories, as in the base version
```

## Features by version

| Tag | Snapshot | Adds |
|---|---|---|
| — | `versions/ls-v1.0.0.c` | instructor's base — `opendir`/`readdir`, one name per line |
| `v1.1.0` | `versions/ls-v1.1.0.c` | `-l` long listing — `lstat`, `getpwuid`, `getgrgid`, `ctime`, `rwx` formatting |
| `v1.2.0` | `versions/ls-v1.2.0.c` | dynamic array + `ioctl(TIOCGWINSZ)`, "down then across" columns |
| `v1.3.0` | `versions/ls-v1.3.0.c` | `-x` horizontal columns, display-mode `enum` |
| `v1.4.0` | `versions/ls-v1.4.0.c` | alphabetical sort via `qsort` + comparison function |
| `v1.5.0` | `versions/ls-v1.5.0.c` | ANSI colour by file type, gated on `isatty()` |
| `v1.6.0` | `versions/ls-v1.6.0.c` | `-R` recursive listing |

`src/ls-v1.0.0.c` is the **working file** — the base, extended feature by
feature. `versions/` holds an immutable snapshot per tag so any released
version can be rebuilt exactly as shipped.

## Layout

```
.
├── src/ls-v1.0.0.c     the evolving working file
├── versions/           one snapshot per tagged release
├── bin/                compiled executable
├── obj/                object files
├── man/                (reserved — man page, bonus task)
├── Makefile
├── README.md
└── REPORT.md           analysis answers for every feature
```

## Design notes

- **Data preparation is separated from presentation.** The directory is read
  into a dynamic array, sorted **once**, then handed to a display function.
  That is why adding `-R`, `-x` and colour required no change to the
  long-listing code, and why every display mode comes out sorted.
- **Column width is uniform**, from the longest filename plus 2 spaces, as
  specified. Real GNU `ls` optimises per-column widths.
- **Escape codes are suppressed when stdout is not a terminal** (`isatty`),
  so `ls > out.txt` produces clean text.
- **Recursion is bounded** three ways: `opendir` failure, skipping `.` and
  `..`, and a depth limit — so symlink cycles and bind-mounts cannot hang
  the program.

See [`REPORT.md`](REPORT.md) for the full analysis and every feature's
report questions.

## Testing performed

- All seven versions compile with zero warnings under `-Wall -Wextra`.
- End-to-end runs of every display mode and combined flags, built through
  the real Makefile, plus the base version's multi-directory argument.
- Output compared against the real `ls` for ordering and layout.
- Column count verified to track terminal width from 40 to 200 columns.
- Stress test with 3200 files and 9 levels of nesting — no stack overflow.
- **75 runs** under AddressSanitizer + UndefinedBehaviorSanitizer +
  LeakSanitizer (symlink cycles, broken links, FIFOs, names containing
  spaces, empty directories, unreadable paths, multiple arguments):
  **zero findings**.