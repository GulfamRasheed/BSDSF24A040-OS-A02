/*
 * ls-v1.3.0.c  --  Feature-4: Horizontal Column Display (-x)
 *
 * NEW IN THIS VERSION
 *   A second columnar mode. -x prints left-to-right and wraps to the next
 *   line only when the terminal is full ("across then down").
 *
 * NEW CONCEPTS
 *   Display-mode state - one enum replaces the boolean opt_long, so three
 *                         mutually exclusive modes can be tracked.
 *   Index-derived column - the column of an item is computed from its index
 *                         instead of from a running cursor.
 *
 * INHERITED FROM v1.2.0
 *   Dynamic array, ioctl(TIOCGWINSZ), down-then-across printing, -l.
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
 * We replace the `int opt_long` boolean with an enum because there are now
 * THREE mutually exclusive states. A boolean cannot express "neither of the
 * two things I have flags for", so with two booleans we would also need
 * extra code to decide which one wins when both are set. An enum makes the
 * state space explicit and impossible to contradict.
 */
typedef enum {
    MODE_COLUMN,    /* default : down then across */
    MODE_LONG,      /* -l      : one file per line with metadata */
    MODE_ACROSS     /* -x      : left to right, then wrap */
} DisplayMode;

static DisplayMode display_mode = MODE_COLUMN;

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

/* Release every string and the array itself. */
static void namelist_free(NameList *nl)
{
    size_t i;
    for (i = 0; i < nl->count; i++) {
        free(nl->names[i]);   /* the string */
    }
    free(nl->names);          /* the array  */
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
 *   width     = terminal columns
 *   col_width = longest filename + NAME_SPACING   (every column is this
 *               wide so that they line up)
 *   num_cols  = width / col_width                (how many fit)
 *   num_rows  = (count + num_cols - 1) / num_cols (ceiling division)
 *
 *   A single loop `for i in 0..count` prints FILES left-to-right, which is
 *   "across then down" and looks wrong: the last column would be ragged
 *   and the row contents would shift every time the terminal is resized.
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
static void print_columns(const NameList *nl)
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
        for (i = 0; i < nl->count; i++)
            printf("%s\n", nl->names[i]);
        return;
    }

    num_cols = (size_t)width / col_width;
    /* Ceiling division: (a + b - 1) / b rounds a/b up. */
    num_rows = (nl->count + num_cols - 1) / num_cols;

    for (r = 0; r < num_rows; r++) {
        for (c = 0; c < num_cols; c++) {
            size_t idx = c * num_rows + r;

            if (idx >= nl->count)
                continue;              /* ragged last column */

            /* TRAILING-WHITESPACE FIX.
               Pad only when a REAL item further along this row exists:
                 - a next column exists on this row     (c + 1 < num_cols)
                 - that next column actually has a name (idx+num_rows < count)
               Padding the final item of a row would leave spaces at the
               end of the line, which real ls never emits. */
            if (c + 1 < num_cols && idx + num_rows < nl->count)
                printf("%-*s", (int)col_width, nl->names[idx]);
            else
                printf("%s", nl->names[idx]);
        }
        putchar('\n');
    }
}

/*
 * Print the name list left-to-right, wrapping at the terminal edge.
 *
 * WHY THIS IS SIMPLER THAN print_columns()
 *   print_columns() must know num_rows BEFORE it prints anything, because
 *   row r / column c maps to index c * num_rows + r. num_rows depends on
 *   num_cols, which depends on the longest name AND the terminal width, so
 *   a full two-dimensional layout has to be computed up front.
 *
 *   Here the mapping is trivial: index i goes to (row i / num_cols,
 *   column i % num_cols). We never need the total number of rows.
 */
static void print_across(const NameList *nl)
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
        for (i = 0; i < nl->count; i++)
            printf("%s\n", nl->names[i]);
        return;
    }

    num_cols = (size_t)width / col_width;

    for (i = 0; i < nl->count; i++) {
        size_t col = i % num_cols;   /* which column of the line */
        int pad;

        /* WRAP. `i` starts a new line whenever it is in column 0 and is
           not the first item overall. */
        if (col == 0 && i > 0)
            putchar('\n');

        /*
         * PADDING - and why we deliberately do NOT track a running cursor.
         *
         * The intuitive version keeps a variable `used` holding the current
         * column and pads while `used + col_width <= width`. That looks
         * right but is WRONG. A padded item occupies the FULL col_width on
         * screen, whereas an unpadded item occupies only its own length. If
         * `used` counts the padded case correctly but the unpadded case
         * only by len(), then after a line-final bare item `used` is no
         * longer a multiple of col_width and every later wrap test is off
         * by the missing padding - producing "alpha.txtsub2" with no gap.
         *
         * Deriving the column from the index - col = i % num_cols - removes
         * the whole class of bug, because the item-to-position mapping is a
         * pure function of the index and can never drift. Padding then
         * reduces to "is this the last column that still has content?".
         */
        pad = (col + 1 < num_cols) && (i + 1 < nl->count);

        if (pad)
            printf("%-*s", (int)col_width, nl->names[i]);
        else
            printf("%s", nl->names[i]);
    }

    putchar('\n');
}

/* --- long listing (unchanged from v1.1.0) ------------------------------ */

static void mode_to_perms(mode_t mode, char *out)
{
    const char *rwx = "rwxrwxrwx";

    /* The nine permission bits, in printing order: owner rwx, group rwx,
       other rwx. Bit 8 is "owner read" and maps to rwx[0]; bit 0 is "other
       execute" and maps to rwx[8]. Hence the (8 - i) shift. */
    for (int i = 0; i < 9; i++)
        out[i] = (mode & (1 << (8 - i))) ? rwx[i] : '-';
    out[9] = '\0';

    /* The FIRST character is the file type, not 'r'. */
    if (S_ISDIR(mode))       out[0] = 'd';
    else if (S_ISLNK(mode))  out[0] = 'l';
    else if (S_ISCHR(mode))  out[0] = 'c';
    else if (S_ISBLK(mode))  out[0] = 'b';
    else if (S_ISFIFO(mode)) out[0] = 'p';
    else if (S_ISSOCK(mode)) out[0] = 's';
    /* else it stays 'r' -> a regular file */

    /*
     * SPECIAL BITS overwrite the execute character they share a position
     * with. GNU ls uses lower case when the execute bit is ALSO set and
     * upper case when it is not:
     *     setuid + executable -> 's'   setuid + not executable -> 'S'
     */
    if (mode & S_ISUID) out[2] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID) out[5] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX) out[8] = (mode & S_IXOTH) ? 't' : 'T';
}

/*
 * Format st_mtime the way `ls -l` does:
 *     "Mar  5 14:30"   recent files  (time of day)
 *     "Mar  5  2024"   older files   (year)
 *
 * ctime() returns a fixed-layout 24-character string:
 *     "Tue Mar  5 14:30:07 2024"
 *      012 4   8 11    14      20
 * We slice that layout instead of using strftime(), because strftime's
 * output also depends on the locale and would then differ from real ls.
 */
static void format_mtime(time_t mtime, char *buf)
{
    char *raw = ctime(&mtime);
    char month[4];

    if (raw == NULL) {
        snprintf(buf, 16, "            ");
        return;
    }

    memcpy(month, raw + 4, 3);
    month[3] = '\0';

    if (mtime > time(NULL) - 15552000L) {     /* ~6 months */
        snprintf(buf, 16, "%s %2d %02d:%02d",
                 month, atoi(raw + 8), atoi(raw + 11), atoi(raw + 14));
    } else {
        snprintf(buf, 16, "%s %2d  %s", month, atoi(raw + 8), raw + 20);
    }
}

/*
 * Print ONE file in long format.
 *
 * getpwuid() / getgrgid() return a pointer to a STATIC buffer that the
 * NEXT call overwrites. We must therefore never free() it, and we must
 * copy the string out if it has to outlive the call. Here we only print it
 * straight away, so that is safe.
 */
static void print_long_entry(const char *path, const char *name)
{
    struct stat info;
    char perms[11];
    char timebuf[16];
    struct passwd *pwd;
    struct group  *grp;

    if (lstat(path, &info) != 0) {
        /* A file can disappear between readdir() and lstat(); that is not
           fatal for the whole listing, so report it and carry on. */
        fprintf(stderr, "ls: cannot access '%s': %s\n", path, strerror(errno));
        return;
    }

    mode_to_perms(info.st_mode, perms);
    format_mtime(info.st_mtime, timebuf);

    errno = 0;
    pwd = getpwuid(info.st_uid);
    grp = getgrgid(info.st_gid);

    /*
     * Column widths match GNU ls:
     *   perms 10 | nlink 3 right | user 8 left | group 8 left
     *   | size 9 right | time 12 | name
     * The * and - flags of printf are how C does column alignment.
     */
    printf("%s %3lu %-8s %-8s %9ld %s %s",
           perms,
           (unsigned long)info.st_nlink,
           pwd ? pwd->pw_name : "UNKNOWN",
           grp ? grp->gr_name : "UNKNOWN",
           (long)info.st_size,
           timebuf,
           name);

    /* Symlinks get "-> target" at the end, exactly like real ls. */
    if (S_ISLNK(info.st_mode)) {
        char target[1024];
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';       /* readlink does NOT null-terminate */
            printf(" -> %s", target);
        }
    }
    printf("\n");
}

/* ------------------------------------------------------------------------ */

static void do_ls(const char *path)
{
    DIR *dir;
    struct dirent *entry;
    NameList nl;

    dir = opendir(path);
    if (dir == NULL) {
        perror(path);
        return;
    }

    namelist_init(&nl);

    /*
     * PHASE 1 - READ EVERYTHING.
     * We cannot start printing until we know the LONGEST filename, because
     * the column width depends on it. So the whole directory is slurped
     * into memory first, and only then printed.
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

    /* PHASE 2 - PRINT.  Dispatch on the display mode. */
    if (display_mode == MODE_LONG) {
        size_t i;
        for (i = 0; i < nl.count; i++) {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", path, nl.names[i]);
            print_long_entry(full, nl.names[i]);
        }
    } else if (display_mode == MODE_ACROSS) {
        print_across(&nl);
    } else {
        print_columns(&nl);
    }

    namelist_free(&nl);
}

int main(int argc, char const *argv[])
{
    int opt;

    /*
     * The optstring "lx" tells getopt() that 'l' and 'x' are valid options
     * and that NEITHER takes an argument (a ':' after a letter would say it
     * does).
     */
    /*
     * The cast is required, not cosmetic: getopt() is declared as
     *     int getopt(int, char *const *, const char *)
     * but the base version main() is
     *     int main(int argc, char const *argv[])
     * which makes argv a `const char **`. Those are incompatible pointer
     * types. The cast is safe because getopt() may PERMUTE argv and
     * substitute option values, but it never modifies the strings the
     * pointers refer to.
     */

    while ((opt = getopt(argc, (char *const *)argv, "lx")) != -1) {
        switch (opt) {
            case 'l':
                display_mode = MODE_LONG;
                break;
            case 'x':
                display_mode = MODE_ACROSS;
                break;
            case '?':
            default:
                fprintf(stderr, "Usage: %s [-lx] [DIRECTORY]...\n", argv[0]);
                return EXIT_FAILURE;
        }
    }

    /* Flag order decides the winner, so `ls -l -x` ends up horizontal. Real
       GNU ls rejects that combination; ours lets the last flag win. */
    if (optind >= argc) {
        do_ls(".");
    } else {
        for (int i = optind; i < argc; i++) {
            printf("Directory listing of %s : \n", argv[i]);
            do_ls(argv[i]);
            puts("");
        }
    }
    return 0;
}