#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdint.h>

#define PATH_SZ  512
#define CMD_SZ   256
#define LINE_SZ  256

static char proc_root[PATH_SZ] = "/proc";
static pid_t our_pid;
static double page_kb;
static int have_pss = 0;

/* ---- memory entry ---- */

typedef struct {
    char          *cmd;
    double         priv_total;
    double         shared_total;
    int            count;
    unsigned long *mem_ids;
    size_t         n_ids;
    size_t         cap_ids;
} Entry;

static Entry  *entries     = NULL;
static size_t  n_entries   = 0;
static size_t  cap_entries = 0;

static void *xrealloc(void *p, size_t sz) {
    void *r = realloc(p, sz);
    if (!r) { perror("realloc"); exit(1); }
    return r;
}

static void free_entries(void) {
    for (size_t i = 0; i < n_entries; i++) {
        free(entries[i].cmd);
        free(entries[i].mem_ids);
    }
    free(entries);
    entries = NULL; n_entries = cap_entries = 0;
    have_pss = 0;
}

static Entry *find_or_create(const char *cmd) {
    for (size_t i = 0; i < n_entries; i++)
        if (strcmp(entries[i].cmd, cmd) == 0) return &entries[i];
    if (n_entries == cap_entries) {
        cap_entries = cap_entries ? cap_entries * 2 : 16;
        entries = xrealloc(entries, cap_entries * sizeof(Entry));
    }
    Entry *e = &entries[n_entries++];
    memset(e, 0, sizeof(*e));
    e->cmd = strdup(cmd);
    if (!e->cmd) { perror("strdup"); exit(1); }
    return e;
}

static void entry_add_id(Entry *e, unsigned long id) {
    for (size_t i = 0; i < e->n_ids; i++)
        if (e->mem_ids[i] == id) return;
    if (e->n_ids == e->cap_ids) {
        e->cap_ids = e->cap_ids ? e->cap_ids * 2 : 4;
        e->mem_ids = xrealloc(e->mem_ids, e->cap_ids * sizeof(unsigned long));
    }
    e->mem_ids[e->n_ids++] = id;
}

/* ---- path builder ---- */

static void build_path(char *buf, size_t sz, const char *fmt, ...) {
    char rest[PATH_SZ];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rest, sizeof(rest), fmt, ap);
    va_end(ap);
    snprintf(buf, sz, "%s/%s", proc_root, rest);
}

/* ---- kernel version ---- */

static void kernel_ver(int *maj, int *min, int *rel) {
    *maj = *min = *rel = 0;
    char path[PATH_SZ];
    build_path(path, sizeof(path), "sys/kernel/osrelease");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char ver[64];
    if (fgets(ver, sizeof(ver), f)) {
        char *p = ver, *e;
        *maj = (int)strtol(p, &e, 10); if (*e == '.') p = e + 1;
        *min = (int)strtol(p, &e, 10); if (*e == '.') p = e + 1;
        *rel = (int)strtol(p, &e, 10);
    }
    fclose(f);
}

/* ---- human readable ---- */

static void human(double kb, char *buf, size_t sz) {
    static const char *powers[] = {"Ki", "Mi", "Gi", "Ti"};
    int u = 0;
    double v = kb;
    while (v >= 1000.0 && u < 3) { v /= 1024.0; u++; }
    snprintf(buf, sz, "%.1f %sB", v, powers[u]);
}

/* ---- value after colon (smaps "Key: N kB" lines) ---- */

static double value_after_colon(const char *line) {
    const char *p = strchr(line, ':');
    return p ? strtod(p + 1, NULL) : 0.0;
}

/* ---- get command name ---- */

static int get_cmd_name(pid_t pid, int split_args, char *buf, size_t sz) {
    char path[PATH_SZ];
    char exe[PATH_SZ];
    ssize_t n;

    build_path(path, sizeof(path), "%d/exe", (int)pid);
    n = readlink(path, exe, sizeof(exe) - 1);
    if (n < 0) return -1;
    exe[n] = '\0';

    if (split_args) {
        build_path(path, sizeof(path), "%d/cmdline", (int)pid);
        FILE *f = fopen(path, "r");
        if (!f) return -1;
        size_t rd = fread(buf, 1, sz - 1, f);
        fclose(f);
        buf[rd] = '\0';
        for (size_t i = 0; i < rd; i++) if (buf[i] == '\0') buf[i] = ' ';
        while (rd > 0 && buf[rd - 1] == ' ') buf[--rd] = '\0';
        return 0;
    }

    int deleted = 0;
    if (n > 10 && strcmp(exe + n - 10, " (deleted)") == 0) {
        exe[n - 10] = '\0';
        deleted = 1;
    }

    if (deleted) {
        if (access(exe, F_OK) == 0) {
            snprintf(buf, sz, "%s [updated]", exe);
        } else {
            build_path(path, sizeof(path), "%d/cmdline", (int)pid);
            FILE *f = fopen(path, "r");
            char cl[PATH_SZ] = {0};
            if (f) { fread(cl, 1, sizeof(cl) - 1, f); fclose(f); }
            if (cl[0] && access(cl, F_OK) == 0)
                snprintf(buf, sz, "%s [updated]", cl);
            else
                snprintf(buf, sz, "%s [deleted]", exe);
        }
        return 0;
    }

    /* basename of exe */
    const char *base = strrchr(exe, '/');
    base = base ? base + 1 : exe;

    /* Name from /proc/<pid>/status first line */
    build_path(path, sizeof(path), "%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char name_line[CMD_SZ] = {0};
    if (fgets(name_line, sizeof(name_line), f)) {
        char *p = name_line;
        /* "Name:\t<name>\n" */
        if (strncmp(p, "Name:", 5) == 0) {
            p += 5;
            while (*p == '\t' || *p == ' ') p++;
            size_t len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) p[--len] = '\0';
            if (strncmp(base, p, len) == 0)
                snprintf(buf, sz, "%s", base);
            else
                snprintf(buf, sz, "%s", p);
        } else {
            snprintf(buf, sz, "%s", base);
        }
    } else {
        snprintf(buf, sz, "%s", base);
    }
    fclose(f);
    return 0;
}

/* ---- get memory stats from smaps / statm ---- */

static int get_mem_stats(pid_t pid, double *out_priv, double *out_shared,
                         unsigned long *out_mem_id) {
    char path[PATH_SZ];
    char smaps_path[PATH_SZ];
    build_path(smaps_path, sizeof(smaps_path), "%d/smaps_rollup", (int)pid);
    if (access(smaps_path, F_OK) != 0)
        build_path(smaps_path, sizeof(smaps_path), "%d/smaps", (int)pid);
    strncpy(path, smaps_path, sizeof(path) - 1);

    *out_mem_id = (unsigned long)pid;

    FILE *f = fopen(path, "r");
    if (!f) {
        /* fallback to statm */
        build_path(path, sizeof(path), "%d/statm", (int)pid);
        f = fopen(path, "r");
        if (!f) return -1;
        unsigned long long rss = 0, shr = 0;
        int rv = fscanf(f, "%*llu %llu %llu", &rss, &shr);
        fclose(f);
        if (rv < 2) return -1;
        *out_priv   = (double)(rss - shr) * page_kb;
        *out_shared = (double)shr * page_kb;
        return 0;
    }

    /* FNV-1a hash of smaps content for CLONE_VM detection */
    unsigned long h = 2166136261UL;
    double priv = 0, shr = 0, pss = 0;
    int local_pss = 0;
    char line[LINE_SZ];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        for (size_t i = 0; i < len; i++)
            h = (h ^ (unsigned char)line[i]) * 16777619UL;
        if (strncmp(line, "Private_", 8) == 0)
            priv += value_after_colon(line);
        else if (strncmp(line, "Shared_", 7) == 0)
            shr += value_after_colon(line);
        else if (strncmp(line, "Pss:", 4) == 0) {
            pss += value_after_colon(line) + 0.5;
            local_pss = 1;
        }
    }
    fclose(f);

    *out_mem_id = h;

    if (local_pss) {
        have_pss = 1;
        *out_priv   = priv;
        *out_shared = pss - priv;
    } else {
        *out_priv   = priv;
        *out_shared = shr;
    }
    return 0;
}

/* ---- collect memory usage ---- */

static int entry_compare(const void *a, const void *b) {
    const Entry *ea = (const Entry *)a;
    const Entry *eb = (const Entry *)b;
    double ta = ea->priv_total + ea->shared_total;
    double tb = eb->priv_total + eb->shared_total;
    if (ta < tb) return -1;
    if (ta > tb) return  1;
    return 0;
}

static void get_memory_usage(pid_t *filter, int nfilter, int split_args) {
    DIR *d = opendir(proc_root);
    if (!d) { perror(proc_root); exit(1); }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char *end;
        long lpid = strtol(de->d_name, &end, 10);
        if (*end != '\0') continue;
        pid_t pid = (pid_t)lpid;

        if (pid == our_pid) continue;
        if (nfilter > 0) {
            int found = 0;
            for (int i = 0; i < nfilter; i++)
                if (filter[i] == pid) { found = 1; break; }
            if (!found) continue;
        }

        char cmd[CMD_SZ];
        if (get_cmd_name(pid, split_args, cmd, sizeof(cmd)) < 0) continue;

        double priv, shared;
        unsigned long mem_id;
        if (get_mem_stats(pid, &priv, &shared, &mem_id) < 0) continue;

        Entry *e = find_or_create(cmd);
        if (have_pss)
            e->shared_total += shared;
        else if (shared > e->shared_total)
            e->shared_total = shared;
        e->priv_total += priv;
        e->count++;
        entry_add_id(e, mem_id);
    }
    closedir(d);

    /* CLONE_VM correction: single smaps fingerprint across multiple pids */
    for (size_t i = 0; i < n_entries; i++) {
        Entry *e = &entries[i];
        if (e->n_ids == 1 && e->count > 1) {
            e->priv_total /= e->count;
            if (have_pss) e->shared_total /= e->count;
        }
    }

    qsort(entries, n_entries, sizeof(Entry), entry_compare);
}

/* ---- accuracy ---- */

static int shared_val_accuracy(void) {
    int maj, min, rel;
    kernel_ver(&maj, &min, &rel);
    char path[PATH_SZ];

    if (maj == 2 && min == 4) {
        build_path(path, sizeof(path), "meminfo");
        FILE *f = fopen(path, "r");
        if (!f) return 1;
        char buf[8192];
        size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[rd] = '\0';
        return strstr(buf, "Inact_") ? 1 : 0;
    }

    build_path(path, sizeof(path), "%d/smaps", (int)our_pid);
    if (maj > 2 || (maj == 2 && min >= 6)) {
        if (access(path, F_OK) == 0) {
            FILE *f = fopen(path, "r");
            if (!f) return 1;
            char buf[8192];
            size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[rd] = '\0';
            return strstr(buf, "Pss:") ? 2 : 1;
        }
        if (maj == 2 && min == 6 && rel >= 1 && rel <= 9) return -1;
        return 0;
    }
    return 1;
}

static void show_accuracy(int acc, int only_total) {
    const char *lvl = only_total ? "Error" : "Warning";
    if (acc == -1) {
        fprintf(stderr, "%s: Shared memory is not reported by this system.\n", lvl);
        fprintf(stderr, "Values reported will be too large, and totals are not reported\n");
    } else if (acc == 0) {
        fprintf(stderr, "%s: Shared memory is not reported accurately by this system.\n", lvl);
        fprintf(stderr, "Values reported could be too large, and totals are not reported\n");
    } else if (acc == 1) {
        fprintf(stderr, "%s: Shared memory is slightly over-estimated by this system\n"
                "for each program, so totals are not reported.\n", lvl);
    }
    fclose(stderr);
    if (only_total && acc != 2) exit(1);
}

/* ---- print ---- */

static void print_header(void) {
    printf(" Private  +   Shared  =  RAM used\tProgram\n\n");
}

static void print_memory_usage(void) {
    char ps[32], ss[32], ts[32];
    double total = 0;
    for (size_t i = 0; i < n_entries; i++) {
        Entry *e = &entries[i];
        double tot = e->priv_total + e->shared_total;
        if (tot == 0.0) continue;
        human(e->priv_total, ps, sizeof(ps));
        human(e->shared_total, ss, sizeof(ss));
        human(tot, ts, sizeof(ts));
        if (e->count > 1)
            printf("%9s + %9s = %9s\t%s (%d)\n", ps, ss, ts, e->cmd, e->count);
        else
            printf("%9s + %9s = %9s\t%s\n", ps, ss, ts, e->cmd);
        total += tot;
    }
    if (have_pss) {
        human(total, ts, sizeof(ts));
        printf("%s\n%24s%9s\n%s\n",
               "---------------------------------", "", ts,
               "=================================");
    }
}

static void print_total(void) {
    double total = 0;
    for (size_t i = 0; i < n_entries; i++)
        total += entries[i].priv_total + entries[i].shared_total;
    printf("%.0f\n", total * 1024.0);
}

/* ---- usage ---- */

static void usage(void) {
    printf("Usage: ps_mem [OPTION]...\n"
           "Show program core memory usage\n\n"
           "  -h, -help                   Show this help\n"
           "  -p <pid>[,pid2,...pidN]     Only show memory usage PIDs in the specified list\n"
           "  -s, --split-args            Show and separate by, all command line arguments\n"
           "  -t, --total                 Show only the total value\n"
           "  -w <N>                      Measure and show process memory every N seconds\n");
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
    struct utsname un;
    uname(&un);
    if (strcmp(un.sysname, "FreeBSD") == 0)
        snprintf(proc_root, sizeof(proc_root), "/compat/linux/proc");

    our_pid = getpid();
    page_kb = (double)sysconf(_SC_PAGE_SIZE) / 1024.0;

    int split_args = 0, only_total = 0, watch = 0;
    pid_t filter[1024];
    int nfilter = 0;

    static const struct option long_opts[] = {
        {"split-args", no_argument, NULL, 's'},
        {"help",       no_argument, NULL, 'h'},
        {"total",      no_argument, NULL, 't'},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "shtp:w:", long_opts, NULL)) != -1) {
        switch (c) {
        case 's': split_args = 1; break;
        case 'h': usage(); return 0;
        case 't': only_total = 1; break;
        case 'p': {
            char *save = NULL, *copy = strdup(optarg);
            char *tok = strtok_r(copy, ",", &save);
            while (tok && nfilter < 1024) {
                filter[nfilter++] = (pid_t)atoi(tok);
                tok = strtok_r(NULL, ",", &save);
            }
            free(copy);
            break;
        }
        case 'w': watch = atoi(optarg); break;
        default:
            fprintf(stderr, "Usage: ps_mem [OPTION]...\n");
            return 3;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "Extraneous arguments: %s\n", argv[optind]);
        return 3;
    }

    if (geteuid() != 0) {
        fprintf(stderr, "Sorry, root permission required.\n");
        return 1;
    }
    {
        char path[PATH_SZ];
        build_path(path, sizeof(path), "sys/kernel/osrelease");
        if (access(path, F_OK) != 0) {
            fprintf(stderr, "Couldn't access %s\n"
                    "Only GNU/Linux and FreeBSD (with linprocfs) are supported\n",
                    proc_root);
            return 2;
        }
    }

    if (!only_total) print_header();

    if (watch > 0) {
        for (;;) {
            free_entries();
            get_memory_usage(nfilter ? filter : NULL, nfilter, split_args);
            if (n_entries == 0) {
                printf("Process does not exist anymore.\n");
                break;
            }
            if (only_total && have_pss)
                print_total();
            else if (!only_total)
                print_memory_usage();
            sleep((unsigned)watch);
        }
    } else {
        get_memory_usage(nfilter ? filter : NULL, nfilter, split_args);
        if (only_total && have_pss)
            print_total();
        else if (!only_total)
            print_memory_usage();
    }

    int acc = shared_val_accuracy();
    show_accuracy(acc, only_total);
    return 0;
}
