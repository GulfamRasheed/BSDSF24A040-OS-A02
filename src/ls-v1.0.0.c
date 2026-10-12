/*
 * lsv1.1.0.c  --  Feature-2: Complete Long Listing Format (-l)
 *
 * BASE: the instructor's lsv1.0.0 (Programming-Assignments/PA-02).
 * We KEEP its structure deliberately - same function name `do_ls()`, same
 * argument loop in main(), same dotfile filtering - and only add the -l
 * branch inside do_ls() plus the helper functions the long format needs.
 *
 * NEW IN THIS VERSION
 *   -l long listing that mirrors `ls -l`.
 *
 * NEW SYSTEM CALLS / FUNCTIONS USED
 *   getopt()    - POSIX option parser
 *   lstat()     - metadata WITHOUT following symlinks
 *   getpwuid()  - uid  -> username     (from /etc/passwd)
 *   getgrgid()  - gid  -> groupname    (from /etc/group)
 *   ctime()     - time_t -> "Www Mmm dd hh:mm:ss yyyy"
 *   readlink()  - target of a symbolic link
 *
 * WHY lstat() AND NOT stat()?
 *   stat()   FOLLOWS symlinks: for a link to /etc/passwd it returns the
 *            metadata of /etc/passwd itself.
 *   lstat()  does NOT follow: it returns the metadata of the LINK itself
 *            (st_mode has S_IFLNK set). `ls -l` must print 'l' in the first
 *            permission column for a symlink, so lstat() is the correct call.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

/*
 * Option state. `static` keeps it private to this file. It must be global
 * (not a local of main) because do_ls() is a separate function and C has
 * no closures, so there is no other way for the two to share it.
 */
static int opt_long = 0;

/*
 * Convert st_mode into the familiar 10-character "rwxr-xr-x" string.
 *
 * WHY THIS WORKS
 *   st_mode packs THREE kinds of information into one integer:
 *     (1) file type       -> high bits, e.g. S_IFDIR, S_IFREG, S_IFLNK
 *     (2) permission bits -> low bits,  e.g. S_IRUSR, S_IWGRP, S_IXOTH
 *     (3) special bits    -> S_ISUID, S_ISGID, S_ISVTX
 *
 *   Each field occupies disjoint bit positions, so any one of them can be
 *   extracted with a bitwise AND. `mode & S_IRUSR` yields the bit itself
 *   when it is set and 0 when it is clear, so it works directly as a
 *   boolean. That is the whole trick: an AND with a one-bit mask is a bit
 *   test in disguise.
 */
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

/* ---- unchanged from the base version ----------------------------------- */

void do_ls(const char *dir)
{
    struct dirent *entry;
    DIR *dp = opendir(dir);
    if (dp == NULL)
    {
        fprintf(stderr, "Cannot open directory : %s\n", dir);
        return;
    }
    errno = 0;
    while ((entry = readdir(dp)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;

        if (opt_long) {
            /*
             * lstat() resolves relative names against the process CWD, not
             * against `dir`, so we must hand it "<dir>/<name>". snprintf
             * bounds the write; an unbounded sprintf here is a textbook
             * buffer-overflow bug.
             */
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
            print_long_entry(full, entry->d_name);
        } else {
            printf("%s\n", entry->d_name);
        }
    }

    if (errno != 0)
    {
        perror("readdir failed");
    }

    closedir(dp);
}

int main(int argc, char const *argv[])
{
    int opt;

    /*
     * getopt() hands back one recognised option letter at a time. "l" in the
     * optstring means 'l' is a valid option and takes NO argument (a ':'
     * after it would mean it does). '?' means an unknown option, which we
     * reject. On return, optind is the index of the first NON-option
     * argument - the first directory the user wants listed.
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

    while ((opt = getopt(argc, (char *const *)argv, "l")) != -1) {
        switch (opt) {
            case 'l':
                opt_long = 1;
                break;
            case '?':
            default:
                fprintf(stderr, "Usage: %s [-l] [DIRECTORY]...\n", argv[0]);
                return EXIT_FAILURE;
        }
    }

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