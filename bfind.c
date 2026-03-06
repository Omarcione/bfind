/*
 * bfind - Breadth-first find
 *
 * A BFS version of the UNIX find utility using POSIX system calls.
 *
 * Usage: ./bfind [-L] [-xdev] [path...] [filters...]
 *
 * Filters:
 *   -name PATTERN   Glob match on filename (fnmatch)
 *   -type TYPE      f (file), d (directory), l (symlink)
 *   -mtime N        Modified within the last N days
 *   -size SPEC      File size: [+|-]N[c|k|M]
 *   -perm MODE      Exact octal permission match
 *
 * Options:
 *   -L              Follow symbolic links (default: no)
 *   -xdev           Do not cross filesystem boundaries
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "queue.h"

/* ------------------------------------------------------------------ */
/*  Filter definitions                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    FILTER_NAME,
    FILTER_TYPE,
    FILTER_MTIME,
    FILTER_SIZE,
    FILTER_PERM
} filter_kind_t;

typedef enum {
    SIZE_CMP_EXACT,
    SIZE_CMP_GREATER,
    SIZE_CMP_LESS
} size_cmp_t;

typedef struct {
    filter_kind_t kind;
    union {
        char *pattern;       /* -name */
        char type_char;      /* -type: 'f', 'd', or 'l' */
        int mtime_days;      /* -mtime */
        struct {
            off_t size_bytes;
            size_cmp_t size_cmp;
        } size;              /* -size */
        mode_t perm_mode;    /* -perm */
    } filter;
} filter_t;

/* ------------------------------------------------------------------ */
/*  Cycle detection                                                    */
/*                                                                     */
/*  A file's true on-disk identity is its (st_dev, st_ino) pair.       */
/*  You will need this for cycle detection when -L is set.             */
/* ------------------------------------------------------------------ */

typedef struct {
    dev_t dev;
    ino_t ino;
} dev_ino_t;

/* ------------------------------------------------------------------ */
/*  Global configuration                                               */
/* ------------------------------------------------------------------ */

static filter_t *g_filters = NULL;
static int g_nfilters = 0;
static bool g_follow_links = false;
static bool g_xdev = false;
static dev_t g_start_dev = 0;
static time_t g_now;
static dev_ino_t *seen_inos = NULL; //init if -L set
static int seen_inos_cnt = 0;
static int seen_inos_cap = 8;
/* ------------------------------------------------------------------ */
/*  Filter matching                                                    */
/* ------------------------------------------------------------------ */

/*
 * TODO 1: Implement this function.
 *
 * Return true if the single filter 'f' matches the file at 'path' with
 * metadata 'sb'. Handle each filter_kind_t in a switch statement.
 *
 * Refer to the assignment document for the specification of each filter.
 * Relevant man pages: fnmatch(3), stat(2).
 */
static bool filter_matches(const filter_t *f, const char *path,
                           const struct stat *sb) {
    switch (f->kind)
    {
    case FILTER_NAME: {
        const char *filename = strrchr(path, '/'); // finds last '/'
        filename = filename ? filename + 1 : path;  // skip the '/', or use path if no '/' found
        return fnmatch(f->filter.pattern, filename, 0) == 0;
    }
    case FILTER_TYPE: {
        mode_t m = sb->st_mode;

        switch (f->filter.type_char)
        {
        case 'f': return S_ISREG(m);
        case 'd': return S_ISDIR(m);
        case 'l': return S_ISLNK(m);
        }
    }
    case FILTER_MTIME:
        return (difftime(g_now, sb->st_mtime) / 86400) <= f->filter.mtime_days; // modified in last N days
    case FILTER_SIZE: {
        off_t target = f->filter.size.size_bytes;
        off_t file_size = sb->st_size;
        
        switch (f->filter.size.size_cmp)
        {
        case SIZE_CMP_EXACT: return file_size == target;
        case SIZE_CMP_GREATER: return file_size > target;
        case SIZE_CMP_LESS: return file_size < target;
        }
    }
    case FILTER_PERM: {
        mode_t perms = sb->st_mode & 07777;
        return f->filter.perm_mode == perms;
    }
    }
}  

/* Check if ALL filters match (AND semantics).
 * Returns true if every filter matches, false otherwise. */
static bool matches_all_filters(const char *path, const struct stat *sb) {
    for (int i = 0; i < g_nfilters; i++) {
        if (!filter_matches(&g_filters[i], path, sb)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Usage / help                                                       */
/* ------------------------------------------------------------------ */

static void print_usage(const char *progname) {
    printf("Usage: %s [-L] [-xdev] [path...] [filters...]\n"
           "\n"
           "Breadth-first search for files in a directory hierarchy.\n"
           "\n"
           "Options:\n"
           "  -L              Follow symbolic links\n"
           "  -xdev           Do not cross filesystem boundaries\n"
           "  --help          Display this help message and exit\n"
           "\n"
           "Filters (all filters are ANDed together):\n"
           "  -name PATTERN   Match filename against a glob pattern\n"
           "  -type TYPE      Match file type: f (file), d (dir), l (symlink)\n"
           "  -mtime N        Match files modified within the last N days\n"
           "  -size [+|-]N[c|k|M]\n"
           "                  Match file size (c=bytes, k=KiB, M=MiB)\n"
           "                  Prefix + means greater than, - means less than\n"
           "  -perm MODE      Match exact octal permission bits\n"
           "\n"
           "If no path is given, defaults to the current directory.\n",
           progname);
}

/* ------------------------------------------------------------------ */
/*  Argument parsing                                                   */
/* ------------------------------------------------------------------ */

/*
 * TODO 2: Implement this function.
 *
 * Parse a size specifier string into a byte count. The input is the
 * numeric portion (after any leading +/- is stripped by the caller)
 * with an optional unit suffix: 'c' (bytes), 'k' (KiB), 'M' (MiB).
 * No suffix means bytes.
 *
 * Examples: "100c" -> 100, "4k" -> 4096, "2M" -> 2097152, "512" -> 512
 */
static off_t parse_size(const char *arg) {
    char *endptr;

    long value = strtol(arg, &endptr, 10);
    if (value == 0){
        fprintf(stderr, "Size not valid: %s\n", arg);
        exit(EXIT_FAILURE);
    }

    // Check for suffix
    switch (*endptr) {
        case '\0': // No suffix
        case 'c': return (off_t)value;
        case 'k': return (off_t)value * 1024;
        case 'M': return (off_t)value * 1024 * 1024;
        default: return -1; // Invalid suffix
    }
}

/*
 * TODO 3: Implement this function.
 *
 * Parse command-line arguments into options, paths, and filters.
 * See the usage string and assignment document for the expected format.
 *
 * Set the global variables g_follow_links, g_xdev, g_filters, and
 * g_nfilters as appropriate. Return a malloc'd array of path strings
 * and set *npaths. If no paths are given, default to ".".
 *
 * Handle --help by calling print_usage() and exiting.
 * Exit with an error for unknown options or missing filter arguments.
 */
static char **parse_args(int argc, char *argv[], int *npaths) {
    char **paths = NULL;
    bool parsing_filters = false;  // once we see a filter flag, no more paths

    for (int i = 1; i < argc; i++) {
        // handle --help command
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);

            exit(EXIT_SUCCESS);
        }

        // handle options before paths 
        else if (!parsing_filters && argv[i][0] == '-') {
            if (strcmp(argv[i], "-L") == 0) {
                g_follow_links = true;
            } 
            else if (strcmp(argv[i], "-xdev") == 0) {
                g_xdev = true;
            } 
            else {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                exit(EXIT_FAILURE);
            }
        }
        
        // handle filters
        else if (strcmp(argv[i], "-name") == 0) {
            parsing_filters = true;
            if (i + 1 >= argc) { 
                fprintf(stderr, "-name requires an argument\n"); 
                exit(EXIT_FAILURE); 
            }
            g_filters = realloc(g_filters, sizeof(filter_t) * (g_nfilters + 1));
            g_filters[g_nfilters].kind = FILTER_NAME;
            g_filters[g_nfilters].filter.pattern = strdup(argv[++i]);
            g_nfilters++;
        }
        else if (strcmp(argv[i], "-type") == 0) {
            parsing_filters = true;
            if (i + 1 >= argc) { 
                fprintf(stderr, "-type requires an argument\n"); 
                exit(EXIT_FAILURE); 
            }
            i++;
            if (argv[i][0] != 'f' && argv[i][0] != 'd' && argv[i][0] != 'l') {
                fprintf(stderr, "-type must be f, d, or l\n"); 
                exit(EXIT_FAILURE);
            }
            g_filters = realloc(g_filters, sizeof(filter_t) * (g_nfilters + 1));
            g_filters[g_nfilters].kind = FILTER_TYPE;
            g_filters[g_nfilters].filter.type_char = argv[i][0];
            g_nfilters++;
        }
        else if (strcmp(argv[i], "-mtime") == 0) {
            parsing_filters = true;
            if (i + 1 >= argc) { 
                fprintf(stderr, "-mtime requires an argument\n"); 
                exit(EXIT_FAILURE); 
            }
            g_filters = realloc(g_filters, sizeof(filter_t) * (g_nfilters + 1));
            g_filters[g_nfilters].kind = FILTER_MTIME;
            g_filters[g_nfilters].filter.mtime_days = atoi(argv[++i]);
            g_nfilters++;
        }
        else if (strcmp(argv[i], "-size") == 0) {
            parsing_filters = true;
            if (i + 1 >= argc) { 
                fprintf(stderr, "-size requires an argument\n"); 
                exit(EXIT_FAILURE); 
            }
            i++;
            const char *specifier = argv[i];
            size_cmp_t cmp = SIZE_CMP_EXACT;
            if (specifier[0] == '+') { 
                cmp = SIZE_CMP_GREATER; 
                specifier++; 
            }
            else if (specifier[0] == '-') { 
                cmp = SIZE_CMP_LESS;    
                specifier++; 
            }
            g_filters = realloc(g_filters, sizeof(filter_t) * (g_nfilters + 1));
            g_filters[g_nfilters].kind = FILTER_SIZE;
            g_filters[g_nfilters].filter.size.size_bytes = parse_size(specifier);
            g_filters[g_nfilters].filter.size.size_cmp = cmp;
            g_nfilters++;
        }
        else if (strcmp(argv[i], "-perm") == 0) {
            parsing_filters = true;
            if (i + 1 >= argc) { 
                fprintf(stderr, "-perm requires an argument\n"); 
                exit(EXIT_FAILURE); 
            }
            g_filters = realloc(g_filters, sizeof(filter_t) * (g_nfilters + 1));
            g_filters[g_nfilters].kind = FILTER_PERM;
            g_filters[g_nfilters].filter.perm_mode = (mode_t)strtol(argv[++i], NULL, 8); // octal base
            g_nfilters++;
        }

        // handle path args (anything that doesn't start with '-')
        else if (!parsing_filters && argv[i][0] != '-') {
            paths = realloc(paths, sizeof(char *) * (*npaths + 1));
            if (paths == NULL) {
                perror("realloc");
                exit(EXIT_FAILURE);
            }
            paths[*npaths] = strdup(argv[i]);
            if (paths[*npaths] == NULL) {
                perror("strdup");
                exit(EXIT_FAILURE);
            }
            (*npaths)++;
        }

        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            exit(EXIT_FAILURE);
        }
    }

    // handle if no path is given
    if (*npaths == 0) {
        paths = realloc(paths, sizeof(char *) * 1);
        if (paths == NULL) {
            perror("realloc");
            exit(EXIT_FAILURE);
        }
        paths[0] = strdup(".");
        if (paths[0] == NULL) {
            perror("strdup");
            exit(EXIT_FAILURE);
        }
        (*npaths)++;
    }

    return paths;
}

// Returns true if the current path is a starting path, used to decide if we need to free the duped path string
bool is_start_path(char **start_paths, int npaths, char *cur_path) {
    for (int i = 0; i < npaths; i++) {
        if (strcmp(start_paths[i], cur_path) == 0)
        return true;
    }
    return false;
}

// marks an inode as seen, reallocates seen array to current cap*2 if needed
void mark_seen(dev_t dev, ino_t ino) {
    if (seen_inos_cnt == seen_inos_cap) {
        seen_inos_cap*=2;
        seen_inos = realloc(seen_inos, sizeof(dev_ino_t) * seen_inos_cap);
    }
    seen_inos[seen_inos_cnt].dev = dev;
    seen_inos[seen_inos_cnt].ino = ino;
    seen_inos_cnt++;
}

bool check_seen(dev_t dev, ino_t ino) {
    for (int i = 0; i < seen_inos_cnt; i++) {
        if (seen_inos[i].dev == dev && seen_inos[i].ino == ino)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  BFS traversal                                                      */
/* ------------------------------------------------------------------ */

/*
 * TODO 4: Implement this function.
 *
 * Traverse the filesystem breadth-first starting from the given paths.
 * For each entry, check the filters and print matching paths to stdout.
 *
 * You must handle:
 *   - The -L flag: controls whether symlinks are followed. Think about
 *     when to use stat(2) vs lstat(2) and what that means for descending
 *     into directories.
 *   - The -xdev flag: do not descend into directories on a different
 *     filesystem than the starting path (compare st_dev values).
 *   - Cycle detection (only relevant with -L): a symlink can point back
 *     to an ancestor directory. Only symlinks can create cycles (the OS
 *     forbids hard links to directories). Use the dev_ino_t type defined
 *     above to track visited directories — real directories should always
 *     be descended into, but symlinks to already-visited directories
 *     should be skipped.
 *   - Errors: if stat or opendir fails, print a message to stderr
 *     and continue traversing. Do not exit.
 *
 * The provided queue library (queue.h) implements a generic FIFO queue.
 */
static void bfs_traverse(char **start_paths, int npaths) {
    queue_t q;
    queue_init(&q);

    for (int i = 0; i < npaths; i++) {
        queue_enqueue(&q, strdup(start_paths[i])); // dup so we can free path unconditionally
        if (g_xdev && !g_start_dev) {
            //which std_dev do we check?
            struct stat start_sb;
            int stat_res = g_follow_links ? stat(start_paths[i], &start_sb) : lstat(start_paths[i], &start_sb);
            if (stat_res < 0) {
                fprintf(stderr, "Stat failed for path %s\n", start_paths[i]);
                continue;
            }    
            g_start_dev = start_sb.st_dev;
        }
    }

    if (g_xdev && !g_start_dev) {
        fprintf(stderr, "cannot get starting path filesystem");
        exit(1);
    }

    // allocate visted inodes arr
    if(!(seen_inos = malloc(sizeof(dev_ino_t) * seen_inos_cap))) {
        perror("malloc");
        exit(1);
    }
    
    while (!queue_is_empty(&q)) {
        char *cur_path = queue_dequeue(&q);
        struct stat sb;
        int stat_res = g_follow_links ? stat(cur_path, &sb) : lstat(cur_path, &sb);
        if (stat_res < 0) {
            fprintf(stderr, "Stat failed for path %s\n", cur_path);
            free(cur_path);
            continue;
        }
        if (matches_all_filters(cur_path, &sb)) {
            printf("%s\n", cur_path);
        }

        // mark seen, add to array
        if (g_follow_links)
            mark_seen(sb.st_dev, sb.st_ino);

        mode_t m = sb.st_mode;

        if (g_xdev && sb.st_dev != g_start_dev) //skip if different file system
            continue;

        if (S_ISDIR(m) || (g_follow_links && S_ISLNK(m))) {
            DIR *dir = opendir(cur_path);
            if (!dir) {
                fprintf(stderr, "cannot open %s: %s\n", cur_path, strerror(errno));
                free(cur_path);
                continue;
            }
            struct dirent *entry;
            while ((entry = readdir(dir))) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) // skip . and .. 
                    continue;
                char child_path[PATH_MAX];
                snprintf(child_path, sizeof(child_path), "%s/%s", cur_path, entry->d_name);
                struct stat child_sb;
                if (g_follow_links) { 
                    int child_stat_res = stat(child_path, &child_sb);
                    if (S_ISLNK(child_sb.st_mode)) { 
                        if (check_seen(child_sb.st_dev, child_sb.st_ino)) {
                            continue;
                        }
                    }
                }
                
                queue_enqueue(&q, strdup(child_path)); // make a copy of str
            }
            closedir(dir);
        }
        free(cur_path);
    }
    free(seen_inos);

}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    g_now = time(NULL);

    int npaths = 0;
    char **paths = parse_args(argc, argv, &npaths);

    bfs_traverse(paths, npaths);

    free(paths);
    free(g_filters);
    return 0;
}
