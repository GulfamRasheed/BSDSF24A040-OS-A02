# REPORT — Operating Systems, Programming Assignment 02

**Project:** Re-engineering the `ls` utility
**Student Roll No:** `__________________`
**Repository:** `ROLL_NO-OS-A02`

---

## How to build and run

```bash
make                             # builds src/ls-v1.0.0.c into bin/ls
make SRC=versions/ls-v1.2.0.c    # rebuilds a specific tagged version
make clean

./bin/ls                # default: "down then across" columns
./bin/ls -l             # long listing
./bin/ls -x             # horizontal ("across") columns
./bin/ls -R             # recursive
./bin/ls -lR src        # flags combine
./bin/ls dir1 dir2      # several directories, as in the base version
```

**Base version.** The starting point is the instructor's
`Programming-Assignments/PA-02/lsv1.0.0.c` from
[arifpucit/OS-Codes](https://github.com/arifpucit/OS-Codes). Its structure is
preserved throughout: the listing function stays `do_ls()`, `main()` keeps
its multi-directory argument loop, and dotfile filtering is unchanged. Each
feature adds to that base rather than restructuring it.

Two deliberate departures from the base:

1. **`extern int errno;` is dropped from v1.1.0 onward.** `<errno.h>` already
   declares `errno` (as a macro expanding to a thread-local pointer), so the
   redeclaration is redundant.
2. **`getopt()` is called with an explicit cast.** The base declares
   `int main(int argc, char const *argv[])`, which makes `argv` a
   `const char **`. But `getopt` is declared
   `int getopt(int, char *const *, const char *)`. Those are *incompatible
   pointer types*, and passing one without a cast is a constraint violation
   that GCC rejects with `-Wincompatible-pointer-types`. The cast to
   `(char *const *)` is safe because `getopt` may **permute** `argv` and
   substitute option values, but it never modifies the strings the pointers
   refer to.

**The Makefile** is written from scratch for Feature-1: the instructor's
starter repository contains no Makefile at all.

> `ls -R` prints a `path:` header before every directory it lists, which is
> why `do_ls()` prints it itself. On non-recursive invocations that header is
> the only difference from `ls -lx`.

---

## Feature-2 — Long Listing (`-l`)

### Q1. `stat()` vs `lstat()`, and when is `lstat()` the right choice?

Both calls fill the same `struct stat`, and both return `0` on success.
They differ in exactly one respect: **whether they follow symbolic links.**

| | `stat()` | `lstat()` |
|---|---|---|
| Argument is a symlink to a regular file | returns metadata of the **target** | returns metadata of the **link itself** |
| `st_mode` contains `S_IFLNK`? | no | yes |
| Syscall overhead | one extra kernel path-resolution step | one fewer |

`stat()` internally does the work of `lstat()` and then repeats the lookup
on whatever the link resolved to, so it is strictly more expensive.

**When `ls` needs `lstat()`:** whenever the tool must report on the *link*
rather than on what it points to. `ls -l` has to print `l` in the first
permission column and show `link -> target` at the end of the line; that
information exists only in the link's own inode, which `stat()` discards.
For the same reason `ls -R` must use `lstat()` before recursing: if a
directory contains a link pointing back up towards the root, `stat()` would
report "directory" and we would follow it into an infinite cycle, whereas
`lstat()` reports "symlink" and we correctly do not descend.

The mirror-image case is `stat()` being *correct*: if the program genuinely
wants the size of the data, then for `/usr/bin/gcc -> gcc-13` it is the
size of `gcc-13` the user cares about, not the 10-byte link. Both calls are
right; the choice depends on the question being asked.

### Q2. Extracting type and permission bits from `st_mode` with `&`

`st_mode` is a single `mode_t` (an `unsigned int`) that packs three
independent fields into 32 bits:

```
 31    30   29              12  11    9    8    7    6    5    4    3    2    1    0
┌────┬────┬──────────────────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┐
│typ │    │   special bits   │uid │gid │other  grp    own          │oth │grp │own │
│ 4b │    │  setuid setgid st│    │    │ x  r w  x   r   w   x  │ x  │ r  │ w  │ x  │
└────┴────┴──────────────────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┘
```

Because the fields never overlap, each can be extracted with a bitwise AND
against a mask. `x & mask` yields the masked bits when they are set and `0`
when they are clear, so the result is directly usable as a boolean.

**Type extraction — a mask, then a switch.** The four type bits are
*mutually exclusive*: exactly one of `S_IFDIR (0040000)`, `S_IFREG (0100000)`,
`S_IFLNK (0120000)`, `S_IFBLK (0060000)`, `S_IFCHR (0020000)`, `S_IFIFO (0010000)`,
`S_ISSOCK (0140000)` can be set. So:

```c
switch (mode & S_IFMT) {                 /* S_IFMT = 0170000, all 4 bits */
    case S_IFDIR:  type = 'd'; break;
    case S_IFREG:  type = '-'; break;
    case S_IFLNK:  type = 'l'; break;
    ...
}
```

`S_IFMT` is the crucial detail: masking with `S_IFMT` keeps *only* the type
bits, so the equality test is not polluted by permission bits. Testing
`mode & S_IFDIR` directly also "works" for directories, but it is wrong for
every other type, because e.g. a directory's mode `040755 & 0100000` is
non-zero yet the file is not a regular file.

**Permission extraction — one bit per test.** The nine permission bits map
directly onto the characters of `"rwxrwxrwx"` in reading order (owner rwx,
group rwx, other rwx), so a single loop covers all nine:

```c
const char *rwx = "rwxrwxrwx";
for (i = 0; i < 9; i++)
    out[i] = (mode & (1 << (8 - i))) ? rwx[i] : '-';
```

The shift direction is the only subtlety: bit `8` is *owner read*, so it
maps to `rwx[0]`, and bit `0` is *other execute*, so it maps to `rwx[8]`.
Hence `8 - i`.

**Special bits override the execute bit.** `S_ISUID (04000)`, `S_ISGID
(02000)` and `S_ISVTX (01000)` sit in the three execute positions. `ls`
replaces the `x` with `s`/`t`, using lower case when the execute bit is
*also* set and upper case when it is not — hence `s` for setuid+executable
and `S` for setuid-only:

```c
if (mode & S_ISUID) out[2] = (mode & S_IXUSR) ? 's' : 'S';
```

The `&` operator is what makes this a single expression: the first `&`
isolates the setuid bit, the second distinguishes the two letter cases.

---

## Feature-3 — Column Display (Down Then Across)

### Q1. The logic of "down then across", and why one loop is not enough

**The goal.** Given `N` filenames, a column width `C = longest_name + 2`,
and a terminal width `W`, fit `cols = W / C` columns and lay the files out
**column-major**: fill column 0 from top to bottom, then column 1, and so on.
This is what the eye expects — it means that reading *down* the first column
gives the alphabetically first run of files.

**The algorithm.**

```
col_width = longest_filename + NAME_SPACING
num_cols  = terminal_width / col_width
num_rows  = (N + num_cols - 1) / num_cols        /* ceiling division */

for r = 0 .. num_rows-1:          /* outer loop: ROW   */
    for c = 0 .. num_cols-1:      /* inner loop: COLUMN */
        idx = c * num_rows + r
        if idx < N: print names[idx] padded to col_width
    newline
```

The single line that carries the whole idea is `idx = c * num_rows + r`.
Because a column holds `num_rows` items, skipping to column `c` means
adding `c * num_rows` to the base offset of row `r`.

The ceiling division matters: when `N` is not a multiple of `num_cols`,
GNU `ls` puts the leftover items in the *first* columns and lets the last
column be short. With `idx = c * num_rows + r` and an `idx < N` bounds
check this happens automatically — no special case is required.

**Why a single loop is insufficient.** A natural one-pass loop,
`for i in 0 .. N-1: print names[i]`, walks the array in memory order. That
is **row-major**: it fills row 0 left-to-right, then row 1. That is *not*
the same thing. The distinction only becomes visible when you compare
against the real `ls` on a non-square grid:

```
row-major (wrong)        column-major (correct)
a b c d e                a c e
f g h i j                b d f
                         g i j
                         h
```

Three concrete reasons the single loop cannot simply be "the same":

1. **The number of rows must be known before printing starts.** In
   row-major output the row changes exactly every `num_cols` items, so a
   counter alone would work — but the *row-major* layout is not what `ls`
   uses. Producing the column-major layout requires knowing `num_rows` so
   that the stride between two items in the same row (`num_rows`) can be
   applied.
2. **The terminal can be resized.** Because both dimensions are recomputed
   from `ioctl()` on every run, the mapping from index to screen position
   is `c = i / num_rows`, `r = i % num_rows`. When the width changes,
   `num_rows` changes, and therefore the *entire* index-to-position
   mapping changes. In row-major the index-to-position map is trivially
   stable, which is precisely why it is the wrong layout for this output.
3. **Ragged edges must be handled.** In column-major, early columns can be
   one row longer than the last one; the `idx < N` check handles it.

The nested loop is required because the output is a *transposition* of the
input order. There is no way to produce it while visiting the array only
once in order.

### Q2. Why `ioctl()`, and the cost of a fixed-width fallback

**What `ioctl()` does here.** `ioctl()` is the generic device-control entry
point; a terminal driver is just another device. Passing the request code
`TIOCGWINSZ` ("Terminal Input/Output Get Window Size") makes the driver fill
in a `struct winsize`:

```c
struct winsize ws;
if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    return 80;                       /* fallback */
return ws.ws_col;
```

We query file descriptor **1** (`STDOUT_FILENO`) specifically, because that
is the stream the output is destined for — the terminal width that matters is
the width of wherever the text is going.

Alternatives and why they are worse:

- `system("tput cols")` forks a shell, execs `tput`, and parses its output —
  three extra processes and a `PATH` search on *every* invocation of a tool
  meant to be instant.
- Reading the `COLUMNS` environment variable is fragile: it is not exported
  by default and can be stale after a resize.
- Hard-coding 80 loses all responsiveness.

**Limitations of a fixed 80-column fallback:**

1. **Horizontal overflow.** In an 80-column terminal an 80-wide layout fits
   exactly. In a **200-column** terminal the same layout wastes 120 columns
   and forces the user to scroll sideways through information that would
   have fit — a 60 % loss of usable screen.
2. **Wrapping and corrupted layout.** In a **40- or 60-column** terminal the
   computed columns are 33 characters wide plus spacing. When the row exceeds
   the real width the terminal itself performs a soft wrap, breaking the
   grid: two logical rows fuse into one visual row and every subsequent line
   is offset, so the output is no longer column-aligned at all.
3. **No adaptation to embedded terminals.** The layout is wrong in every
   context that is not an 80-column window: an SSH session into a phone
   terminal, a split `tmux` pane, an editor's embedded terminal, a
   `less` preview pane.
4. **Lost density.** For short filenames such as `f1 … f40`, an 80-column
   assumption yields 14 columns when the real terminal can show 20 at
   120 columns and 40 at 200 — forcing far more scrolling than necessary.
5. **Silently wrong, not an error.** Because the fixed path always
   *produces* output, nothing reports the problem. The user sees a
   misformatted listing rather than a diagnostic, which is the worst kind of
   failure for a system utility.

Our guard `if (width <= col_width) { one per line }` handles the extreme
case where even a single name cannot share a line, but it does **not**
rescue the general case: a fixed fallback is a behaviour, not a safe
default.

---

## Feature-4 — Horizontal Display (`-x`)

### Q1. Implementation complexity: down-then-across vs across

**Down-then-across needs substantially more pre-computation**, for one
reason: it is a **transposition**. The output position of item `i` is
`(row, column) = (i % num_rows, i / num_rows)` — note the **modulo on the
row**. Every single item therefore requires knowing `num_rows`, which
requires knowing `num_cols`, which requires knowing both the longest
filename *and* the terminal width. All of it must be resolved *before the
first byte is printed*, and the nested row-outer / column-inner loop is
required to emit the grid in the right sequence.

**Across needs only `num_cols`.** Its mapping is
`(row, column) = (i / num_cols, i % num_cols)` — the division is on the
column instead. Crucially, `num_cols = width / col_width` depends only on
values already in hand, so nothing has to wait on anything else. There is no
row count, no nested loop, and the index-to-position map is a pure function
of the index, so it cannot drift.

Concrete differences:

| | down-then-across | across |
|---|---|---|
| Values pre-computed | `col_width`, `num_cols`, `num_rows` | `col_width`, `num_cols` |
| Loop structure | nested (row outer, column inner) | single linear loop |
| Index arithmetic | `c * num_rows + r` — multiply + add | `i % num_cols` — modulo only |
| Row count needed? | **yes** | no |

**A bug this difference causes, worth noting.** My first `-x` implementation
followed the brief's wording literally and tracked a running cursor
`used`. It was subtly wrong. A *padded* item occupies the full `col_width`
on screen, but an *unpadded* item occupies only its own length. If `used`
counts the padded case correctly and the unpadded case only by `len()`, then
after a line-final bare item `used` is no longer a multiple of `col_width`,
and every later wrap test is off by the missing padding — producing
`alpha.txtsub2` with no gap. Deriving `col = i % num_cols` from the index
removes the whole class of error, because the mapping can no longer drift.
Vertical mode is immune because *every* item there is padded to `col_width`.

### Q2. Managing the display modes in code

I used a single **enum** rather than a set of booleans:

```c
typedef enum {
    MODE_COLUMN,    /* default */
    MODE_LONG,      /* -l */
    MODE_ACROSS     /* -x */
} DisplayMode;

static DisplayMode display_mode = MODE_COLUMN;
```

**Why not three booleans.** The modes are mutually exclusive, and booleans
cannot express a three-way choice cleanly: with `long_flag` and `across_flag`
you would need a fourth variable plus a priority rule to decide which flag
"wins" when both are set. An enum makes the state space explicit and
impossible to represent a contradictory combination, and the compiler warns
if `DisplayMode` is extended without updating the `switch`.

**The three-stage flow:**

1. **`getopt()` populates the state.** The optstring `"lxR"` declares which
   letters are valid; a `:` after a letter would declare that it takes an
   argument. Each `case` assigns the mode:

   ```c
   while ((opt = getopt(argc, argv, "lxR")) != -1) {
       switch (opt) {
           case 'l': display_mode = MODE_LONG;   break;
           case 'x': display_mode = MODE_ACROSS; break;
           case 'R': recursive = 1;              break;
           case '?': /* usage */ return EXIT_FAILURE;
       }
   }
   ```

2. **`do_ls()` builds the array once, then dispatches.** All three modes
   receive the *same* `NameList`; the display function is chosen by a single
   `if / else if` chain after reading is complete:

   ```c
   if      (display_mode == MODE_LONG)   { /* loop calling print_long_entry */
   else if (display_mode == MODE_ACROSS) print_across(&nl, path);
   else                                   print_columns(&nl, path);
   ```

3. **`-R` is deliberately a separate boolean, not a fourth enum value.**
   Recursion is *orthogonal* to display mode: `-lR` must mean long listing
   **and** recursion simultaneously. Encoding it as an enum value would
   require eight combinations (`MODE_LONG_R`, `MODE_ACROSS_R`, …) of which
   five are meaningless. Separating the two keeps the state space at 3 × 2.

Because sorting is applied *once*, centrally, before the dispatch, all three
modes are alphabetically sorted without a single change inside the display
functions — the single most important consequence of separating *data
preparation* from *data presentation*.

**A known simplification:** the order of the flags on the command line
decides the winner, so `ls -l -x` ends up horizontal. Real GNU `ls` rejects
that combination outright.

---

## Feature-5 — Alphabetical Sort

### Q1. Why read everything into memory first, and the drawbacks

**Why it is necessary.** Sorting is not a property of a file; it is a
*relation between* files, so you must be able to compare any two of them.
`readdir()` gives us no such access:

- It returns entries **one at a time**, and the `struct dirent` it returns
  points into a **single internal buffer that the next call overwrites**.
  Storing the pointer would make every entry alias the last name read.
- The stream is **forward-only**. There is no seek, no rewind and no
  "go back to entry 3", so an entry read earlier is simply gone unless we
  copied it — which is exactly what `strdup()` does.
- The kernel returns entries in **whatever order the filesystem stores
  them** — inode order on ext4, hash order on XFS — not a meaningful one.
  That order is an implementation detail we must not expose to the user.

So materialising the names into an array is the only place sorting *can*
happen. (GNU `ls` solves the same problem differently: it reads the whole
directory too, and if the result is too large it re-opens the directory and
sorts in **bounded chunks**, merging them — a merge sort instead of
`qsort`.)

**Drawbacks for directories with millions of files.**

1. **Memory.** Each entry costs a `strdup()` allocation (~32 bytes of
   allocator overhead plus the name) *and* an 8-byte pointer in the array.
   At 10 million entries that is roughly 400 MB. The array is grown by
   doubling, so during a realloc of capacity *n* the process transiently
   needs ~1.5 *n* pointers.
2. **Peak = all of it, at once.** The array cannot be freed early, because
   the longest filename and the total count are only known once every entry
   has been seen, and both are required to compute the layout.
3. **Latency.** Nothing is printed until the last entry has been read. On a
   network filesystem with 200 ms latency, listing a large directory feels
   frozen, whereas real `ls` streams progressively.
4. **No streaming fallback.** A directory that genuinely cannot fit in RAM
   cannot be listed at all by this design. (GNU `ls` avoids this by
   falling back to an unsorted listing.)
5. **Sort cost.** `qsort()` is O(*n* log *n*) with no guaranteed bound;
   the doubling `realloc()` strategy is amortised O(1) per push but still
   requires copying up to *n* pointers on the last growth.
6. **Recursion multiplies it.** Under `-R` every level of the tree would
   hold its own array simultaneously. I free each level's array
   **before** recursing, so at most one level's worth is ever live — but
   that also means the paths cannot be revisited after the fact.

### Q2. The `qsort()` comparison function

```c
int cmp_names(const void *a, const void *b);
```

**Signature, forced by the standard.** `qsort` is declared as

```c
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
```

It must be able to sort arrays of *any* element type, so it cannot know
what the elements are; it can only pass generic `void *` addresses.

**How it works.** `qsort` sorts **elements**, and for each comparison it
hands us the **addresses of two elements**. Our elements are `char *`, so
each argument is really a `char **` — which is why the cast happens *twice*:

```
   a  ──▶  (char **)     points AT our char * slot
           │
           └──▶  (char *)   dereference to reach the string itself
```

```c
const char *sa = *(const char *const *)a;
const char *sb = *(const char *const *)b;
return strcmp(sa, sb);
```

Returning `(a - b)` — the classic "compare pointers" shortcut — would be
undefined behaviour here: these are unrelated objects, pointer subtraction
is only defined within one array, and the result can overflow.

**Return contract:** negative if `*a` precedes `*b`, zero if equal, positive
if it follows. `strcmp()` already returns exactly that.

**Why `const void *` specifically:**

- **`void *`** — the generic, type-erased pointer. `qsort` is type-agnostic
  by design; `const` on top of it is a *promise*, not a limitation.
- **`const`** — `qsort` **sorts in place** but never modifies the
  *elements*; it only moves them. Declaring the parameters `const` states
  that guarantee to the compiler, which (a) prevents us from accidentally
  writing through them, and (b) allows the function to be passed a genuinely
  `const` array without a cast. `const char *const *` is the most precise
  spelling: the pointer and the pointed-to string are both read-only.

---

## Feature-6 — Colourised Output

### Q1. How ANSI escape codes work; the sequence for green

A terminal is fundamentally a **character grid**. It displays bytes; it has
no colour primitive, no framebuffer and no drawing API. Colour is an
*illusion produced by convention*, inherited from the DEC VT100.

Any byte sequence beginning with **ESC (0x1B)** is not displayed. The
terminal **consumes** it and interprets it as a command that changes the
terminal's internal state — much like changing a printer setting — after
which the screen is repainted in the new state. The general shape is:

```
ESC  [  <parameter bytes>  <final byte>
0x1B 0x5B                    e.g. 'm'
```

`0x1B` introduces a **CSI (Control Sequence Introducer)**. The colour
command is **SGR (Select Graphic Rendition)**, final byte `m`, whose
parameters are semicolon-separated:

| Parameter | Meaning | Parameter | Meaning |
|---|---|---|---|
| `0` | reset everything | `30`–`37` | normal fg: black, red, green, yellow, blue, magenta, cyan, white |
| `1` | bold / bright | `40`–`47` | normal bg |
| `2` | faint | `90`–`97` | bright fg |
| `7` | **reverse video** (swap fg/bg) | `100`–`107` | bright bg |
| `22` | bold off | `39` | default fg |
| `27` | reverse off | `49` | default bg |

**The sequence for green is `\033[0;32m`** — `0` (reset), `;`, `32`
(normal green). `\033` is C's octal escape for byte `0x1B`; `\x1B` is
equivalent. It must be followed by the text, then **`\033[0m`** to restore
the default.

**The consequence that matters for this project:** escape sequences are
written to stdout like any other bytes. If stdout is a file or a pipe,
*nothing interprets them* and `ls > out.txt` would contain raw control
garbage. Real `ls` guards against this with `isatty(1)`; so do we:

```c
static int use_color(void) { return isatty(STDOUT_FILENO); }
```

Colours used: `\033[1;34m` directory (bold blue), `\033[1;32m` executable
(bold green), `\033[1;31m` archive (bold red), `\033[1;35m` symlink (bold
magenta), `\033[7m` special file (reverse video).

### Q2. Which `st_mode` bits decide whether a file is executable

`st_mode` stores the execute bit **three times**, once per class of user,
because a file can be executable by its owner but not by others. The three
bits are:

| Macro | Octal | Binary position | Meaning |
|---|---|---|---|
| `S_IXUSR` | `0100` | bit 6 | owner may execute |
| `S_IXGRP` | `0010` | bit 3 | group may execute |
| `S_IXOTH` | `0001` | bit 0 | others may execute |

A file is treated as executable if **any** of them is set, matching GNU `ls`:

```c
if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
    return ANSI_EXEC;
```

The `|` combines the three bits into one mask; the single `&` then tests
them all at once. (Equivalently, `ls` colours based on "does anyone have
execute permission".)

Two refinements visible in `ls -l`:

1. **The same bits drive the permission string.** In `mode_to_perms()` the
   `x` in each triplet is exactly `mode & S_IXUSR / S_IXGRP / S_IXOTH`.
2. **Setuid/setgid mask them.** If `S_ISUID` is set, the owner `x` becomes
   `s` (or `S`), so a setuid binary with execute permission prints `s` and is
   *not* green even though it is executable. GNU `ls` reflects this
   faithfully.

**Bit layout of the execute bits, in context:**

```
  octal:  0100 = S_IXUSR    0010 = S_IXGRP    0001 = S_IXOTH
  binary:  000100 000        000 010 000       000 000 001
          (user)             (group)            (other)
  ---rwx rwx  rwx  <-- read/write/execute, three triplets, 9 bits total
```

---

## Feature-7 — Recursive Listing (`-R`)

### Q1. Base cases of a recursive function

A **base case** is the condition under which a recursive function returns
**without calling itself**. Without one, recursion has no natural
termination and the program either crashes or hangs.

The textbook base case is "the input is trivially small". Ours has three
layers, and a production recursive function needs all three:

**1. `opendir()` fails — the primary base case.**
```c
dir = opendir(path);
if (dir == NULL) { perror(path); return; }
```
Recursion only continues into things already proven to be directories, so
in principle every recursive call should succeed. This check is what makes
the function **safe when called directly** from `main()` with a plain file
argument — `ls somefile.txt` reports the error instead of misbehaving.

**2. `"."` and `".."` are skipped — the base case that actually bounds the recursion.**
```c
if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    continue;
```
These two entries are how a directory points at **itself** and at **its
parent**. Following `"."` re-enters the *same* directory, producing
infinite recursion at the same level. Following `".."` walks *upwards
forever*. This is the base case that terminates on a **finite filesystem**:
once back-references are removed from the tree, no cycle exists.

**3. A depth limit — the defensive backstop.**
```c
if (depth >= depth_limit) { fprintf(stderr, ...); return; }
```
On a normal filesystem (1) and (2) are sufficient, but real systems have
bind-mounts and symlink chains that defeat that reasoning. A depth cap
converts a possible stack overflow into a controlled error message. Every
recursive function should have one: the failure mode of a runaway recursion
is a **crash or a hang**, not merely wrong output.

**Resource behaviour.** Each call owns its stack frame and its own `NameList`,
so everything it allocates is released on return. The stack therefore grows
with the **depth** of the tree, not with the number of files — a wide
directory is cheap, a deep one is not. Freeing the array *before*
recursing keeps at most one level's entries live at a time.

### Q2. Why a full path must be constructed before recursing

```c
snprintf(child, sizeof(child), "%s/%s", path, name);
```

Every system call resolves a relative path against the **process's current
working directory** (fd 0/1/2 aside, that is the in-process cwd, and it never
changes in this program). The cwd is **not** the directory being listed —
it is wherever the user happened to run the command.

Therefore, inside `do_ls("parent_dir")`, the string `"subdir"` does not mean
`parent_dir/subdir`. It means `./subdir` — i.e. **the directory the user
typed the command in**. The recursive call would be handed the wrong
directory.

**What actually happens if we call `do_ls("subdir")` from inside
`do_ls("parent_dir")`:**

- `opendir("subdir")` resolves against the cwd. It **succeeds only by
  accident**, if a directory of that name happens to exist in the cwd.
- If it fails, `perror()` fires and the subtree is **silently skipped** — a
  bug that is easy to miss, because the top-level listing still looks
  correct.
- If it succeeds, it lists the **wrong directory**, and its own "children"
  are again resolved against the cwd. With a nested tree
  `a/b/c/d`, running `./bin/ls -R a` from the project root would print the
  header `a:`, list `a`'s children, then for the child `b` call
  `opendir("b")` — which **does not exist in the root**, so the error
  message `ls: cannot open 'b': No such file or directory` appears and the
  recursion dies one level down. Every level would be lost.
- Even worse, if the name is `.` or `..`-like, the failure mode is a hang
  rather than an error.

The cumulative-path rule — always pass `current_path + "/" + entry_name` —
is what makes the path mean the same thing at every depth. It is the same
reason a shell's `cd` inside a recursive function must build an absolute path
if it is to return to where it started.

**Two related safety points in the same function:**

- `snprintf()` bounds the copy and its return value is **checked**. A deep
  tree can exceed the buffer; truncating silently and then `lstat()`-ing the
  truncated path would list the wrong directory.
- `lstat()`, not `stat()`, is used for the type test. With `stat()`, a
  symlink pointing back up the tree would report "directory" and we would
  recurse forever; `lstat()` reports "symlink" and we correctly do not
  descend.

---

## Implementation notes and limitations

**Ordering of `-R` output.** Descending is a **separate loop** after the
display loop, not part of it. In `MODE_COLUMN` the display function chooses
its own internal order (row-first), so descending from inside it would print
the children of `a` before `b` and `c` had even been shown. `ls -R` prints a
whole level, then recurses. The separate loop also means `print_columns()`,
`print_across()` and `print_long_entry()` needed **no changes at all** for
recursion — a good sign that the data/presentation separation was correct.

**Memory.** Every `strdup()` has a matching `free()`; a `realloc()` failure
preserves the original pointer rather than leaking it. LeakSanitizer reports
nothing across all display modes.

**Build.** `-std=c11 -D_GNU_SOURCE -Wall -Wextra -O2`; all seven versions
compile with **zero warnings**.

**Testing.** Every version was built through the real Makefile and run in all
display modes, then checked under AddressSanitizer, UndefinedBehaviorSanitizer
and LeakSanitizer — 75 runs covering symlink cycles, broken links, FIFOs,
names containing spaces, empty directories, unreadable paths and multiple
arguments: **zero findings**. Three genuine bugs were caught this way and are
documented below.

### Three bugs found by testing, and what they taught

1. **`qsort(NULL, 0, ...)` on an empty directory.** `qsort`'s first parameter
   is declared a pointer that "shall not be null", so sorting zero elements
   through a NULL pointer is formally undefined behaviour even though glibc
   tolerates it. UBSan reported
   *"null pointer passed as argument 1, which is declared to never be null"*.
   Fixed by guarding the call with `if (count > 1)` — sorting 0 or 1 element
   is a no-op anyway.

2. **Column misalignment in `-x`.** The first horizontal implementation
   tracked a running cursor `used`, as the brief describes. It produced
   `alpha.txtsub2` — two names glued together with no gap. The cause: a
   *padded* item occupies the full `col_width` on screen, but an *unpadded*
   item occupies only its own length. Counting the first case correctly and
   the second only by `len()` leaves `used` no longer a multiple of
   `col_width`, so every later wrap test is off by the missing padding.
   Deriving the column from the index — `col = i % num_cols` — removes the
   entire class of bug, because the item-to-position mapping becomes a pure
   function of the index and can never drift.

3. **Trailing whitespace.** Padding the final item of a row left visible
   spaces at end of line, which real `ls` never emits and which show up as
   noise in `ls | cat -A`. Fixed by padding only when a real item follows on
   that row.

### Known limitations (deliberate scope reductions)

1. `ls -l -x` lets the last flag win; GNU `ls` rejects the combination.
2. Column width is uniform (the brief specifies "use the length of the
   longest filename"); GNU `ls` optimises per-column widths and can fit
   noticeably more columns.
3. No `-a`, `-t`, `-S`, `-h`, `-1`, `-r`, `-F`, `-i`, `-n`, `-d` or `-G`.
4. `-R` descends into directories, but the top-level argument list is still
   handled by the base's simple loop (no blank-line grouping).
5. The `path:` header is printed unconditionally by `do_ls()`; real `ls`
   prints it only under `-R`.
6. Owner/group columns are fixed at 8 characters; GNU `ls` sizes them to the
   widest actual name.
7. `getpwuid()`/`getgrgid()` results are not cached, so `-l` over a large
   directory performs one `/etc/passwd` lookup per file.