/* ps_mem.c — Cross-platform per-program memory reporter
 * Linux:   /proc/<pid>/smaps_rollup  (PSS-accurate)
 * macOS:   TASK_VM_INFO.phys_footprint (Activity Monitor equivalent)
 * Windows: PROCESS_MEMORY_COUNTERS_EX.PrivateUsage
 * Licence: LGPLv2 — based on pixelb/ps_mem
 */
#define _GNU_SOURCE

#if defined(__linux__)
#  define PS_LINUX 1
#elif defined(__APPLE__) && defined(__MACH__)
#  define PS_MACOS 1
#elif defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
#  define _WIN32_WINNT 0x0600
#  define PS_WINDOWS 1
#else
#  error "Unsupported platform"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#if defined(PS_LINUX)
#  include <dirent.h>
#  include <unistd.h>
#  include <getopt.h>
#  include <sys/stat.h>
#  include <sys/utsname.h>
#  include <stdint.h>
#elif defined(PS_MACOS)
#  include <unistd.h>
#  include <getopt.h>
#  include <sys/sysctl.h>
#  include <libproc.h>
#  include <mach/mach.h>
#  include <mach/task_info.h>
#elif defined(PS_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#  include <tlhelp32.h>
#  define strtok_r strtok_s
static int   _optind = 1;
static char *_optarg = NULL;
#  define optind _optind
#  define optarg _optarg
static int _getopt(int argc, char *const *argv, const char *opts) {
    while (_optind < argc) {
        if (argv[_optind][0] != '-' || argv[_optind][1] == '\0') return -1;
        if (argv[_optind][1] == '-') { _optind++; return -1; }
        char c = argv[_optind][1];
        const char *p = strchr(opts, c);
        if (!p) { _optind++; return '?'; }
        if (*(p+1) == ':') {
            if (argv[_optind][2] != '\0') { _optarg = &argv[_optind][2]; }
            else { _optind++; if (_optind >= argc) return '?'; _optarg = argv[_optind]; }
        }
        _optind++; return (int)(unsigned char)c;
    }
    return -1;
}
#  define getopt _getopt
#endif

#define PATH_SZ 512
#define CMD_SZ  256
#define LINE_SZ 256

/* ---- Entry storage ---- */
typedef struct {
    char          *cmd;
    double         priv_total;
    double         shared_total;
    int            count;
    unsigned long *mem_ids;
    size_t         n_ids, cap_ids;
} Entry;

static Entry  *entries     = NULL;
static size_t  n_entries   = 0;
static size_t  cap_entries = 0;
static int     have_pss    = 0;

static void *xrealloc(void *p, size_t sz) {
    p = realloc(p, sz); if (!p) { perror("realloc"); exit(1); } return p;
}
static void free_entries(void) {
    for (size_t i = 0; i < n_entries; i++) { free(entries[i].cmd); free(entries[i].mem_ids); }
    free(entries); entries = NULL; n_entries = cap_entries = 0; have_pss = 0;
}
static Entry *find_or_create(const char *cmd) {
    for (size_t i = 0; i < n_entries; i++)
        if (strcmp(entries[i].cmd, cmd) == 0) return &entries[i];
    if (n_entries == cap_entries) {
        cap_entries = cap_entries ? cap_entries * 2 : 16;
        entries = xrealloc(entries, cap_entries * sizeof(Entry));
    }
    Entry *e = &entries[n_entries++]; memset(e, 0, sizeof(*e));
    e->cmd = strdup(cmd); if (!e->cmd) { perror("strdup"); exit(1); }
    return e;
}
static void entry_add_id(Entry *e, unsigned long id) {
    for (size_t i = 0; i < e->n_ids; i++) if (e->mem_ids[i] == id) return;
    if (e->n_ids == e->cap_ids) {
        e->cap_ids = e->cap_ids ? e->cap_ids * 2 : 4;
        e->mem_ids = xrealloc(e->mem_ids, e->cap_ids * sizeof(unsigned long));
    }
    e->mem_ids[e->n_ids++] = id;
}
static int entry_compare(const void *a, const void *b) {
    double ta = ((Entry*)a)->priv_total + ((Entry*)a)->shared_total;
    double tb = ((Entry*)b)->priv_total + ((Entry*)b)->shared_total;
    return (ta > tb) - (ta < tb);
}

/* ---- Output ---- */
static void human(double kb, char *buf, size_t sz) {
    const char *p[] = {"Ki","Mi","Gi","Ti"}; int u = 0; double v = kb;
    while (v >= 1000.0 && u < 3) { v /= 1024.0; u++; }
    snprintf(buf, sz, "%.1f %sB", v, p[u]);
}
static void print_header(void) { printf(" Private  +   Shared  =  RAM used\tProgram\n\n"); }
static void print_memory_usage(void) {
    char ps[32], ss[32], ts[32]; double total = 0;
    for (size_t i = 0; i < n_entries; i++) {
        Entry *e = &entries[i];
        double tot = e->priv_total + e->shared_total; if (tot == 0.0) continue;
        human(e->priv_total, ps, sizeof(ps));
        human(e->shared_total, ss, sizeof(ss));
        human(tot, ts, sizeof(ts));
        if (e->count > 1) printf("%9s + %9s = %9s\t%s (%d)\n", ps, ss, ts, e->cmd, e->count);
        else               printf("%9s + %9s = %9s\t%s\n",      ps, ss, ts, e->cmd);
        total += tot;
    }
    if (have_pss) { human(total, ts, sizeof(ts)); printf("%s\n%24s%9s\n%s\n", "---------------------------------","",ts,"================================="); }
}
static void print_total(void) {
    double t = 0;
    for (size_t i = 0; i < n_entries; i++) t += entries[i].priv_total + entries[i].shared_total;
    printf("%.0f\n", t * 1024.0);
}
static void usage(void) {
    printf("Usage: ps_mem [OPTION]...\nShow program core memory usage\n\n"
           "  -h, -help                   Show this help\n"
           "  -p <pid>[,pid2,...pidN]     Only show memory usage PIDs in the specified list\n"
           "  -s, --split-args            Show and separate by, all command line arguments\n"
           "  -t, --total                 Show only the total value\n"
           "  -w <N>                      Measure and show process memory every N seconds\n");
}

/* ================================================================ LINUX ================================================================ */
#ifdef PS_LINUX
static char   proc_root[PATH_SZ] = "/proc";
static pid_t  our_pid;
static double page_kb;

static void build_path(char *buf, size_t sz, const char *fmt, ...) {
    char rest[PATH_SZ]; va_list ap; va_start(ap, fmt); vsnprintf(rest, sizeof(rest), fmt, ap); va_end(ap);
    snprintf(buf, sz, "%s/%s", proc_root, rest);
}
static void kernel_ver(int *maj, int *min, int *rel) {
    *maj = *min = *rel = 0;
    char path[PATH_SZ]; build_path(path, sizeof(path), "sys/kernel/osrelease");
    FILE *f = fopen(path, "r"); if (!f) return;
    char ver[64]; if (fgets(ver, sizeof(ver), f)) {
        char *p = ver, *e;
        *maj = (int)strtol(p,&e,10); if (*e=='.') p=e+1;
        *min = (int)strtol(p,&e,10); if (*e=='.') p=e+1;
        *rel = (int)strtol(p,NULL,10);
    }
    fclose(f);
}
static double value_after_colon(const char *line) { const char *p = strchr(line,':'); return p ? strtod(p+1,NULL) : 0.0; }
static int get_cmd_name(pid_t pid, int split_args, char *buf, size_t sz) {
    char path[PATH_SZ], exe[PATH_SZ]; ssize_t n;
    build_path(path, sizeof(path), "%d/exe", (int)pid);
    n = readlink(path, exe, sizeof(exe)-1); if (n < 0) return -1; exe[n] = '\0';
    if (split_args) {
        build_path(path, sizeof(path), "%d/cmdline", (int)pid);
        FILE *f = fopen(path,"r"); if (!f) return -1;
        size_t rd = fread(buf,1,sz-1,f); fclose(f); buf[rd]='\0';
        for (size_t i=0; i<rd; i++) if (buf[i]=='\0') buf[i]=' ';
        while (rd>0 && buf[rd-1]==' ') buf[--rd]='\0';
        return 0;
    }
    int deleted = 0;
    if (n>10 && strcmp(exe+n-10," (deleted)")==0) { exe[n-10]='\0'; deleted=1; }
    if (deleted) {
        if (access(exe,F_OK)==0) snprintf(buf,sz,"%s [updated]",exe);
        else {
            build_path(path,sizeof(path),"%d/cmdline",(int)pid);
            FILE *f=fopen(path,"r"); char cl[PATH_SZ]={0};
            if (f) { fread(cl,1,sizeof(cl)-1,f); fclose(f); }
            if (cl[0] && access(cl,F_OK)==0) snprintf(buf,sz,"%s [updated]",cl);
            else snprintf(buf,sz,"%s [deleted]",exe);
        }
        return 0;
    }
    const char *base = strrchr(exe,'/'); base = base ? base+1 : exe;
    build_path(path,sizeof(path),"%d/status",(int)pid);
    FILE *f = fopen(path,"r"); if (!f) return -1;
    char nl[CMD_SZ]={0};
    if (fgets(nl,sizeof(nl),f)) {
        char *p = nl;
        if (strncmp(p,"Name:",5)==0) {
            p+=5; while (*p=='\t'||*p==' ') p++;
            size_t len=strlen(p); while (len>0&&(p[len-1]=='\n'||p[len-1]=='\r')) p[--len]='\0';
            snprintf(buf,sz,"%s", strncmp(base,p,len)==0 ? base : p);
        } else snprintf(buf,sz,"%s",base);
    } else snprintf(buf,sz,"%s",base);
    fclose(f); return 0;
}
static int get_mem_stats(pid_t pid, double *op, double *os, unsigned long *oid) {
    char path[PATH_SZ];
    build_path(path,sizeof(path),"%d/smaps_rollup",(int)pid);
    if (access(path,F_OK)!=0) build_path(path,sizeof(path),"%d/smaps",(int)pid);
    *oid = (unsigned long)pid;
    FILE *f = fopen(path,"r");
    if (!f) {
        build_path(path,sizeof(path),"%d/statm",(int)pid); f=fopen(path,"r"); if (!f) return -1;
        unsigned long long rss=0,shr=0; int rv=fscanf(f,"%*llu %llu %llu",&rss,&shr); fclose(f);
        if (rv<2) return -1; *op=(double)(rss-shr)*page_kb; *os=(double)shr*page_kb; return 0;
    }
    unsigned long h=2166136261UL; double priv=0,shr=0,pss=0; int lpss=0; char line[LINE_SZ];
    while (fgets(line,sizeof(line),f)) {
        size_t len=strlen(line); for (size_t i=0;i<len;i++) h=(h^(unsigned char)line[i])*16777619UL;
        if      (strncmp(line,"Private_",8)==0) priv+=value_after_colon(line);
        else if (strncmp(line,"Shared_", 7)==0) shr +=value_after_colon(line);
        else if (strncmp(line,"Pss:",    4)==0) { pss+=value_after_colon(line)+0.5; lpss=1; }
    }
    fclose(f); *oid=h;
    if (lpss) { have_pss=1; *op=priv; *os=pss-priv; } else { *op=priv; *os=shr; }
    return 0;
}
static void get_memory_usage_linux(int *filter, int nfilter, int split_args) {
    DIR *d = opendir(proc_root); if (!d) { perror(proc_root); exit(1); }
    struct dirent *de;
    while ((de=readdir(d))!=NULL) {
        char *end; long lpid=strtol(de->d_name,&end,10); if (*end!='\0') continue;
        pid_t pid=(pid_t)lpid; if (pid==our_pid) continue;
        if (nfilter>0) {
            int found=0; for (int i=0;i<nfilter;i++) if (filter[i]==(int)pid){found=1;break;}
            if (!found) continue;
        }
        char cmd[CMD_SZ]; if (get_cmd_name(pid,split_args,cmd,sizeof(cmd))<0) continue;
        double priv,shared; unsigned long mid;
        if (get_mem_stats(pid,&priv,&shared,&mid)<0) continue;
        Entry *e=find_or_create(cmd);
        if (have_pss) e->shared_total+=shared; else if (shared>e->shared_total) e->shared_total=shared;
        e->priv_total+=priv; e->count++; entry_add_id(e,mid);
    }
    closedir(d);
    for (size_t i=0;i<n_entries;i++) {
        Entry *e=&entries[i];
        if (e->n_ids==1&&e->count>1) { e->priv_total/=e->count; if (have_pss) e->shared_total/=e->count; }
    }
    qsort(entries,n_entries,sizeof(Entry),entry_compare);
}
static int shared_val_accuracy(void) {
    int maj,min,rel; kernel_ver(&maj,&min,&rel); char path[PATH_SZ];
    if (maj==2&&min==4) {
        build_path(path,sizeof(path),"meminfo"); FILE *f=fopen(path,"r"); if (!f) return 1;
        char buf[8192]; size_t rd=fread(buf,1,sizeof(buf)-1,f); fclose(f); buf[rd]='\0';
        return strstr(buf,"Inact_")?1:0;
    }
    build_path(path,sizeof(path),"%d/smaps",(int)our_pid);
    if (maj>2||(maj==2&&min>=6)) {
        if (access(path,F_OK)==0) {
            FILE *f=fopen(path,"r"); if (!f) return 1;
            char buf[8192]; size_t rd=fread(buf,1,sizeof(buf)-1,f); fclose(f); buf[rd]='\0';
            return strstr(buf,"Pss:")?2:1;
        }
        if (maj==2&&min==6&&rel>=1&&rel<=9) return -1; return 0;
    }
    return 1;
}
static void show_accuracy(int acc, int ot) {
    const char *l=ot?"Error":"Warning";
    if (acc==-1) { fprintf(stderr,"%s: Shared memory is not reported by this system.\nValues reported will be too large, and totals are not reported\n",l); }
    else if (acc==0) { fprintf(stderr,"%s: Shared memory is not reported accurately by this system.\nValues reported could be too large, and totals are not reported\n",l); }
    else if (acc==1) { fprintf(stderr,"%s: Shared memory is slightly over-estimated by this system\nfor each program, so totals are not reported.\n",l); }
    fclose(stderr); if (ot&&acc!=2) exit(1);
}
static void verify_linux(void) {
    if (geteuid()!=0) { fprintf(stderr,"Sorry, root permission required.\n"); exit(1); }
    char path[PATH_SZ]; build_path(path,sizeof(path),"sys/kernel/osrelease");
    if (access(path,F_OK)!=0) { fprintf(stderr,"Couldn't access %s\nOnly GNU/Linux and FreeBSD (with linprocfs) are supported\n",proc_root); exit(2); }
}
#endif /* PS_LINUX */

/* ================================================================ macOS ================================================================ */
#ifdef PS_MACOS
static void get_memory_usage_macos(int *filter, int nfilter, int split_args) {
    pid_t our_pid = getpid();
    int n = proc_listallpids(NULL, 0); if (n<=0) return;
    pid_t *pids = malloc((n+64)*sizeof(pid_t)); if (!pids) return;
    n = proc_listallpids(pids, (n+64)*sizeof(pid_t));
    for (int i=0; i<n; i++) {
        pid_t pid = pids[i]; if (pid<=0||pid==our_pid) continue;
        if (nfilter>0) {
            int found=0; for (int j=0;j<nfilter;j++) if (filter[j]==(int)pid){found=1;break;}
            if (!found) continue;
        }
        mach_port_t task=MACH_PORT_NULL;
        if (task_for_pid(mach_task_self(),pid,&task)!=KERN_SUCCESS) continue;
        task_vm_info_data_t vi; mach_msg_type_number_t cnt=TASK_VM_INFO_COUNT;
        kern_return_t kr=task_info(task,TASK_VM_INFO,(task_info_t)&vi,&cnt);
        mach_port_deallocate(mach_task_self(),task);
        if (kr!=KERN_SUCCESS) continue;
        double priv_kb = vi.phys_footprint/1024.0;
        double rss_kb  = vi.resident_size /1024.0;
        double shr_kb  = rss_kb>priv_kb ? rss_kb-priv_kb : 0;
        char cmd[CMD_SZ]={0};
        if (split_args) {
            size_t argmax=0, sz=sizeof(argmax);
            sysctlbyname("kern.argmax",&argmax,&sz,NULL,0);
            if (argmax>0) {
                int mib[3]={CTL_KERN,KERN_PROCARGS2,(int)pid}; char *buf=malloc(argmax);
                if (buf) {
                    sz=argmax;
                    if (sysctl(mib,3,buf,&sz,NULL,0)==0 && sz>(size_t)sizeof(int)) {
                        int argc=*(int*)buf; char *p=buf+sizeof(int);
                        p+=strlen(p)+1; while ((size_t)(p-buf)<sz&&*p=='\0') p++;
                        size_t off=0;
                        for (int a=0;a<argc&&(size_t)(p-buf)<sz;a++) {
                            size_t pl=strlen(p);
                            if (off+pl+2<CMD_SZ) { if (off) cmd[off++]=' '; memcpy(cmd+off,p,pl); off+=pl; }
                            p+=pl+1;
                        }
                        cmd[off]='\0';
                    }
                    free(buf);
                }
            }
        }
        if (!cmd[0]) {
            char path[PROC_PIDPATHINFO_MAXSIZE]={0};
            if (proc_pidpath(pid,path,sizeof(path))>0) {
                char *b=strrchr(path,'/'); snprintf(cmd,sizeof(cmd),"%s",b?b+1:path);
            } else proc_name(pid,cmd,sizeof(cmd));
        }
        if (!cmd[0]) continue;
        Entry *e=find_or_create(cmd);
        e->shared_total+=shr_kb; e->priv_total+=priv_kb; e->count++;
    }
    free(pids);
    have_pss=1;
    qsort(entries,n_entries,sizeof(Entry),entry_compare);
}
static void verify_macos(void) {
    if (geteuid()!=0) { fprintf(stderr,"Sorry, root permission required.\n"); exit(1); }
}
#endif /* PS_MACOS */

/* ================================================================ WINDOWS ================================================================ */
#ifdef PS_WINDOWS
static void get_memory_usage_windows(int *filter, int nfilter, int split_args) {
    (void)split_args;
    DWORD our_pid=GetCurrentProcessId();
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snap==INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32 pe; pe.dwSize=sizeof(pe);
    if (!Process32First(snap,&pe)) { CloseHandle(snap); return; }
    do {
        DWORD pid=pe.th32ProcessID; if (pid==0||pid==our_pid) continue;
        if (nfilter>0) {
            int found=0; for (int i=0;i<nfilter;i++) if (filter[i]==(int)pid){found=1;break;}
            if (!found) continue;
        }
        HANDLE proc=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
        if (!proc) continue;
        PROCESS_MEMORY_COUNTERS_EX pmc; pmc.cb=sizeof(pmc);
        if (!GetProcessMemoryInfo(proc,(PROCESS_MEMORY_COUNTERS*)&pmc,sizeof(pmc))) { CloseHandle(proc); continue; }
        double ws_kb  =(double)pmc.WorkingSetSize/1024.0;
        double priv_kb=(double)pmc.PrivateUsage  /1024.0;
        if (priv_kb>ws_kb) priv_kb=ws_kb;
        double shr_kb =ws_kb-priv_kb;
        char cmd[CMD_SZ]={0}; char full[MAX_PATH]={0}; DWORD fsz=MAX_PATH;
        if (QueryFullProcessImageNameA(proc,0,full,&fsz)) {
            char *b=strrchr(full,'\\'); strncpy(cmd,b?b+1:full,CMD_SZ-1);
        } else strncpy(cmd,pe.szExeFile,CMD_SZ-1);
        CloseHandle(proc);
        char *dot=strrchr(cmd,'.'); if (dot&&_stricmp(dot,".exe")==0) *dot='\0';
        if (!cmd[0]) continue;
        Entry *e=find_or_create(cmd);
        if (shr_kb>e->shared_total) e->shared_total=shr_kb;
        e->priv_total+=priv_kb; e->count++;
    } while (Process32Next(snap,&pe));
    CloseHandle(snap);
    have_pss=1;
    qsort(entries,n_entries,sizeof(Entry),entry_compare);
}
static void verify_windows(void) {
    BOOL admin=FALSE; HANDLE tok=NULL;
    if (OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&tok)) {
        TOKEN_ELEVATION elev; DWORD sz;
        if (GetTokenInformation(tok,TokenElevation,&elev,sizeof(elev),&sz)) admin=(BOOL)elev.TokenIsElevated;
        CloseHandle(tok);
    }
    if (!admin) fprintf(stderr,"Warning: not running as Administrator; some processes may be invisible.\n");
}
#endif /* PS_WINDOWS */

/* ================================================================ MAIN ================================================================ */
int main(int argc, char *argv[]) {
#ifdef PS_LINUX
    { struct utsname un; uname(&un); if (strcmp(un.sysname,"FreeBSD")==0) snprintf(proc_root,sizeof(proc_root),"/compat/linux/proc"); }
    our_pid=getpid(); page_kb=(double)sysconf(_SC_PAGE_SIZE)/1024.0;
#endif
#ifdef PS_WINDOWS
    for (int i=1;i<argc;i++) {
        if      (strcmp(argv[i],"--split-args")==0) argv[i]=(char*)"-s";
        else if (strcmp(argv[i],"--help"      )==0) argv[i]=(char*)"-h";
        else if (strcmp(argv[i],"--total"     )==0) argv[i]=(char*)"-t";
    }
#endif
    int split_args=0, only_total=0, watch=0, filter[1024], nfilter=0;
#if defined(PS_LINUX) || defined(PS_MACOS)
    static const struct option lo[] = {{"split-args",no_argument,NULL,'s'},{"help",no_argument,NULL,'h'},{"total",no_argument,NULL,'t'},{NULL,0,NULL,0}};
    int c; while ((c=getopt_long(argc,argv,"shtp:w:",lo,NULL))!=-1) {
#else
    int c; while ((c=getopt(argc,argv,"shtp:w:"))!=-1) {
#endif
        switch(c) {
        case 's': split_args=1; break;
        case 'h': usage(); return 0;
        case 't': only_total=1; break;
        case 'p': { char *sv=NULL,*cp=strdup(optarg); char *t=strtok_r(cp,",",&sv);
                    while (t&&nfilter<1024){filter[nfilter++]=atoi(t);t=strtok_r(NULL,",",&sv);}
                    free(cp); break; }
        case 'w': watch=atoi(optarg); break;
        default: fprintf(stderr,"Usage: ps_mem [OPTION]...\n"); return 3;
        }
    }
#ifdef PS_LINUX
    if (optind<argc) { fprintf(stderr,"Extraneous arguments: %s\n",argv[optind]); return 3; }
    verify_linux();
#elif defined(PS_MACOS)
    if (optind<argc) { fprintf(stderr,"Extraneous arguments: %s\n",argv[optind]); return 3; }
    verify_macos();
#elif defined(PS_WINDOWS)
    if (_optind<argc) { fprintf(stderr,"Extraneous arguments: %s\n",argv[_optind]); return 3; }
    verify_windows();
#endif
    if (!only_total) print_header();
    do {
        free_entries();
#ifdef PS_LINUX
        get_memory_usage_linux(filter,nfilter,split_args);
#elif defined(PS_MACOS)
        get_memory_usage_macos(filter,nfilter,split_args);
#elif defined(PS_WINDOWS)
        get_memory_usage_windows(filter,nfilter,split_args);
#endif
        if (only_total&&have_pss) print_total(); else if (!only_total) print_memory_usage();
        if (watch>0) {
#ifdef PS_WINDOWS
            Sleep((DWORD)watch*1000);
#else
            sleep((unsigned)watch);
#endif
        }
    } while (watch>0);
#ifdef PS_LINUX
    show_accuracy(shared_val_accuracy(), only_total);
#endif
    return 0;
}
