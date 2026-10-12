/*
 * ls-v1.6.0.c  --  Feature-7: Recursive Listing (-R)   [FINAL VERSION]
 *
 * NEW IN THIS VERSION
 *   -R descends into every subdirectory it finds, printing a header for
 *   each one, exactly like `ls -R`.
 *
 * NEW CONCEPTS
 *   Recursion    - do_ls() calls itself with a deeper path.
 *   Base case    - the condition that stops the recursion. Ours is
 *                   "the current path is not a directory", plus the
 *                   explicit skip of "." and "..".
 *   Path building - joining "parent" + "/" + "child" into a new buffer.
 *
 * INHERITED FROM v1.5.0
 *   Everything: dynamic array, ioctl width, -l, -x, qsort, ANSI colours.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/ioctl.h>     /* struct winsize, TIOCGWINSZ */

/*
 * Display mode.
 *
 * We replaced the old `int opt_long` boolean with an enum because we now
 * have THREE mutually exclusive states. A boolean cannot express "neither
 * of the two things I have flags for", so with two booleans we would also
 * need extra code to decide which one wins when both are set. An enum makes
 * the state space explicit and impossible to contradict.
 */
typedef enum {
    MODE_COLUMN,    /* default : down then across */
    MODE_LONG,      /* -l      : one file per line with metadata */
    MODE_ACROSS     /* -x      : left to right, then wrap */
} DisplayMode;

static DisplayMode display_mode = MODE_COLUMN;

/*
 * -R state. This is a SEPARATE boolean, not a fourth enum value, because
 * recursion is an ORTHOGONAL concern: it modifies all three display modes
 * at once, whereas display_mode chooses between them. So `ls -lR` must be
 * able to mean MODE_LONG *and* recursion simultaneously. Encoding it as a
 * fourth enum value would create eight combinations, of which five are
 * meaningless.
 */
static int recursive = 0;

/* Guards against symlink loops and pathological trees (see do_ls). */
static int depth_limit = 64;

/* --- constants --------------------------------------------------------- */
#define NAME_SPACING 2      /* blank columns between adjacent columns      */
#define FALLBACK_WIDTH 80   /* used when the real terminal width is unknown */

/*
 * A growable array of C strings.
 *
 * WHY strdup() AND NOT &name[i]?
 *   readdir() REUSES one internal `struct dirent` buffer every call, so
 *   the d_name pointer is overwritten on the next iteration. Storing that
 *   pointer would leave every array slot pointing at the LAST name read.
 *   strdup() allocates heap memory and copies the bytes, so each slot owns
 *   an independent string. Every strdup() MUST have a matching free().
 */
typedef struct {
    char **names;       /* array of pointers to filenames */
    size_t count;       /* how many entries are filled in   */
    size_t capacity;    /* how many slots allocated so far  */
    size_t max_len;     /* length of the longest filename   */
} NameList;

static void namelist_init(NameList *nl)
{
    nl->names = NULL;
    nl->count = 0;
    nl->capacity = 0;
    nl->max_len = 0;
}

/* Append one filename, growing the array geometrically when it is full. */
static void namelist_push(NameList *nl, const char *name)
{
    if (nl->count == nl->capacity) {
        size_t new_cap = (nl->capacity == 0) ? 8 : nl->capacity * 2;
        char **tmp = realloc(nl->names, new_cap * sizeof(char *));

        if (tmp == NULL) {
            /* realloc failure: the ORIGINAL pointer is still valid and
               still owned by us, so we must not overwrite nl->names or we
               would leak it. */
            perror("realloc");
            exit(EXIT_FAILURE);
        }
        nl->names = tmp;
        nl->capacity = new_cap;
    }

    nl->names[nl->count] = strdup(name);
    if (nl->names[nl->count] == NULL) {
        perror("strdup");
        exit(EXIT_FAILURE);
    }

    if (strlen(name) > nl->max_len)
        nl->max_len = strlen(name);

    nl->count++;
}

/*
 * THE qsort() COMPARISON FUNCTION.
 *
 * SIGNATURE
 *     int cmp_names(const void *a, const void *b);
 *
 * The prototype is fixed by the C standard because qsort must be able to
 * call a function whose argument types it cannot know - it is sorting a
 * byte range of unknown element type.
 *
 * WHAT QSORT PASSES
 *     qsort sorts an array of ELEMENTS, and for each comparison it hands
 *     us the ADDRESSES OF TWO ELEMENTS. Our elements are `char *`, so
 *     `a` is really a `char **`. The cast therefore has to go:
 *
 *         a --> (char **)   now points AT our char * slot
 *          |
 *          +--> (char *)   dereference to get the string itself
 *
 *     Getting one dereference wrong is the single most common bug when
 *     sorting an array of pointers with qsort.
 *
 * WHY const void *
 *     - void *  : qsort has no idea whether the array holds int, double or
 *                 char *, so it can only pass a generic pointer.
 *     - const  : qsort SORTS IN PLACE and never modifies the elements.
 *                 Declaring the parameter const is a promise to the caller
 *                 that we will only read them. It also means our function
 *                 can legally be used with a genuinely const array.
 *
 * THE COMPARISON CONTRACT
 *     return < 0   if *a sorts BEFORE *b
 *     return  0    if they are equal
 *     return > 0   if *a sorts AFTER *b
 *     We must never return (a - b): that is arithmetic on unrelated
 *     addresses, which is undefined behaviour and can overflow. strcmp()
 *     already returns exactly the value we need.
 */
static int cmp_names(const void *a, const void *b)
{
    /* Two dereferences: one to reach the slot, one to reach the string. */
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;

    return strcmp(sa, sb);
}

/* Release every string and the array itself. */
static void namelist_free(NameList *nl)
{
    size_t i;
    for (i = 0; i < nl->count; i++) {
        free(nl->names[i]);   /* the string   */
    }
    free(nl->names);          /* the array    */
    namelist_init(nl);
}

/* --- terminal size ----------------------------------------------------- */

/*
 * Ask the kernel how many columns the terminal has.
 *
 * ioctl() is the generic "device control" entry point. The constant
 * TIOCGWINSZ ("Terminal Input/Output Get Window Size") tells the terminal
 * driver which piece of information we want; it fills in `struct winsize`
 * whose ws_col member holds the column count.
 *
 * WHY ioctl() AND NOT system("tput cols")?
 *   system() would fork a shell, run tput and parse its output on every
 *   invocation. ioctl() is a single syscall with no process spawn.
 *
 * Returns 0 on failure, in which case the caller falls back to a constant.
 */
static int terminal_width(void)
{
    struct winsize ws;

    /* STDOUT_FILENO is fd 1. We query fd 1 because that is the stream the
       output will actually go to. */
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return FALLBACK_WIDTH;
    }
    return ws.ws_col;
}

/*
 * Print the name list in GNU ls "down then across" order.
 *
 * THE ALGORITHM, STEP BY STEP
 *   width      = terminal columns
 *   col_width  = longest filename + NAME_SPACING   (every column is this
 *                wide so they line up)
 *   num_cols   = width / col_width               (how many fit)
 *   num_rows   = (count + num_cols - 1) / num_cols (ceiling division)
 *
 *   A single loop `for i in 0..count` prints FILES left-to-right, which is
 *   "across then down" and looks wrong: the last column would be ragged
 *   and the row contents shift every time the terminal is resized.
 *
 *   "Down then across" fills column 0 top to bottom, then column 1, etc.
 *   With N rows, the element at (row r, column c) is simply:
 *
 *              index = c * num_rows + r
 *
 *   So we loop ROW-first on the outside and COLUMN-second on the inside:
 *       for r in 0..num_rows:  for c in 0..num_cols:  print names[c*num_rows + r]
 *
 *   GNU ls puts any leftover entries in the FIRST columns, so early
 *   columns are one row taller. The `c * num_rows + r` formula plus
 *   bounds-checking reproduces that automatically.
 */
/* --- ANSI escape codes ------------------------------------------------- */

/*
 * HOW TERMINAL COLOUR WORKS
 *
 * A terminal is fundamentally a character grid: it displays bytes. It has
 * no colour primitive of its own. The illusion of colour is produced by a
 * convention inherited from the DEC VT100: a byte sequence beginning with
 * ESC (0x1B) is not displayed as text but is CONSUMED by the terminal as a
 * command that changes the terminal's internal state - much like changing
 * a printer setting. The screen is then repainted in that new state.
 *
 * The general shape is:
 *
 *     ESC [ <parameter bytes> <final byte>
 *
 *   ESC       = 0x1B, written in C as "\033" or "\x1B"
 *   '['       = introduces a CSI (Control Sequence Introducer)
 *   final byte= the command letter
 *
 * The SGR ("Select Graphic Rendition") command is the colour one, final
 * byte 'm'. Its parameters are semicolon separated:
 *
 *     0   reset everything           30-37  normal fg colours
 *     1   bold / bright              40-47  normal bg colours
 *     2   faint                     90-97  bright fg colours
 *     7   reverse video             40-47  bright bg colours
 *     22  bold off                  39     default fg
 *
 * The "0" before the colour number means "reset, then set", which is why
 * the textbook green is "\033[0;32m" rather than the shorter "\033[32m".
 *
 * CONSEQUENCE FOR US: the escape bytes go to STDOUT like any other bytes.
 * If stdout is a pipe or a file, nothing interprets them, so a redirected
 * `ls > out.txt` would contain literal control garbage. Real ls guards
 * against this with isatty(); so do we, in use_color() below.
 */
#define ANSI_RESET     "\033[0m"
#define ANSI_DIR       "\033[1;34m"    /* bold blue                     */
#define ANSI_EXEC      "\033[1;32m"    /* bold green                    */
#define ANSI_ARCHIVE   "\033[1;31m"    /* bold red                      */
#define ANSI_LINK      "\033[1;35m"    /* bold magenta (pink)           */
#define ANSI_SPECIAL   "\033[7m"       /* reverse video                 */

/* Archive suffixes the brief asks for, plus the common compound ones. */
static const char *const archive_ext[] = {
    ".tar", ".gz", ".zip", ".tgz", ".bz2", ".xz", ".tar.gz", ".tar.bz2", NULL
};

/*
 * TRUE if `name` ends with one of the archive suffixes.
 *
 * strrchr(name, '.') finds the LAST dot - the correct one, because
 * "report.2024.final" must be tested against "final", not against "2024".
 * We then compare that tail against each suffix with an explicit length
 * check plus strcmp, which is an equality test AT THE END. strstr() alone
 * would wrongly match a substring in the middle, so "my.gzfile" would be
 * coloured as if it were an archive.
 */
static int has_archive_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    int i;

    if (dot == NULL)
        return 0;

    for (i = 0; archive_ext[i] != NULL; i++) {
        size_t nl = strlen(dot);
        size_t el = strlen(archive_ext[i]);
        if (nl == el && strcmp(dot, archive_ext[i]) == 0)
            return 1;
    }
    return 0;
}

/*
 * Decide the colour for one entry from its already-fetched st_mode.
 *
 * ORDER MATTERS - the tests are ordered most specific to least, because
 * each one narrows what can still be true:
 *   1. symlink   - a link pointing at a directory must STILL be pink, so
 *                  this must be tested BEFORE the directory test.
 *   2. directory
 *   3. special   - character/block devices, FIFOs, sockets.
 *   4. executable - via the execute bits; only meaningful for regular files.
 *   5. archive   - matched on the NAME, because the filesystem itself has
 *                  no concept of "archive"; only the name does.
 *   6. default   - ordinary data file, terminal's own colour.
 *
 * We take st_mode as a parameter rather than the path, so the caller can
 * lstat() ONCE per entry and reuse the result. A version that called
 * lstat() inside this function would work too, but would be slower when
 * the caller also needs the metadata.
 */
static const char *color_for_mode(const char *name, mode_t mode)
{
    if (S_ISLNK(mode))                      return ANSI_LINK;
    if (S_ISDIR(mode))                      return ANSI_DIR;
    if (S_ISCHR(mode) || S_ISBLK(mode) ||
        S_ISFIFO(mode) || S_ISSOCK(mode))   return ANSI_SPECIAL;

    /*
     * EXECUTABLE TEST.
     * st_mode stores the execute bit THREE times - once per class of user -
     * because a file can be executable by its owner but not by others. If
     * ANY of the three is set we call it executable, matching GNU ls:
     *     S_IXUSR   owner may execute    0100 octal
     *     S_IXGRP   group may execute    0010 octal
     *     S_IXOTH   others may execute   0001 octal
     */
    if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        return ANSI_EXEC;

    if (has_archive_ext(name))             return ANSI_ARCHIVE;

    return "";                              /* terminal default colour */
}

/*
 * Colour only when stdout is a real terminal.
 *
 * isatty(1) is true only when file descriptor 1 is attached to a terminal.
 * Redirecting to a file or piping into another program makes it false, and
 * we then suppress all escape codes. This MUST exist, otherwise
 * `ls > out.txt` writes raw ESC bytes into the file.
 */
static int use_color(void)
{
    return isatty(STDOUT_FILENO);
}

/*
 * Print one filename in its colour, optionally padded to `col_width`.
 *
 * ALIGNMENT SUBTLETY - the most important detail in this feature.
 * The escape codes are invisible on screen but they DO count as bytes.
 * Passing an already-coloured string to `%-*s` would therefore mis-pad
 * every column by the length of the escape sequence and the whole grid
 * would collapse. The fix is to emit the colour code, pad and print ONLY
 * the bare name, then emit the reset code AFTER the padding. The terminal
 * renders the escape bytes as zero-width, so the visible width is exactly
 * col_width.
 */
static void print_colored_padded(const char *path, const char *name,
                                 int col_width, int pad)
{
    struct stat st;
    const char *code = "";

    /* The isatty() gate lives HERE, in the one place every columnar print
       funnels through, rather than being repeated at each call site.
       When stdout is a pipe or a file we emit no escape bytes at all, so
       `ls > out.txt` contains clean text. */
    if (use_color() && lstat(path, &st) == 0) {
        code = color_for_mode(name, st.st_mode);
    }
    /* If lstat failed we still print the name, just without colour - one
       unreadable entry must not abort the whole listing. */

    if (code[0] != '\0')
        fputs(code, stdout);

    if (pad)
        printf("%-*s", col_width, name);
    else
        printf("%s", name);

    if (code[0] != '\0')
        fputs(ANSI_RESET, stdout);
}

/* Colour only when stdout is a real terminal. */
/* Print the name list in GNU ls "down then across" order. */
static void print_columns(const NameList *nl, const char *path)
{
    int width = terminal_width();
    size_t col_width;
    size_t num_cols;
    size_t num_rows;
    size_t r, c;

    if (nl->count == 0)
        return;

    /* Guard against a single filename longer than the terminal: at least
       one column, and shrink the width so we never divide by zero. */
    col_width = nl->max_len + NAME_SPACING;
    if ((size_t)width <= col_width) {
        /* Names too wide to share a line - print one per line. */
        size_t i;
        for (i = 0; i < nl->count; i++) {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", path, nl->names[i]);
            print_colored_padded(full, nl->names[i], 0, 0);
            putchar('\n');
        }
        return;
    }

    num_cols = (size_t)width / col_width;
    /* Ceiling division: (a + b - 1) / b rounds a/b up. */
    num_rows = (nl->count + num_cols - 1) / num_cols;

    for (r = 0; r < num_rows; r++) {
        for (c = 0; c < num_cols; c++) {
            size_t idx = c * num_rows + r;
            char full[4096];

            if (idx >= nl->count)
                continue;              /* ragged last column */

            snprintf(full, sizeof(full), "%s/%s", path, nl->names[idx]);

            /* TRAILING-WHITESPACE FIX.
               Pad only when there is a REAL item further along this row.
               Two conditions must both hold:
                 - a next column exists on this row      (c + 1 < num_cols)
                 - that next column actually has a name  (idx+num_rows < count)
               Padding the final item of a row would leave spaces after the
               colour reset, which real ls never emits and which show up as
               noise in `ls | cat -A`. */
            if (c + 1 < num_cols && idx + num_rows < nl->count)
                print_colored_padded(full, nl->names[idx], (int)col_width, 1);
            else
                print_colored_padded(full, nl->names[idx], 0, 0);
        }
        putchar('\n');
    }
}

/*
 * Print the name list left-to-right, wrapping at the terminal edge.
 *
 * WHY THIS IS FAR SIMPLER THAN print_columns()
 *   print_columns() must know num_rows BEFORE it prints anything, because
 *   row r / column c maps to index c * num_rows + r. num_rows depends on
 *   num_cols, which depends on the longest name AND the terminal width -
 *   so it has to compute a full two-dimensional layout up front.
 *
 *   Here the mapping is trivial: index i goes to (row i / num_cols,
 *   column i % num_cols). We never need to know the total number of rows
 *   at all. Instead we keep ONE running variable, `used`, holding how many
 *   characters we have already written on the current line. Before printing
 *   the next name we ask: "would this name + padding still fit?" If not, we
 *   emit '\n' and reset used to zero. That is the whole algorithm - it is
 *   a streaming, single-pass solution with no look-ahead.
 */
static void print_across(const NameList *nl, const char *path)
{
    int width = terminal_width();
    size_t col_width;
    size_t num_cols;
    size_t i;

    if (nl->count == 0)
        return;

    col_width = nl->max_len + NAME_SPACING;

    /* Same degenerate case as before: a name wider than the screen. */
    if ((size_t)width <= col_width) {
        for (i = 0; i < nl->count; i++) {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", path, nl->names[i]);
            print_colored_padded(full, nl->names[i], 0, 0);
            putchar('\n');
        }
        return;
    }

    num_cols = (size_t)width / col_width;

    for (i = 0; i < nl->count; i++) {
        size_t col = i % num_cols;   /* which column of the line */
        char full[4096];
        int pad;

        /* WRAP: index i starts a new line whenever it sits in column 0 and
           is not the very first item. This is the horizontal equivalent of
           the brief's "track the cursor and print '\n' when the next name
           would not fit". Deriving the column from the index is what keeps
           the grid aligned - see the note below. */
        if (col == 0 && i > 0)
            putchar('\n');

        /*
         * PADDING - and why we deliberately do NOT track a running cursor.
         *
         * The intuitive version keeps a variable `used` holding the current
         * column and pads while `used + col_width <= width`. That looks
         * right but is WRONG, and the reason is worth understanding: a
         * padded item occupies the FULL col_width on screen, whereas an
         * unpadded item occupies only its own length. If `used` counts the
         * padded case correctly but the unpadded case only by len(), then
         * after a line-final bare item `used` is no longer a multiple of
         * col_width, and every later wrap test is off by the missing
         * padding. The visible symptom is a long name glued onto the next
         * one ("alpha.txtsub2") with no gap between them.
         *
         * Deriving the column from the index - col = i % num_cols - removes
         * the entire class of bug, because the item-to-position mapping is
         * a pure function of the index and can never drift. Padding then
         * reduces to "is this the last column that still has content?"
         */
        pad = (col + 1 < num_cols) && (i + 1 < nl->count);

        snprintf(full, sizeof(full), "%s/%s", path, nl->names[i]);
        print_colored_padded(full, nl->names[i], pad ? (int)col_width : 0, pad);
    }

    /* Trailing newline so the shell prompt starts on a fresh line. */
    putchar('\n');
}

/* --- long listing (unchanged from v1.1.0) ------------------------------ */

static void mode_to_perms(mode_t mode, char out[11])
{
    const char *rwx = "rwxrwxrwx";
    int i;

    for (i = 0; i < 9; i++)
        out[i] = (mode & (1 << (8 - i))) ? rwx[i] : '-';
    out[9] = '\0';

    if (S_ISDIR(mode))       out[0] = 'd';
    else if (S_ISLNK(mode))  out[0] = 'l';
    else if (S_ISCHR(mode))  out[0] = 'c';
    else if (S_ISBLK(mode))  out[0] = 'b';
    else if (S_ISFIFO(mode)) out[0] = 'p';
    else if (S_ISSOCK(mode)) out[0] = 's';

    if (mode & S_ISUID) out[2] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID) out[5] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX) out[8] = (mode & S_IXOTH) ? 't' : 'T';
}

static void format_mtime(time_t mtime, char *buf, size_t bufsz)
{
    char *raw = ctime(&mtime);
    char month[4];

    if (raw == NULL) {
        snprintf(buf, bufsz, "            ");
        return;
    }

    memcpy(month, raw + 4, 3);
    month[3] = '\0';

    if (mtime > time(NULL) - 15552000L) {
        snprintf(buf, bufsz, "%s %2d %02d:%02d",
                 month, atoi(raw + 8), atoi(raw + 11), atoi(raw + 14));
    } else {
        snprintf(buf, bufsz, "%s %2d  %s", month, atoi(raw + 8), raw + 20);
    }
}

static void print_long_entry(const char *path, const char *name)
{
    struct stat st;
    char perms[11];
    char timebuf[32];
    struct passwd *pw;
    struct group  *gr;

    if (lstat(path, &st) != 0) {
        fprintf(stderr, "ls: cannot access '%s': %s\n", path, strerror(errno));
        return;
    }

    mode_to_perms(st.st_mode, perms);
    format_mtime(st.st_mtime, timebuf, sizeof(timebuf));

    pw = getpwuid(st.st_uid);
    gr = getgrgid(st.st_gid);

    /*
     * All the metadata columns are printed plainly; only the NAME is
     * coloured, and it is coloured using the SAME st_mode we already
     * fetched above - no second lstat() call is needed.
     */
    printf("%s %3lu %-8s %-8s %9ld %s ",
           perms, (unsigned long)st.st_nlink,
           pw ? pw->pw_name : "UNKNOWN",
           gr ? gr->gr_name : "UNKNOWN",
           (long)st.st_size, timebuf);

    if (use_color()) {
        const char *code = color_for_mode(name, st.st_mode);
        if (code[0] != '\0')
            fputs(code, stdout);
    }

    /* In -l mode the name is the last column, so it needs no padding -
       a width of 0 with pad=0 prints it bare. */
    fputs(name, stdout);

    if (use_color())
        fputs(ANSI_RESET, stdout);

    if (S_ISLNK(st.st_mode)) {
        char target[1024];
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            printf(" -> %s", target);
        }
    }
    printf("\n");
}

/* ------------------------------------------------------------------------ */

/*
 * do_ls()  --  list ONE directory.  This is the function that recurses.
 *
 * A BASE CASE is the condition that makes a recursive function stop
 * calling itself. Ours has three layers, and every real recursive program
 * needs all three:
 *
 *   1. opendir() FAILS.  The path is not a directory, or is unreadable.
 *      This is the primary base case: recursion only continues into
 *      things we have ALREADY proven are directories, so by construction
 *      every call should succeed - the check is what makes the function
 *      safe when called directly from main() on a plain file argument.
 *
 *   2. "." AND ".." ARE SKIPPED.  These are the special entries through
 *      which a directory points at itself and at its parent. Following
 *      either would make the tree infinite: ".." would walk forever
 *      upwards. Skipping them is what bounds the recursion on a FINITE
 *      filesystem - a finite tree with back-references removed cannot
 *      cycle.
 *
 *   3. THE DEPTH LIMIT.  Real filesystems can contain symlink chains or
 *      bind-mount arrangements that defeat the ".." reasoning (e.g. a
 *      directory that is its own ancestor through a different path).
 *      depth is a belt-and-braces backstop: after N levels we stop. Every
 *      recursive function should have one, because the cost of a runaway
 *      recursion is a stack overflow and a hang, not just wrong output.
 *
 * Each call owns its own frame and its own `nl`, so on return everything
 * it allocated is released. The call stack grows with the DEPTH of the
 * tree, not with the number of files - which is why a wide directory is
 * cheap but a very deep one is not.
 */
static void do_ls(const char *path, int depth)
{
    DIR *dir;
    struct dirent *entry;
    NameList nl;

    /* BASE CASE 1 - not an openable directory. */
    dir = opendir(path);
    if (dir == NULL) {
        perror(path);
        return;
    }

    /* BASE CASE 3 - refuse to go deeper than the limit. */
    if (depth >= depth_limit) {
        fprintf(stderr, "ls: maximum recursion depth (%d) reached at %s\n",
                depth_limit, path);
        closedir(dir);
        return;
    }

    /* `-R` prints a header for every directory it lists, not just the
       first. Real ls writes it in the form "path:" followed by a newline. */
    printf("%s:\n", path);

    namelist_init(&nl);

    /*
     * PHASE 1 - READ EVERYTHING.
     * We cannot start printing until we know the LONGEST filename,
     * because the column width depends on it. So the whole directory is
     * slurped into memory first, then printed. (Feature-5 adds sorting
     * to this same phase.)
     */
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (entry->d_name[0] == '.')
            continue;
        namelist_push(&nl, entry->d_name);
    }
    if (errno != 0)
        perror("readdir");
    closedir(dir);

    /*
     * PHASE 1b - SORT.
     *
     * WHY sort HERE and not inside each display function?
     *   Sorting mutates the array in place and is expensive. Doing it once,
     *   in the single place where the array is finished being built, means
     *   every display mode downstream sees an already-correct order and
     *   none of them needs to know that sorting even exists.
     *
     * WHY we must read everything into memory first?
     *   readdir() returns entries in whatever order the filesystem hands
     *   them over (inode order on ext4, hash order on XFS) and there is no
     *   way to seek or re-order a live directory stream. The only place
     *   sorting can happen is on a materialised array.
     *
     * The arguments: base pointer, element COUNT, element SIZE in bytes,
     * then the comparison function pointer (note: no '&' needed, a
     * function name already decays to a pointer).
     *
     * HIDDEN FILES: our reader already skips names beginning with '.', but
     * the comparator is completely indifferent to that - strcmp orders any
     * string, and a leading '.' simply sorts before letters, which is why
     * real ls shows ".bashrc" first. Enabling -a later would need no change
     * here at all.
     */
    /*
     * GUARD: qsort's first parameter is declared `void *` that "shall not be
     * a null pointer", so calling qsort(NULL, 0, ...) on an EMPTY directory
     * is formally undefined behaviour even though glibc happens to tolerate
     * it. UBSan reports it as
     *     "null pointer passed as argument 1, which is declared to never
     *      be null".
     * Sorting 0 or 1 elements is a no-op anyway, so skipping the call is
     * both correct and clearer. (Testing count > 1 also guarantees the
     * array is non-NULL, since namelist_push is what allocates it.)
     */
    if (nl.count > 1)
        qsort(nl.names, nl.count, sizeof(char *), cmp_names);

    /* PHASE 2 - PRINT.  Dispatch on the display mode. */
    if (display_mode == MODE_LONG) {
        size_t i;
        for (i = 0; i < nl.count; i++) {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", path, nl.names[i]);
            print_long_entry(full, nl.names[i]);
        }
    } else if (display_mode == MODE_ACROSS) {
        print_across(&nl, path);
    } else {
        print_columns(&nl, path);
    }

    /*
     * PHASE 3 - DESCEND.
     *
     * A SEPARATE LOOP, not part of the printing loop above. Two reasons:
     *   1. In MODE_COLUMN the display function decides its OWN internal
     *      iteration order (row-first, column-second). Descending from
     *      inside that loop would produce "a b c / a1 b1 c1" - children of
     *      'a' printed before 'b' and 'c' have even been shown. `ls -R`
     *      prints the whole level first, THEN recurses. A separate loop
     *      gives us that ordering naturally.
     *   2. It keeps the display functions pure with respect to recursion -
     *      no change was needed inside print_columns/print_across/
     *      print_long_entry at all.
     *
     * THE LOOP MUST SEE A CLEAN STATE. namelist_free() must run BEFORE we
     * recurse, otherwise every level of the tree holds its own array and
     * its own strings in memory simultaneously - which is exactly the
     * unbounded growth the brief warns about. Freeing here means at most
     * one level's worth of directory entries is live at any moment.
     */
    if (recursive) {
        size_t i;
        for (i = 0; i < nl.count; i++) {
            struct stat st;
            char child[4096];

            /* Redundant with the readdir filter above, but this is the
               single most important guard in the whole program. "." and
               ".." are directories; recursing into ".." returns to the
               PARENT, and recursing into "." returns to THIS SAME
               directory. The second case is an infinite loop - the base
               case that would never terminate. */
            if (strcmp(nl.names[i], ".") == 0 || strcmp(nl.names[i], "..") == 0)
                continue;

            /*
             * PATH CONSTRUCTION - the crux of recursive listing.
             * Every system call resolves relative paths against the
             * process's CURRENT WORKING DIRECTORY, which never changes
             * here. So "sub" means "./sub", NOT "parent_dir/sub".
             * Passing the bare name down would make every nested call
             * operate on the same directory in the CWD. The only correct
             * argument is an absolute-or-cumulative path built as
             *     "<current path>/<entry name>".
             * snprintf bounds the copy so a deep tree cannot overflow the
             * buffer; the return value is checked so we never truncate
             * silently and then stat a wrong path.
             */
            if (snprintf(child, sizeof(child), "%s/%s", path, nl.names[i])
                    >= (int)sizeof(child)) {
                fprintf(stderr, "ls: path too long, skipping %s/%s\n",
                        path, nl.names[i]);
                continue;
            }

            /*
             * lstat(), NOT stat(), for the type test. If a subdirectory
             * contains a symlink back up towards the root, stat() would
             * report "directory" and we would follow it. lstat() reports
             * "symlink" instead, so we do not recurse into it - which is
             * what breaks symlink cycles such as  d/link -> ../d.
             */
            if (lstat(child, &st) != 0)
                continue;            /* vanished or unreadable: skip quietly */

            if (S_ISDIR(st.st_mode))
                do_ls(child, depth + 1);   /* THE RECURSIVE CALL */
        }
    }

    namelist_free(&nl);
}

int main(int argc, char const *argv[])
{
    int opt;

    /*
     * The optstring "lxR" tells getopt() which letters are valid. A ':'
     * after a letter would mean that letter TAKES an argument; none of ours
     * do, so the string is just the three characters. Anything else the
     * user types comes back as '?' and we reject it.
     *
     * The cast is required, not cosmetic: getopt() is declared as
     *     int getopt(int, char *const *, const char *)
     * but the base version main() is
     *     int main(int argc, char const *argv[])
     * which makes argv a `const char **` - an incompatible pointer type.
     * The cast is safe because getopt() may PERMUTE argv and substitute
     * option values, but it never modifies the strings themselves.
     *
     * Note the difference between the two kinds of state:
     *   display_mode is MUTUALLY EXCLUSIVE - -l and -x fight each other.
     *   recursive     is INDEPENDENT     - it combines with either.
     */
    while ((opt = getopt(argc, (char *const *)argv, "lxR")) != -1) {
        switch (opt) {
            case 'l':
                display_mode = MODE_LONG;
                break;
            case 'x':
                display_mode = MODE_ACROSS;
                break;
            case 'R':
                recursive = 1;
                break;
            case '?':
            default:
                fprintf(stderr, "usage: %s [-lxR] [DIRECTORY]\n", argv[0]);
                return EXIT_FAILURE;
        }
    }

    /*
     * The order of the flags decides the winner, so `ls -l -x` ends up
     * horizontal. Real GNU ls rejects that combination; ours lets the last
     * flag win, which is a deliberate simplification.
     *
     * Depth 0 is the directory the user named; each nested call adds one.
     */
    if (optind >= argc) {
        do_ls(".", 0);
    } else {
        for (int i = optind; i < argc; i++) {
            do_ls(argv[i], 0);
            puts("");
        }
    }
    return 0;
}