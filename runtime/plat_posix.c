/* Linux and macOS builds: the Win32 functions of plat/windows.h on POSIX. Compiled into the exe only off Windows. On macOS
 * (plan/phases/MAC1_port.md) the Linux interfaces used here come from Mach, dyld and libSystem (the block after the includes). */
#define _GNU_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "plat/windows.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <dlfcn.h>
#ifndef __APPLE__
#include <link.h>
#endif
#include <limits.h>
#if !(defined(__APPLE__) && defined(__aarch64__))
#include <ucontext.h>
#endif
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/resource.h>
#ifndef __APPLE__
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <linux/fs.h>
#endif
#include <sys/ioctl.h>
#include <SDL3/SDL.h>
#include <sys/random.h>
#include <signal.h>
#include <pwd.h>
#include <unwind.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/sysctl.h>
#include <libkern/OSCacheControl.h>
#include <os/os_sync_wait_on_address.h>
#define PLAT_SO_EXT ".dylib"
#ifndef MAP_STACK
#define MAP_STACK 0
#endif
#define MAP_FIXED_NOREPLACE 0          /* the address is a hint: VirtualAlloc checks where the mapping landed */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
static int get_nprocs(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
static int sched_getcpu(void) { return -1; }          /* macOS cannot say: GetCurrentProcessorNumber answers 0 */
static ssize_t getrandom(void *b, size_t n, unsigned flags) { (void)flags; if (n > 256) n = 256; return getentropy(b, n) ? -1 : (ssize_t)n; }
/* The futex of the condition variables: os_sync_wait_on_address (macOS 14.4+), the public form of the same wait on a word.
 * As with the futex a timeout fails with ETIMEDOUT; a wake with no waiter fails with ENOENT, which no caller looks at. */
#define FUTEX_WAIT_PRIVATE 0
#define FUTEX_WAKE_PRIVATE 1
static long futex(volatile uint32_t *a, int op, uint32_t v, const struct timespec *ts) {
    if (op == FUTEX_WAKE_PRIVATE)
        return v == 1 ? os_sync_wake_by_address_any((void *)a, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE) : os_sync_wake_by_address_all((void *)a, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    if (!ts) return os_sync_wait_on_address((void *)a, v, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
    return os_sync_wait_on_address_with_timeout((void *)a, v, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME,
                                                (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec);
}
/* The image a handle of LoadLibraryA belongs to (macOS has no dlinfo): every loaded image asked again with the same flags,
 * which gives the same handle. */
static int dyld_image_of(void *m, const char **path, uintptr_t *base) {
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        void *h = name ? dlopen(name, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD) : NULL;
        if (!h) continue;
        dlclose(h);
        if (h == m) { *path = name; *base = (uintptr_t)_dyld_get_image_header(i); return 1; }
    }
    return 0;
}
#else
#define PLAT_SO_EXT ".so"
#endif
static pid_t os_tid(void) {                    /* the kernel's id of the calling thread */
#ifdef __APPLE__
    uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return (pid_t)t;
#else
    return (pid_t)(long)syscall(SYS_gettid);
#endif
}
static ssize_t self_exe_path(char *b, size_t n) {          /* the exe's path, as readlink of /proc/self/exe gives it (b: PATH_MAX) */
#ifdef __APPLE__
    char raw[PATH_MAX];
    uint32_t sz = sizeof raw;
    (void)n;
    if (_NSGetExecutablePath(raw, &sz) || !realpath(raw, b)) return -1;
    return (ssize_t)strlen(b);
#else
    return readlink("/proc/self/exe", b, n);
#endif
}

#undef fopen
#undef remove
#undef rename

/* GetLastError: per thread, as on Windows; a failing POSIX call's errno becomes the nearest ERROR_ code (err_from_errno). */
static __thread DWORD t_lasterr;
DWORD GetLastError(void) { return t_lasterr; }
void SetLastError(DWORD e) { t_lasterr = e; }
static DWORD err_from_errno(int e) {
    switch (e) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case ENOSPC: return ERROR_DISK_FULL;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    case EFAULT: return ERROR_NOACCESS;
    default: return 0x1F;    /* ERROR_GEN_FAILURE */
    }
}
static BOOL fail_errno(void) { t_lasterr = err_from_errno(errno); return FALSE; }

/* ================================================================ time */
/* QueryPerformanceCounter: CLOCK_MONOTONIC in 100 ns units at a fixed 10 MHz frequency, which is what Windows 10/11 report. */
static int64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec; }
BOOL QueryPerformanceCounter(LARGE_INTEGER *t) { t->QuadPart = mono_ns() / 100; return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 10000000; return TRUE; }
ULONGLONG GetTickCount64(void) { return (ULONGLONG)(mono_ns() / 1000000); }
DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
#define EPOCH_1601 116444736000000000ULL           /* 100 ns intervals from 1601-01-01 to 1970-01-01 */
static void ft_set(FILETIME *ft, uint64_t v) { ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32); }
static uint64_t ts_to_ft(const struct timespec *ts) { return EPOCH_1601 + (uint64_t)ts->tv_sec * 10000000ULL + (uint64_t)ts->tv_nsec / 100; }
void GetSystemTimeAsFileTime(FILETIME *ft) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ft_set(ft, ts_to_ft(&ts)); }
static void tm_to_st(const struct tm *tm, long ns, SYSTEMTIME *st) {
    st->wYear = (WORD)(tm->tm_year + 1900); st->wMonth = (WORD)(tm->tm_mon + 1); st->wDayOfWeek = (WORD)tm->tm_wday; st->wDay = (WORD)tm->tm_mday;
    st->wHour = (WORD)tm->tm_hour; st->wMinute = (WORD)tm->tm_min; st->wSecond = (WORD)tm->tm_sec; st->wMilliseconds = (WORD)(ns / 1000000);
}
void GetSystemTime(SYSTEMTIME *st) { struct timespec ts; struct tm tm; clock_gettime(CLOCK_REALTIME, &ts); gmtime_r(&ts.tv_sec, &tm); tm_to_st(&tm, ts.tv_nsec, st); }
void GetLocalTime(SYSTEMTIME *st) { struct timespec ts; struct tm tm; clock_gettime(CLOCK_REALTIME, &ts); localtime_r(&ts.tv_sec, &tm); tm_to_st(&tm, ts.tv_nsec, st); }
BOOL GetSystemTimes(FILETIME *idle, FILETIME *kernel, FILETIME *user) {
    /* /proc/stat, USER_HZ ticks; kernel includes idle, as on Windows */
#ifdef __APPLE__                                  /* macOS: Mach's load counters, in the same 1/100 s ticks */
    host_cpu_load_info_data_t li;
    mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&li, &cnt) != KERN_SUCCESS) return FALSE;
    unsigned long long u = li.cpu_ticks[CPU_STATE_USER], n = li.cpu_ticks[CPU_STATE_NICE], s = li.cpu_ticks[CPU_STATE_SYSTEM],
                       i = li.cpu_ticks[CPU_STATE_IDLE], w = 0, q = 0, sq = 0;
#else
    unsigned long long u = 0, n = 0, s = 0, i = 0, w = 0, q = 0, sq = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return FALSE;
    int got = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &w, &q, &sq);
    fclose(f);
    if (got < 4) return FALSE;
#endif
    long hz = sysconf(_SC_CLK_TCK);
    uint64_t k = 10000000ULL / (uint64_t)(hz > 0 ? hz : 100);
    if (idle) ft_set(idle, (i + w) * k);
    if (kernel) ft_set(kernel, (s + q + sq + i + w) * k);
    if (user) ft_set(user, (u + n) * k);
    return TRUE;
}
BOOL GetProcessTimes(HANDLE p, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user) {
    (void)p;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    if (create) ft_set(create, 0);
    if (exit_) ft_set(exit_, 0);
    if (kernel) ft_set(kernel, (uint64_t)ru.ru_stime.tv_sec * 10000000ULL + (uint64_t)ru.ru_stime.tv_usec * 10);
    if (user) ft_set(user, (uint64_t)ru.ru_utime.tv_sec * 10000000ULL + (uint64_t)ru.ru_utime.tv_usec * 10);
    return TRUE;
}
/* Sleep(0) gives up the processor (sched_yield) as on Windows; other times are not rounded to a timer period (timeBeginPeriod is a no-op here). */
void Sleep(DWORD ms) {
    if (!ms) { sched_yield(); return; }
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

/* ================================================================ condition variables, init-once */
/* CONDITION_VARIABLE: a futex on a wake counter. As on Windows a waiter can return without a Wake (callers loop on their
 * condition), and a timeout returns FALSE with ERROR_TIMEOUT. */
#ifndef __APPLE__                                 /* (macOS: above) */
static long futex(volatile uint32_t *a, int op, uint32_t v, const struct timespec *ts) { return syscall(SYS_futex, a, op, v, ts, NULL, 0); }
#endif
static BOOL cv_wait(CONDITION_VARIABLE *cv, DWORD ms, void (*unlock)(void *), void (*lock)(void *), void *l) {
    uint32_t seq = __atomic_load_n(&cv->seq, __ATOMIC_SEQ_CST);
    unlock(l);
    long r;
    if (ms == INFINITE) r = futex(&cv->seq, FUTEX_WAIT_PRIVATE, seq, NULL);
    else { struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L}; r = futex(&cv->seq, FUTEX_WAIT_PRIVATE, seq, &ts); }
    int e = errno;
    lock(l);
    if (r == -1 && e == ETIMEDOUT) { t_lasterr = 1460; return FALSE; }      /* ERROR_TIMEOUT */
    return TRUE;
}
static void cs_unlock(void *l) { pthread_mutex_unlock(l); }
static void cs_lock(void *l) { pthread_mutex_lock(l); }
static void srw_unlock(void *l) { pthread_rwlock_unlock(l); }
static void srw_lock_x(void *l) { pthread_rwlock_wrlock(l); }
static void srw_lock_s(void *l) { pthread_rwlock_rdlock(l); }
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms) { return cv_wait(cv, ms, cs_unlock, cs_lock, cs); }
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags) {
    return cv_wait(cv, ms, srw_unlock, (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) ? srw_lock_s : srw_lock_x, l);
}
void WakeConditionVariable(CONDITION_VARIABLE *cv) { __atomic_add_fetch(&cv->seq, 1, __ATOMIC_SEQ_CST); futex(&cv->seq, FUTEX_WAKE_PRIVATE, 1, NULL); }
void WakeAllConditionVariable(CONDITION_VARIABLE *cv) { __atomic_add_fetch(&cv->seq, 1, __ATOMIC_SEQ_CST); futex(&cv->seq, FUTEX_WAKE_PRIVATE, INT_MAX, NULL); }
void InitializeCriticalSection(CRITICAL_SECTION *cs) {         /* a CRITICAL_SECTION is re-entrant on Windows: a recursive mutex */
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &a);
    pthread_mutexattr_destroy(&a);
}
BOOL InitOnceExecuteOnce(INIT_ONCE *once, PINIT_ONCE_FN fn, PVOID param, PVOID *ctx) {
    /* 0 not run, 1 running, 2 done. Per object (a callback may start another init-once); a failed callback lets the next caller retry. */
    for (;;) {
        int st = __atomic_load_n(&once->state, __ATOMIC_ACQUIRE);
        if (st == 2) return TRUE;
        if (st == 0 && __atomic_compare_exchange_n(&once->state, &st, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            BOOL ok = fn(once, param, ctx);
            __atomic_store_n(&once->state, ok ? 2 : 0, __ATOMIC_RELEASE);
            return ok;
        }
        sched_yield();
    }
}
BOOL InitOnceBeginInitialize(INIT_ONCE *once, DWORD flags, BOOL *pending, PVOID *ctx) {
    /* the same states: the caller that moves 0 -> 1 gets pending = TRUE and must call InitOnceComplete; the others wait */
    (void)flags;
    if (ctx) *ctx = NULL;
    for (;;) {
        int st = __atomic_load_n(&once->state, __ATOMIC_ACQUIRE);
        if (st == 2) { *pending = FALSE; return TRUE; }
        if (st == 0 && __atomic_compare_exchange_n(&once->state, &st, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { *pending = TRUE; return TRUE; }
        sched_yield();
    }
}
BOOL InitOnceComplete(INIT_ONCE *once, DWORD flags, PVOID ctx) {
    (void)ctx;
    __atomic_store_n(&once->state, (flags & INIT_ONCE_INIT_FAILED) ? 0 : 2, __ATOMIC_RELEASE);
    return TRUE;
}

/* ================================================================ handle objects */
enum { PO_THREAD = 1, PO_EVENT, PO_SEM, PO_MUTEX, PO_TIMER, PO_FILE, PO_FIND, PO_MAPPING, PO_MODULE };
typedef struct PlatObj {
    int kind;
    volatile int refs;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int signaled, manual;            /* events; threads: signaled = finished */
    LONG count, maxcount;            /* semaphores */
    pthread_t owner; int depth;      /* mutexes */
    int64_t due_ns;                  /* timers: CLOCK_MONOTONIC deadline */
    /* threads */
    pthread_t th; pid_t tid; DWORD exit_code; int started;
    LPTHREAD_START_ROUTINE fn; LPVOID arg;
    void **fiber_slot;               /* the thread's running fiber (its t_fiber), for plat_thread_fiber */
    uintptr_t stk_lo, stk_hi;        /* its own stack */
    volatile uint32_t sus_req, sus_ack, sus_res;    /* SuspendThread: stops asked / acknowledged by the thread, resumes */
    DWORD sus_count; void *sus_uc;   /* (g_sus_lock) suspend count; the stopped thread's signal context */
    /* files, finds, mappings */
    int fd;
    DIR *dir; char dir_path[MAX_PATH]; char pattern[MAX_PATH];
    size_t map_size;
} PlatObj;
#define PSEUDO_PROCESS ((HANDLE)(intptr_t)-1)
#define PSEUDO_THREAD ((HANDLE)(intptr_t)-2)

static pthread_condattr_t g_cv_mono;
static pthread_once_t g_cv_once = PTHREAD_ONCE_INIT;
#ifdef __APPLE__                                  /* macOS: no clock choice; the timed waits use CLOCK_REALTIME (abs_deadline) */
static void cv_attr_init(void) { pthread_condattr_init(&g_cv_mono); }
#else
static void cv_attr_init(void) { pthread_condattr_init(&g_cv_mono); pthread_condattr_setclock(&g_cv_mono, CLOCK_MONOTONIC); }
#endif
static PlatObj *obj_new(int kind) {
    pthread_once(&g_cv_once, cv_attr_init);
    PlatObj *o = calloc(1, sizeof *o);
    o->kind = kind; o->refs = 1; o->fd = -1;
    pthread_mutex_init(&o->m, NULL);
    pthread_cond_init(&o->cv, &g_cv_mono);
    return o;
}
static void obj_release(PlatObj *o) {
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (o->fd >= 0) close(o->fd);
    if (o->dir) closedir(o->dir);
    pthread_mutex_destroy(&o->m);
    pthread_cond_destroy(&o->cv);
    free(o);
}
#define FD_TAG 0x7f0000000000ULL                 /* _get_osfhandle: FD_TAG + fd */
#define PID_TAG 0x7e0000000000ULL                /* OpenProcess: PID_TAG + pid */
static int is_tagged(HANDLE h, uint64_t tag) { return ((uint64_t)(uintptr_t)h & ~0xffffffffULL) == tag; }
static int is_obj(HANDLE h) { return h && h != INVALID_HANDLE_VALUE && h != PSEUDO_THREAD && h != PSEUDO_PROCESS && !is_tagged(h, FD_TAG) && !is_tagged(h, PID_TAG); }

/* threads */
static __thread PlatObj *t_self;
/* A thread is never stopped by SuspendThread while it holds g_vm_lock (the rollback's caller asks VirtualQuery while
 * threads are stopped): the stop waits until it lets the lock go (sus_handler, sus_deferred). */
static __thread int t_nosus, t_sus_pending;
static void sus_deferred(void);
static void **this_fiber_slot(void);
static void thread_note(PlatObj *o) {                  /* the calling thread's fiber slot and stack bounds into its object */
    o->fiber_slot = this_fiber_slot();
#ifndef __APPLE__
    pthread_attr_t a; void *base; size_t sz;
    if (!pthread_getattr_np(pthread_self(), &a)) {
        if (!pthread_attr_getstack(&a, &base, &sz)) { o->stk_lo = (uintptr_t)base; o->stk_hi = (uintptr_t)base + sz; }
        pthread_attr_destroy(&a);
    }
#endif
}
static void *thread_tramp(void *p) {
    PlatObj *o = p;
    t_self = o;
    thread_note(o);
    o->tid = os_tid();
    pthread_mutex_lock(&o->m);
    while (!o->started) pthread_cond_wait(&o->cv, &o->m);     /* CREATE_SUSPENDED: held until ResumeThread */
    pthread_mutex_unlock(&o->m);
    DWORD r = o->fn(o->arg);
    pthread_mutex_lock(&o->m);
    o->exit_code = r; o->signaled = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    obj_release(o);
    return NULL;
}
HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *tid) {
    (void)sa;
    PlatObj *o = obj_new(PO_THREAD);
    o->fn = fn; o->arg = arg; o->refs = 2; o->started = !(flags & CREATE_SUSPENDED); o->exit_code = STILL_ACTIVE;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack ? (stack < (1 << 20) ? (1 << 20) : stack) : (SIZE_T)8 << 20);   /* Windows default: 1 MB; ours: 8 MB */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&o->th, &a, thread_tramp, o);
    pthread_attr_destroy(&a);
    if (e) { free(o); t_lasterr = err_from_errno(e); return NULL; }
    if (tid) { while (!__atomic_load_n(&o->tid, __ATOMIC_ACQUIRE)) sched_yield(); *tid = (DWORD)o->tid; }
    return o;
}
/* SuspendThread / ResumeThread of a running thread (Linux, x86-64; the rollback's save holds the guest threads at their
 * waits, r1_state.c). The stop is a signal (SIGRTMIN + 6): the thread's handler acknowledges it and waits until the
 * count drops back to 0; its signal context is what GetThreadContext reads. A thread blocked in a system call sits in
 * the handler with its RIP at (restarted) or just after (interrupted) the `syscall` instruction; the call is restarted
 * or returns EINTR (the runtime's waits take that as a spurious wake-up). A thread that holds g_vm_lock stops when it lets
 * it go. macOS: (DWORD)-1, which every caller handles. */
#if defined(__linux__) && defined(__x86_64__)
static int g_sus_sig;
static pthread_mutex_t g_sus_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_sus_once = PTHREAD_ONCE_INIT;
static void sus_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    PlatObj *o = t_self;
    if (!o) return;
    if (t_nosus) { t_sus_pending = 1; return; }        /* inside g_vm_lock: stopped at vm_unlock */
    int e = errno;
    uint32_t want = __atomic_load_n(&o->sus_req, __ATOMIC_ACQUIRE), r0 = __atomic_load_n(&o->sus_res, __ATOMIC_ACQUIRE);
    o->sus_uc = uc;
    __atomic_store_n(&o->sus_ack, want, __ATOMIC_RELEASE);
    futex(&o->sus_ack, FUTEX_WAKE_PRIVATE, INT_MAX, NULL);
    while (__atomic_load_n(&o->sus_res, __ATOMIC_ACQUIRE) == r0) futex(&o->sus_res, FUTEX_WAIT_PRIVATE, r0, NULL);
    errno = e;
}
static void sus_deferred(void) { t_sus_pending = 0; syscall(SYS_tgkill, getpid(), os_tid(), g_sus_sig); }
static void sus_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    g_sus_sig = SIGRTMIN + 6;
    sa.sa_sigaction = sus_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(g_sus_sig, &sa, NULL);
}
static PlatObj *sus_target(HANDLE h) {                 /* a started, running thread other than this one */
    if (!is_obj(h)) return NULL;
    PlatObj *o = h;
    if (o->kind != PO_THREAD || !o->started || __atomic_load_n(&o->signaled, __ATOMIC_ACQUIRE) || !o->tid || o->tid == os_tid()) return NULL;
    return o;
}
DWORD SuspendThread(HANDLE h) {
    PlatObj *o = sus_target(h);
    if (!o) return (DWORD)-1;
    pthread_once(&g_sus_once, sus_install);
    pthread_mutex_lock(&g_sus_lock);
    DWORD prev = o->sus_count;
    if (!prev) {
        uint32_t req = __atomic_add_fetch(&o->sus_req, 1, __ATOMIC_SEQ_CST);
        if (syscall(SYS_tgkill, getpid(), o->tid, g_sus_sig)) { pthread_mutex_unlock(&g_sus_lock); return (DWORD)-1; }
        for (;;) {
            uint32_t a = __atomic_load_n(&o->sus_ack, __ATOMIC_ACQUIRE);
            if (a == req) break;
            if (__atomic_load_n(&o->signaled, __ATOMIC_ACQUIRE)) { pthread_mutex_unlock(&g_sus_lock); return (DWORD)-1; }   /* it ended meanwhile */
            struct timespec ts = {0, 1000000};
            futex(&o->sus_ack, FUTEX_WAIT_PRIVATE, a, &ts);
        }
    }
    o->sus_count = prev + 1;
    pthread_mutex_unlock(&g_sus_lock);
    return prev;
}
static DWORD sus_resume(PlatObj *o) {
    pthread_mutex_lock(&g_sus_lock);
    DWORD prev = o->sus_count;
    if (prev && !--o->sus_count) {
        o->sus_uc = NULL;
        __atomic_add_fetch(&o->sus_res, 1, __ATOMIC_SEQ_CST);
        futex(&o->sus_res, FUTEX_WAKE_PRIVATE, INT_MAX, NULL);
    }
    pthread_mutex_unlock(&g_sus_lock);
    return prev;
}
BOOL GetThreadContext(HANDLE h, CONTEXT *c) {
    PlatObj *o = sus_target(h);
    if (!o || SuspendThread(h) == (DWORD)-1) return FALSE;       /* held already: the count goes up and back down */
    const greg_t *g = ((ucontext_t *)o->sus_uc)->uc_mcontext.gregs;
    c->Rax = g[REG_RAX]; c->Rcx = g[REG_RCX]; c->Rdx = g[REG_RDX]; c->Rbx = g[REG_RBX]; c->Rsp = g[REG_RSP]; c->Rbp = g[REG_RBP];
    c->Rsi = g[REG_RSI]; c->Rdi = g[REG_RDI]; c->R8 = g[REG_R8]; c->R9 = g[REG_R9]; c->R10 = g[REG_R10]; c->R11 = g[REG_R11];
    c->R12 = g[REG_R12]; c->R13 = g[REG_R13]; c->R14 = g[REG_R14]; c->R15 = g[REG_R15]; c->Rip = g[REG_RIP]; c->EFlags = (DWORD)g[REG_EFL];
    sus_resume(o);
    return TRUE;
}
#else
static void sus_deferred(void) { t_sus_pending = 0; }
static DWORD sus_resume(PlatObj *o) { (void)o; return 0; }
DWORD SuspendThread(HANDLE h) { (void)h; return (DWORD)-1; }
BOOL GetThreadContext(HANDLE h, CONTEXT *c) { (void)h; (void)c; return FALSE; }
#endif
DWORD ResumeThread(HANDLE h) {
    if (!is_obj(h)) return (DWORD)-1;
    PlatObj *o = h;
    if (o->kind == PO_THREAD && __atomic_load_n(&o->started, __ATOMIC_ACQUIRE)) return sus_resume(o);
    pthread_mutex_lock(&o->m);
    DWORD was = o->started ? 0 : 1;
    __atomic_store_n(&o->started, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    return was;
}
void *plat_thread_fiber(HANDLE h) {
    if (h == PSEUDO_THREAD) return *this_fiber_slot();
    if (!is_obj(h) || ((PlatObj *)h)->kind != PO_THREAD || !((PlatObj *)h)->fiber_slot) return NULL;
    return __atomic_load_n(((PlatObj *)h)->fiber_slot, __ATOMIC_RELAXED);
}
int plat_thread_stack(HANDLE h, uint64_t *lo, uint64_t *hi) {
    if (!is_obj(h) || ((PlatObj *)h)->kind != PO_THREAD || !((PlatObj *)h)->stk_hi) return 0;
    *lo = ((PlatObj *)h)->stk_lo; *hi = ((PlatObj *)h)->stk_hi;
    return 1;
}
HANDLE GetCurrentThread(void) { return PSEUDO_THREAD; }
HANDLE GetCurrentProcess(void) { return PSEUDO_PROCESS; }
DWORD GetCurrentThreadId(void) { static __thread DWORD id; if (!id) id = (DWORD)os_tid(); return id; }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD GetThreadId(HANDLE h) { if (h == PSEUDO_THREAD) return GetCurrentThreadId(); return is_obj(h) && ((PlatObj *)h)->kind == PO_THREAD ? (DWORD)((PlatObj *)h)->tid : 0; }
BOOL GetExitCodeThread(HANDLE h, DWORD *code) { if (!is_obj(h)) return FALSE; *code = ((PlatObj *)h)->exit_code; return TRUE; }
BOOL SetThreadPriority(HANDLE h, int prio) { (void)h; (void)prio; return TRUE; }   /* no real-time scheduling without privileges */
int GetThreadPriority(HANDLE h) { (void)h; return THREAD_PRIORITY_NORMAL; }
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
    pthread_t th = h == PSEUDO_THREAD ? pthread_self() : is_obj(h) ? ((PlatObj *)h)->th : 0;
    if (!th || !mask) return 0;
#ifndef __APPLE__                                 /* macOS has no thread affinity: the mask is accepted, not applied */
    cpu_set_t set; CPU_ZERO(&set);
    for (int i = 0; i < 64; i++) if (mask >> i & 1) CPU_SET(i, &set);
    if (pthread_setaffinity_np(th, sizeof set, &set)) return 0;
#endif
    return ~(DWORD_PTR)0 >> (64 - (get_nprocs() < 64 ? get_nprocs() : 64));
}
/* The kernel's thread name: at most 15 characters (Windows keeps the whole description); non-ASCII becomes '?'. */
HRESULT SetThreadDescription(HANDLE h, LPCWSTR name) {
    pthread_t th = h == PSEUDO_THREAD ? pthread_self() : is_obj(h) ? ((PlatObj *)h)->th : 0;
    char s[16];
    int i = 0;
    if (!th || !name) return E_FAIL;
    for (; i < 15 && name[i]; i++) s[i] = name[i] < 128 ? (char)name[i] : '?';
    s[i] = 0;
#ifdef __APPLE__                                  /* macOS names the calling thread only */
    if (!pthread_equal(th, pthread_self())) return E_FAIL;
    return pthread_setname_np(s) ? E_FAIL : S_OK;
#else
    return pthread_setname_np(th, s) ? E_FAIL : S_OK;
#endif
}
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys) {
    (void)p;
    int n = get_nprocs();
    DWORD_PTR m = n >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1);
    if (proc) *proc = m;
    if (sys) *sys = m;
    return TRUE;
}
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *dst, DWORD access, BOOL inherit, DWORD opts) {
    (void)sp; (void)tp; (void)access; (void)inherit; (void)opts;
    if (src == PSEUDO_THREAD) {                    /* a real handle to the calling thread */
        PlatObj *o = t_self;
        if (o) { __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL); *dst = o; return TRUE; }
        o = obj_new(PO_THREAD);
        o->th = pthread_self(); o->tid = os_tid(); o->started = 1; o->exit_code = STILL_ACTIVE;
        thread_note(o);
        o->refs = 2; t_self = o;                   /* the thread's own reference is never dropped: it was not made by CreateThread */
        *dst = o;
        return TRUE;
    }
    if (!is_obj(src)) return FALSE;
    __atomic_add_fetch(&((PlatObj *)src)->refs, 1, __ATOMIC_ACQ_REL);
    *dst = src;
    return TRUE;
}

/* events, semaphores, mutexes, timers. A wait takes the object as Windows does (ready_locked): an auto-reset event wakes ONE
 * waiter and is reset by it, a manual-reset event stays signaled until ResetEvent; a mutex belongs to a thread and is
 * re-entrant (an owner thread that ends without releasing it is not detected: no WAIT_ABANDONED); a waitable timer is
 * one-shot (period and completion routine are not implemented; the runtime uses neither). */
HANDLE CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, LPCSTR name) { (void)sa; (void)name; PlatObj *o = obj_new(PO_EVENT); o->manual = manual; o->signaled = initial; return o; }
BOOL SetEvent(HANDLE h) {
    PlatObj *o = h;
    pthread_mutex_lock(&o->m); o->signaled = 1;
    if (o->manual) pthread_cond_broadcast(&o->cv); else pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
BOOL ResetEvent(HANDLE h) { PlatObj *o = h; pthread_mutex_lock(&o->m); o->signaled = 0; pthread_mutex_unlock(&o->m); return TRUE; }
HANDLE CreateSemaphoreA(SECURITY_ATTRIBUTES *sa, LONG initial, LONG maximum, LPCSTR name) { (void)sa; (void)name; PlatObj *o = obj_new(PO_SEM); o->count = initial; o->maxcount = maximum; return o; }
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *prev) {
    PlatObj *o = h;
    pthread_mutex_lock(&o->m);
    if (prev) *prev = o->count;
    if (o->count + n > o->maxcount) { pthread_mutex_unlock(&o->m); t_lasterr = 298; return FALSE; }   /* ERROR_TOO_MANY_POSTS */
    o->count += n;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
HANDLE CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL owned, LPCSTR name) {
    (void)sa; (void)name;
    PlatObj *o = obj_new(PO_MUTEX);
    if (owned) { o->owner = pthread_self(); o->depth = 1; }
    return o;
}
BOOL ReleaseMutex(HANDLE h) {
    PlatObj *o = h;
    pthread_mutex_lock(&o->m);
    if (!o->depth || !pthread_equal(o->owner, pthread_self())) { pthread_mutex_unlock(&o->m); return FALSE; }
    if (!--o->depth) pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
HANDLE CreateWaitableTimerExW(SECURITY_ATTRIBUTES *sa, LPCWSTR name, DWORD flags, DWORD access) { (void)sa; (void)name; (void)flags; (void)access; return obj_new(PO_TIMER); }
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, LPVOID arg, BOOL resume) {
    (void)period; (void)fn; (void)arg; (void)resume;
    PlatObj *o = h;
    int64_t d = due->QuadPart;
    pthread_mutex_lock(&o->m);
    if (d < 0) o->due_ns = mono_ns() + (-d) * 100;
    else { FILETIME ft; GetSystemTimeAsFileTime(&ft); int64_t now = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime); o->due_ns = mono_ns() + (d - now) * 100; }
    o->signaled = 0;
    pthread_mutex_unlock(&o->m);
    return TRUE;
}

/* Is the object signaled for this thread? Takes it (auto-reset event, semaphore, mutex) when `take`. Called with o->m held. */
static int ready_locked(PlatObj *o, int take) {
    switch (o->kind) {
    case PO_THREAD: return o->signaled;
    case PO_EVENT: if (!o->signaled) return 0; if (take && !o->manual) o->signaled = 0; return 1;
    case PO_SEM: if (o->count <= 0) return 0; if (take) o->count--; return 1;
    case PO_MUTEX:
        if (o->depth && !pthread_equal(o->owner, pthread_self())) return 0;
        if (take) { o->owner = pthread_self(); o->depth++; }
        return 1;
    case PO_TIMER: if (o->signaled || (o->due_ns && mono_ns() >= o->due_ns)) { o->signaled = 1; return 1; } return 0;
    default: return 1;
    }
}
static void abs_deadline(struct timespec *ts, int64_t ns_from_now) {
#ifdef __APPLE__
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    int64_t t = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec + ns_from_now;
#else
    int64_t t = mono_ns() + ns_from_now;
#endif
    ts->tv_sec = t / 1000000000LL; ts->tv_nsec = t % 1000000000LL;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (is_tagged(h, PID_TAG)) {                    /* a process: poll */
        pid_t pid = (pid_t)((uintptr_t)h & 0xffffffff);
        int64_t end = ms == INFINITE ? INT64_MAX : mono_ns() + (int64_t)ms * 1000000LL;
        for (;;) { if (kill(pid, 0) && errno == ESRCH) return WAIT_OBJECT_0; if (mono_ns() >= end) return WAIT_TIMEOUT; Sleep(10); }
    }
    if (h == PSEUDO_THREAD || h == PSEUDO_PROCESS) { if (ms == INFINITE) for (;;) pause(); Sleep(ms); return WAIT_TIMEOUT; }
    if (!is_obj(h)) { t_lasterr = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
    PlatObj *o = h;
    struct timespec dl;
    if (ms != INFINITE) abs_deadline(&dl, (int64_t)ms * 1000000LL);
    pthread_mutex_lock(&o->m);
    DWORD r = WAIT_OBJECT_0;
    while (!ready_locked(o, 1)) {
        if (o->kind == PO_TIMER) {                  /* a timer has nothing that signals it: sleep to its deadline */
            int64_t left = o->due_ns - mono_ns();
            if (!o->due_ns) left = INT64_MAX / 2;
            if (ms != INFINITE && left > (int64_t)ms * 1000000LL) { pthread_mutex_unlock(&o->m); Sleep(ms); return WAIT_TIMEOUT; }
            pthread_mutex_unlock(&o->m);
            struct timespec ts = {left / 1000000000LL, left % 1000000000LL};
            if (left > 0) while (nanosleep(&ts, &ts) && errno == EINTR) {}
            pthread_mutex_lock(&o->m);
            continue;
        }
        if (ms == INFINITE) pthread_cond_wait(&o->cv, &o->m);
        else if (pthread_cond_timedwait(&o->cv, &o->m, &dl) == ETIMEDOUT) { if (!ready_locked(o, 1)) r = WAIT_TIMEOUT; break; }
    }
    pthread_mutex_unlock(&o->m);
    return r;
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    /* rare (diagnostic threads): polls every millisecond; with `all` the objects are taken one after another, not at once as on Windows */
    if (all && ms == INFINITE) {                   /* (the rollback's copy threads, r1_state.c cp_run, every frame: no 1 ms poll) */
        for (DWORD i = 0; i < n; i++) if (WaitForSingleObject(hs[i], INFINITE) != WAIT_OBJECT_0) return WAIT_FAILED;
        return WAIT_OBJECT_0;
    }
    int64_t end = ms == INFINITE ? INT64_MAX : mono_ns() + (int64_t)ms * 1000000LL;
    for (;;) {
        if (all) {
            DWORD ok = 0;
            for (DWORD i = 0; i < n; i++) { PlatObj *o = hs[i]; pthread_mutex_lock(&o->m); ok += ready_locked(o, 0) != 0; pthread_mutex_unlock(&o->m); }
            if (ok == n) { for (DWORD i = 0; i < n; i++) { PlatObj *o = hs[i]; pthread_mutex_lock(&o->m); ready_locked(o, 1); pthread_mutex_unlock(&o->m); } return WAIT_OBJECT_0; }
        } else for (DWORD i = 0; i < n; i++) {
            PlatObj *o = hs[i];
            pthread_mutex_lock(&o->m);
            int r = ready_locked(o, 1);
            pthread_mutex_unlock(&o->m);
            if (r) return WAIT_OBJECT_0 + i;
        }
        if (mono_ns() >= end) return WAIT_TIMEOUT;
        Sleep(1);
    }
}
BOOL CloseHandle(HANDLE h) {
    if (is_tagged(h, FD_TAG) || is_tagged(h, PID_TAG)) return TRUE;
    if (!is_obj(h)) return h == PSEUDO_THREAD || h == PSEUDO_PROCESS;
    PlatObj *o = h;
    if (o->kind == PO_MAPPING) { obj_release(o); return TRUE; }
    obj_release(o);
    return TRUE;
}
void ExitProcess(UINT code) { fflush(NULL); _exit((int)code); }
BOOL TerminateProcess(HANDLE p, UINT code) { if (p == PSEUDO_PROCESS) ExitProcess(code); return FALSE; }

/* ================================================================ thread-local slots */
DWORD TlsAlloc(void) { pthread_key_t k; return pthread_key_create(&k, NULL) ? TLS_OUT_OF_INDEXES : (DWORD)k; }
BOOL TlsFree(DWORD i) { return pthread_key_delete((pthread_key_t)i) == 0; }
LPVOID TlsGetValue(DWORD i) { return pthread_getspecific((pthread_key_t)i); }
BOOL TlsSetValue(DWORD i, LPVOID v) { return pthread_setspecific((pthread_key_t)i, v) == 0; }
/* FLS: the callback runs when a thread ends (pthread key destructor); Windows also runs it when a fiber is deleted. */
DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION cb) { pthread_key_t k; return pthread_key_create(&k, cb) ? FLS_OUT_OF_INDEXES : (DWORD)k; }
BOOL FlsFree(DWORD i) { return pthread_key_delete((pthread_key_t)i) == 0; }
PVOID FlsGetValue(DWORD i) { return pthread_getspecific((pthread_key_t)i); }
BOOL FlsSetValue(DWORD i, PVOID v) { return pthread_setspecific((pthread_key_t)i, v) == 0; }

/* ================================================================ fibers
 * On ucontext. As on Windows a fiber can be resumed on another thread than the one it last ran on (the running fiber is
 * per thread: t_fiber); swapcontext also saves and restores the signal mask, one system call per switch. The stack is
 * reserved whole at creation (MAP_NORESERVE: pages are used as touched), not grown from a commit size. */
#if defined(__APPLE__) && defined(__aarch64__)
/* macOS on Apple Silicon has no working ucontext: a switch of the callee-saved registers instead (x19-x28, fp, lr, sp,
 * d8-d15; everything else is the caller's to keep across a call). A new fiber's first switch "returns" into fiber_entry
 * on its own stack. The signal mask is not switched (ucontext did that with one system call per switch). */
typedef struct { uint64_t r[13]; uint64_t d[8]; } FiberRegs;        /* r: x19..x28, fp, lr, sp */
void plat_fiber_switch(FiberRegs *from, FiberRegs *to);
__asm__(".text\n.globl _plat_fiber_switch\n.p2align 2\n_plat_fiber_switch:\n"
        "  mov x9, sp\n"
        "  stp x19, x20, [x0, #0]\n  stp x21, x22, [x0, #16]\n  stp x23, x24, [x0, #32]\n  stp x25, x26, [x0, #48]\n"
        "  stp x27, x28, [x0, #64]\n  stp x29, x30, [x0, #80]\n  str x9, [x0, #96]\n"
        "  stp d8, d9, [x0, #104]\n  stp d10, d11, [x0, #120]\n  stp d12, d13, [x0, #136]\n  stp d14, d15, [x0, #152]\n"
        "  ldp x19, x20, [x1, #0]\n  ldp x21, x22, [x1, #16]\n  ldp x23, x24, [x1, #32]\n  ldp x25, x26, [x1, #48]\n"
        "  ldp x27, x28, [x1, #64]\n  ldp x29, x30, [x1, #80]\n  ldr x9, [x1, #96]\n  mov sp, x9\n"
        "  ldp d8, d9, [x1, #104]\n  ldp d10, d11, [x1, #120]\n  ldp d12, d13, [x1, #136]\n  ldp d14, d15, [x1, #152]\n"
        "  ret\n");
typedef struct { FiberRegs uc; LPFIBER_START_ROUTINE fn; LPVOID param; void *stack; size_t stack_size; int is_thread; } Fiber;
#else
typedef struct { ucontext_t uc; LPFIBER_START_ROUTINE fn; LPVOID param; void *stack; size_t stack_size; int is_thread; } Fiber;
#endif
_Static_assert(sizeof(Fiber) <= PLAT_FIBER_BLOCK, "a fiber block is PLAT_FIBER_BLOCK bytes (r1_state.c copies that much)");
static __thread Fiber *t_fiber;
static void **this_fiber_slot(void) { return (void **)&t_fiber; }
int plat_fiber_stack(const void *fiber, uint64_t *lo, uint64_t *hi) {
    const Fiber *f = fiber;
    if (!f || f->is_thread || !f->stack) return 0;
    *lo = (uintptr_t)f->stack; *hi = (uintptr_t)f->stack + f->stack_size;
    return 1;
}
uint64_t plat_fiber_saved_sp(const void *fiber) {
    const Fiber *f = fiber;
#if defined(__APPLE__) && defined(__aarch64__)
    return f->uc.r[12];
#else
    return (uint64_t)f->uc.uc_mcontext.gregs[REG_RSP];
#endif
}
LPVOID ConvertThreadToFiber(LPVOID param) {
    if (t_fiber) return t_fiber;
    Fiber *f = calloc(1, PLAT_FIBER_BLOCK);
    f->param = param; f->is_thread = 1;
    t_fiber = f;
    return f;
}
BOOL ConvertFiberToThread(void) { if (t_fiber && t_fiber->is_thread) free(t_fiber); t_fiber = NULL; return TRUE; }
static void fiber_entry(void) { Fiber *f = t_fiber; f->fn(f->param); fprintf(stderr, "plat: a fiber returned\n"); ExitProcess(3); }
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID param) {
    (void)commit; (void)flags;
    Fiber *f = calloc(1, PLAT_FIBER_BLOCK);
    f->fn = fn; f->param = param;
    f->stack_size = reserve ? reserve : (SIZE_T)1 << 20;
    f->stack = mmap(NULL, f->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (f->stack == MAP_FAILED) { free(f); return NULL; }
#if defined(__APPLE__) && defined(__aarch64__)
    memset(&f->uc, 0, sizeof f->uc);
    f->uc.r[11] = (uint64_t)(uintptr_t)fiber_entry;                                     /* lr */
    f->uc.r[12] = ((uint64_t)(uintptr_t)f->stack + f->stack_size) & ~(uint64_t)15;      /* sp */
#else
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = f->stack; f->uc.uc_stack.ss_size = f->stack_size; f->uc.uc_link = NULL;
    makecontext(&f->uc, fiber_entry, 0);
#endif
    return f;
}
void SwitchToFiber(LPVOID fiber) {
    Fiber *from = t_fiber, *to = fiber;
    if (from == to) return;
    t_fiber = to;
#if defined(__APPLE__) && defined(__aarch64__)
    plat_fiber_switch(&from->uc, &to->uc);
#else
    swapcontext(&from->uc, &to->uc);
#endif
}
void DeleteFiber(LPVOID fiber) {
    Fiber *f = fiber;
    if (!f) return;
    if (f == t_fiber) { fprintf(stderr, "plat: DeleteFiber of the running fiber\n"); ExitProcess(3); }
    if (f->stack) munmap(f->stack, f->stack_size);
    free(f);
}
LPVOID GetCurrentFiber(void) { return t_fiber; }
void plat_symbolize(uintptr_t pc, char *out, size_t n) {          /* crash reports: "symbol+0xoff (file)" */
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_fname)
        snprintf(out, n, "%s+0x%lx (%s)", di.dli_sname ? di.dli_sname : "?", di.dli_saddr ? (unsigned long)(pc - (uintptr_t)di.dli_saddr) : (unsigned long)(pc - (uintptr_t)di.dli_fbase), di.dli_fname);
    else snprintf(out, n, "?");
}
int plat_current_fiber_stack(uintptr_t *lo, uintptr_t *hi) {      /* crash reports (hostrt.c) */
    Fiber *f = t_fiber;
    if (!f || f->is_thread || !f->stack) return 0;
    *lo = (uintptr_t)f->stack; *hi = (uintptr_t)f->stack + f->stack_size;
    return 1;
}
LPVOID GetFiberData(void) { return t_fiber ? t_fiber->param : NULL; }
/* nnunwind.c: the bounds of the stack this thread runs on now: the running fiber's, else the thread's own */
int plat_current_stack(uintptr_t *lo, uintptr_t *hi) {
    if (plat_current_fiber_stack(lo, hi)) return 1;
#ifdef __APPLE__
    pthread_t t = pthread_self();
    *hi = (uintptr_t)pthread_get_stackaddr_np(t);
    *lo = *hi - pthread_get_stacksize_np(t);
    return 1;
#else
    pthread_attr_t a; void *base; size_t sz;
    if (pthread_getattr_np(pthread_self(), &a)) return 0;
    pthread_attr_getstack(&a, &base, &sz);
    pthread_attr_destroy(&a);
    *lo = (uintptr_t)base; *hi = (uintptr_t)base + sz;
    return 1;
#endif
}

/* ================================================================ virtual memory
 * Windows keeps a state for every page (free / reserved / committed, protection); VirtualQuery reports runs of pages
 * with the same state. Here: an ordered list of segments for the memory that went through VirtualAlloc, and
 * /proc/self/maps for everything else (stacks, malloc, the libraries), reported as committed. */
typedef struct { uintptr_t lo, hi, alloc_base; DWORD state, prot, alloc_prot; } Seg;
static Seg *g_seg;
static int g_nseg, g_capseg;
static pthread_mutex_t g_vm_lock = PTHREAD_MUTEX_INITIALIZER;
static void vm_lock(void) { t_nosus++; pthread_mutex_lock(&g_vm_lock); }
static void vm_unlock(void) { pthread_mutex_unlock(&g_vm_lock); if (!--t_nosus && t_sus_pending) sus_deferred(); }
#define PAGE 4096u
static int prot_posix(DWORD p) {
    switch (p & 0xff) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: case PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: return PROT_EXEC;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE | PROT_EXEC;
    }
}
static int seg_find(uintptr_t a) {                  /* index of the first segment with hi > a */
    int lo = 0, hi = g_nseg;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_seg[m].hi <= a) lo = m + 1; else hi = m; }
    return lo;
}
/* Sets [lo, hi) to one state; state MEM_FREE removes it. Splits the segments it cuts. */
static void seg_set(uintptr_t lo, uintptr_t hi, uintptr_t base, DWORD state, DWORD prot, DWORD alloc_prot) {
    Seg tmp[3]; int nt = 0;
    int i = seg_find(lo), j = i;
    while (j < g_nseg && g_seg[j].lo < hi) j++;
    if (i < j && g_seg[i].lo < lo) { tmp[nt] = g_seg[i]; tmp[nt].hi = lo; nt++; }
    if (state != MEM_FREE) { Seg s = {lo, hi, base, state, prot, alloc_prot}; tmp[nt++] = s; }
    if (i < j && g_seg[j - 1].hi > hi) { tmp[nt] = g_seg[j - 1]; tmp[nt].lo = hi; nt++; }
    int n = g_nseg - (j - i) + nt;
    if (n > g_capseg) { g_capseg = n * 2 + 64; g_seg = realloc(g_seg, (size_t)g_capseg * sizeof *g_seg); }
    memmove(g_seg + i + nt, g_seg + j, (size_t)(g_nseg - j) * sizeof *g_seg);
    memcpy(g_seg + i, tmp, (size_t)nt * sizeof *tmp);
    g_nseg = n;
}
#ifdef __APPLE__
/* Host pages (MAC2): Apple Silicon maps memory in 16 KB pages; the bookkeeping above, VirtualQuery and the guest keep 4 KB
 * ones (PAGE). mprotect works on whole host pages, so each host page gets the union of the rights of its committed 4 KB
 * pieces (vm_sync). Decision: more permissive is harmless for the game; what is lost is the fault on a NOACCESS / READONLY
 * 4 KB piece that shares a host page with a writable one (a guard page smaller than 16 KB does not fault). Write + execute
 * together is refused on Apple Silicon outside MAP_JIT: such a page keeps read + write (nothing executes guest memory). */
static uintptr_t g_hpage;
static uintptr_t hpage(void) { if (!g_hpage) g_hpage = (uintptr_t)getpagesize(); return g_hpage; }
static int host_rights(uintptr_t h, uintptr_t he) {    /* g_vm_lock held */
    int r = 0;
    for (int k = seg_find(h); k < g_nseg && g_seg[k].lo < he; k++) if (g_seg[k].state == MEM_COMMIT) r |= prot_posix(g_seg[k].prot);
    if ((r & PROT_WRITE) && (r & PROT_EXEC)) r &= ~PROT_EXEC;
    return r;
}
static int ww_arm(uintptr_t h, int r);
/* Applies the segments' rights to the host pages over [lo, hi) (after seg_set); one mprotect per run of equal rights. */
static void vm_sync(uintptr_t lo, uintptr_t hi) {     /* g_vm_lock held */
    uintptr_t H = hpage(), run = 0;
    int rr = -1;
    for (uintptr_t h = lo & ~(H - 1); h < hi; h += H) {
        int r = ww_arm(h, host_rights(h, h + H));
        if (r != rr) { if (rr >= 0) mprotect((void *)run, h - run, rr); run = h; rr = r; }
    }
    if (rr >= 0) mprotect((void *)run, ((hi + H - 1) & ~(H - 1)) - run, rr);
}
#define vm_mprotect(lo, hi, prot) 0                   /* the rights are applied by vm_sync, after the bookkeeping */
#else
#define vm_sync(lo, hi) ((void)0)
#define vm_mprotect(lo, hi, prot) mprotect((void *)(lo), (hi) - (lo), prot_posix(prot))
#endif
/* Write watch (MEM_WRITE_WATCH, GetWriteWatch): userfaultfd write protection in asynchronous mode (Linux 6.7+), which the
 * kernel resolves by itself on the first write to a page and records; PAGEMAP_SCAN lists the written pages of a range
 * and protects them again in the same call. This is what Wine uses for the same function. Without it (older kernel,
 * userfaultfd not allowed) the regions are not watched and GetWriteWatch fails, as for memory that is not watched. */
#ifdef __linux__
static int g_uffd = -2, g_pagemap = -1;
typedef struct { uintptr_t lo, hi; } WwRegion;
static WwRegion g_ww[64];
static int g_nww;
static int ww_init(void) {
    if (g_uffd != -2) return g_uffd >= 0;
    g_uffd = -1;
    if (getenv("SSBU_NO_WRITEWATCH")) return 0;
    int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (fd < 0) { fprintf(stderr, "plat: no userfaultfd (%s): write watch off\n", strerror(errno)); return 0; }
    struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
    if (ioctl(fd, UFFDIO_API, &api)) { fprintf(stderr, "plat: userfaultfd without asynchronous write protection: write watch off\n"); close(fd); return 0; }
    g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (g_pagemap < 0) { close(fd); return 0; }
    g_uffd = fd;
    return 1;
}
static void ww_protect(uintptr_t lo, size_t n) {
    struct uffdio_writeprotect wp = {.range = {lo, n}, .mode = UFFDIO_WRITEPROTECT_MODE_WP};
    ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp);
}
static void ww_add(uintptr_t lo, size_t n) {                /* g_vm_lock held */
    if (g_nww == 64 || !ww_init()) return;
    struct uffdio_register reg = {.range = {lo, n}, .mode = UFFDIO_REGISTER_MODE_WP};
    if (ioctl(g_uffd, UFFDIO_REGISTER, &reg)) return;
    ww_protect(lo, n);
    g_ww[g_nww].lo = lo; g_ww[g_nww].hi = lo + n; g_nww++;
}
static int ww_find(uintptr_t a) { for (int i = 0; i < g_nww; i++) if (a >= g_ww[i].lo && a < g_ww[i].hi) return i; return -1; }
static void ww_drop(uintptr_t lo) { int i = ww_find(lo); if (i >= 0) g_ww[i] = g_ww[--g_nww]; }      /* munmap unregisters by itself */
UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addrs, ULONG_PTR *count, ULONG *gran) {
    uintptr_t lo = (uintptr_t)base & ~(uintptr_t)(PAGE - 1), hi = ((uintptr_t)base + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    vm_lock();
    int i = ww_find(lo);
    int ok = i >= 0 && hi <= g_ww[i].hi;
    vm_unlock();
    if (!ok || !size) { t_lasterr = ERROR_INVALID_PARAMETER; return 1; }
    struct page_region v[256];
    ULONG_PTR max = *count, got = 0;
    if (gran) *gran = PAGE;
    while (lo < hi && got < max) {
        struct pm_scan_arg arg;
        memset(&arg, 0, sizeof arg);
        arg.size = sizeof arg; arg.start = lo; arg.end = hi;
        arg.vec = (uintptr_t)v; arg.vec_len = 256; arg.max_pages = max - got;
        arg.category_mask = PAGE_IS_WRITTEN; arg.return_mask = PAGE_IS_WRITTEN;
        arg.flags = (flags & WRITE_WATCH_FLAG_RESET) ? PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC : 0;
        long nr = ioctl(g_pagemap, PAGEMAP_SCAN, &arg);
        if (nr < 0) { t_lasterr = ERROR_INVALID_PARAMETER; return 1; }
        for (long k = 0; k < nr; k++)
            for (uintptr_t a = v[k].start; a < v[k].end && got < max; a += PAGE) addrs[got++] = (PVOID)a;
        if (arg.walk_end <= lo) break;
        lo = arg.walk_end;                                 /* where the kernel stopped (vector full or done) */
    }
    *count = got;
    return 0;
}
UINT ResetWriteWatch(LPVOID base, SIZE_T size) {
    uintptr_t lo = (uintptr_t)base & ~(uintptr_t)(PAGE - 1), hi = ((uintptr_t)base + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    vm_lock();
    int i = ww_find(lo);
    vm_unlock();
    if (i < 0) return 1;
    ww_protect(lo, hi - lo);
    return 0;
}
void plat_prepare_write(void *p, size_t n) { (void)p; (void)n; }       /* the kernel resolves the write protection itself */
int plat_write_fault(void *addr) { (void)addr; return 0; }
#else
/* macOS (MAC2): no userfaultfd. A watched host page that has not been written since the last reset is mapped without the
 * write right; the first write faults (SIGBUS), plat_write_fault gives the right back and marks the page's 4 KB pieces
 * written; GetWriteWatch lists them and, with the reset flag, takes the right away again. Granularity: a write anywhere
 * in a 16 KB host page reports its four 4 KB pieces (more than Windows, never less: nvn_gl.c then hashes the texture).
 * The fault path takes no lock (the faulting thread may hold g_vm_lock): regions stay in their slot, rights[] and dirty[]
 * are read with atomics. The kernel does not fault for its own writes (read(), fread's direct read): those fail with
 * EFAULT, so ReadFile and fread call plat_prepare_write first. */
typedef struct { uintptr_t lo, hi; uint8_t *rights, *dirty; } WwRegion;    /* rights: the host page's own rights (vm_sync) */
static WwRegion g_ww[64];
static int g_nww;                                          /* slots in use or freed (hi = 0); never moved */
static struct sigaction g_ww_old_segv, g_ww_old_bus;
static volatile LONG64 g_ww_faults;
static int ww_find(uintptr_t a) {
    int n = __atomic_load_n(&g_nww, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        uintptr_t lo = __atomic_load_n(&g_ww[i].lo, __ATOMIC_ACQUIRE), hi = __atomic_load_n(&g_ww[i].hi, __ATOMIC_ACQUIRE);
        if (a >= lo && a < hi) return i;
    }
    return -1;
}
/* Gives a watched host page its write right back and marks its pieces written (the marks after the mprotect: see
 * ww_reset). 0 = not a watched page that has the write right. */
static int ww_unarm(uintptr_t a) {
    int i = ww_find(a);
    if (i < 0) return 0;
    uintptr_t H = g_hpage, idx = (a - g_ww[i].lo) / H;
    int r = __atomic_load_n(&g_ww[i].rights[idx], __ATOMIC_ACQUIRE);
    if (!(r & PROT_WRITE) || mprotect((void *)(a & ~(H - 1)), H, r)) return 0;
    __atomic_fetch_or(&g_ww[i].dirty[idx], 0xff, __ATOMIC_SEQ_CST);
    return 1;
}
int plat_write_fault(void *addr) {
    if (!ww_unarm((uintptr_t)addr)) return 0;
    __atomic_fetch_add(&g_ww_faults, 1, __ATOMIC_RELAXED);
    return 1;
}
void plat_prepare_write(void *p, size_t n) {
    if (!n || !__atomic_load_n(&g_nww, __ATOMIC_ACQUIRE)) return;
    uintptr_t H = hpage(), a = (uintptr_t)p, e = a + n;
    for (uintptr_t h = a & ~(H - 1); h < e; h += H) {
        int i = ww_find(h);
        if (i >= 0 && __atomic_load_n(&g_ww[i].dirty[(h - g_ww[i].lo) / H], __ATOMIC_ACQUIRE) != 0xff) ww_unarm(h);
    }
}
static void ww_signal(int sig, siginfo_t *si, void *uc) {
    if (plat_write_fault(si->si_addr)) return;
    struct sigaction *o = sig == SIGBUS ? &g_ww_old_bus : &g_ww_old_segv;
    if (o->sa_flags & SA_SIGINFO) { if (o->sa_sigaction) { o->sa_sigaction(sig, si, uc); return; } }
    else if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) { o->sa_handler(sig); return; }
    signal(sig, SIG_DFL);                                  /* nobody else: the default action when the instruction runs again */
}
/* The rights a watched host page gets: no write right (armed), even if some pieces are still marked (a write marks them
 * all again). Records the page's own rights for the fault path. g_vm_lock held. */
static int ww_arm(uintptr_t h, int r) {
    int i = ww_find(h);
    if (i < 0) return r;
    __atomic_store_n(&g_ww[i].rights[(h - g_ww[i].lo) / g_hpage], (uint8_t)r, __ATOMIC_RELEASE);
    return r & ~PROT_WRITE;
}
static void ww_add(uintptr_t lo, size_t n) {               /* g_vm_lock held; the caller runs vm_sync over the range */
    static int installed;
    if (getenv("SSBU_NO_WRITEWATCH")) return;
    int slot = -1;
    for (int i = 0; i < g_nww; i++) if (!g_ww[i].hi) { slot = i; break; }
    if (slot < 0) { if (g_nww == 64) return; slot = g_nww; }
    uintptr_t H = hpage(), nh = (n + H - 1) / H;
    uint8_t *rights = calloc(nh, 1), *dirty = calloc(nh, 1);
    if (!rights || !dirty) { free(rights); free(dirty); return; }
    if (!installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = ww_signal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGBUS, &sa, &g_ww_old_bus); sigaction(SIGSEGV, &sa, &g_ww_old_segv);
        installed = 1;
    }
    free(g_ww[slot].rights); free(g_ww[slot].dirty);
    g_ww[slot].rights = rights; g_ww[slot].dirty = dirty;
    __atomic_store_n(&g_ww[slot].lo, lo, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ww[slot].hi, lo + n, __ATOMIC_RELEASE);
    if (slot == g_nww) __atomic_store_n(&g_nww, g_nww + 1, __ATOMIC_RELEASE);
}
static void ww_drop(uintptr_t lo) {        /* the arrays stay until the slot is reused: a late fault reads valid memory */
    int i = ww_find(lo);
    if (i >= 0) __atomic_store_n(&g_ww[i].hi, 0, __ATOMIC_RELEASE);
}
/* Clears the marks of the 4 KB pieces of [lo, hi) (g_vm_lock held); with rearm the written host pages lose the write right
 * again. Order: protect, clear, protect again. A write whose fault raced the first mprotect sets its marks after its own
 * mprotect, so it is either after the clear (still marked) or caught by the second mprotect: a page is never left
 * writable with clear marks. */
static void ww_reset(int i, uintptr_t lo, uintptr_t hi, int rearm) {
    uintptr_t H = g_hpage;
    for (uintptr_t h = lo & ~(H - 1); h < hi; h += H) {
        uintptr_t idx = (h - g_ww[i].lo) / H, a = h < lo ? lo : h, e = h + H > hi ? hi : h + H;
        if (!__atomic_load_n(&g_ww[i].dirty[idx], __ATOMIC_ACQUIRE)) continue;     /* not written: already armed */
        uint8_t mask = 0;
        for (uintptr_t p = a; p < e; p += PAGE) mask |= (uint8_t)(1u << ((p - h) / PAGE));
        int r = __atomic_load_n(&g_ww[i].rights[idx], __ATOMIC_ACQUIRE);
        if (rearm && (r & PROT_WRITE)) mprotect((void *)h, H, r & ~PROT_WRITE);
        __atomic_fetch_and(&g_ww[i].dirty[idx], (uint8_t)~mask, __ATOMIC_SEQ_CST);
        if (rearm && (r & PROT_WRITE)) mprotect((void *)h, H, r & ~PROT_WRITE);
    }
}
static void ww_protect(uintptr_t lo, size_t n) {          /* decommitted pieces: not written; vm_sync arms the pages */
    int i = ww_find(lo);
    if (i >= 0) ww_reset(i, lo, lo + n, 0);
}
UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addrs, ULONG_PTR *count, ULONG *gran) {
    uintptr_t lo = (uintptr_t)base & ~(uintptr_t)(PAGE - 1), hi = ((uintptr_t)base + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    vm_lock();
    int i = ww_find(lo);
    if (i < 0 || hi > g_ww[i].hi || !size) { vm_unlock(); t_lasterr = ERROR_INVALID_PARAMETER; return 1; }
    ULONG_PTR max = *count, got = 0;
    uintptr_t H = g_hpage, p = lo;
    for (; p < hi && got < max; p += PAGE)
        if (__atomic_load_n(&g_ww[i].dirty[(p - g_ww[i].lo) / H], __ATOMIC_ACQUIRE) & (1u << ((p & (H - 1)) / PAGE))) addrs[got++] = (PVOID)p;
    if (flags & WRITE_WATCH_FLAG_RESET) ww_reset(i, lo, p, 1);           /* the pieces looked at */
    vm_unlock();
    if (gran) *gran = PAGE;
    *count = got;
    return 0;
}
UINT ResetWriteWatch(LPVOID base, SIZE_T size) {
    uintptr_t lo = (uintptr_t)base & ~(uintptr_t)(PAGE - 1), hi = ((uintptr_t)base + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    vm_lock();
    int i = ww_find(lo);
    if (i >= 0) ww_reset(i, lo, hi < g_ww[i].hi ? hi : g_ww[i].hi, 1);
    vm_unlock();
    return i < 0;
}
#endif
/* MEM_RESERVE maps the range PROT_NONE (address space only) and MEM_COMMIT is an mprotect; reservations start on 64 KB, the
 * Windows allocation granularity. Decommit (VirtualFree MEM_DECOMMIT) drops the pages, so a new commit reads zeros as on Windows. */
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot) {
    if (!size) { t_lasterr = ERROR_INVALID_PARAMETER; return NULL; }
    uintptr_t a = (uintptr_t)addr;
    vm_lock();
    if (type & MEM_RESERVE || !addr) {
        uintptr_t lo = a & ~(uintptr_t)0xffff;          /* reservations start on 64 KB, as on Windows */
        size_t n = (size + (a - lo) + PAGE - 1) & ~(size_t)(PAGE - 1);
        int commit = (type & MEM_COMMIT) != 0;
#ifdef __APPLE__                                  /* whole host pages, no rights yet (vm_sync below gives them) */
        size_t nm = (n + hpage() - 1) & ~(size_t)(hpage() - 1);
        int mprot = PROT_NONE;
#else
        size_t nm = n;
        int mprot = commit ? prot_posix(prot) : PROT_NONE;
#endif
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
        void *p;
        if (addr) p = mmap((void *)lo, nm, mprot, flags | MAP_FIXED_NOREPLACE, -1, 0);
        else {
            /* 64 KB alignment: over-allocate and trim */
            p = mmap(NULL, nm + 0x10000, mprot, flags, -1, 0);
            if (p != MAP_FAILED) {
                uintptr_t s = (uintptr_t)p, al = (s + 0xffff) & ~(uintptr_t)0xffff;
                if (al > s) munmap(p, al - s);
                if (s + nm + 0x10000 > al + nm) munmap((void *)(al + nm), s + nm + 0x10000 - (al + nm));
                p = (void *)al;
            }
        }
        if (p == MAP_FAILED || (addr && (uintptr_t)p != lo)) {
            if (p != MAP_FAILED) munmap(p, nm);
            vm_unlock();
            t_lasterr = ERROR_INVALID_ADDRESS;
            return NULL;
        }
        lo = (uintptr_t)p;
        seg_set(lo, lo + n, lo, commit ? MEM_COMMIT : MEM_RESERVE, commit ? prot : 0, prot);
        if (type & MEM_WRITE_WATCH) ww_add(lo, n);
        if (commit) vm_sync(lo, lo + n);
        vm_unlock();
        return addr ? (void *)a : p;
    }
    /* commit inside a reservation */
    uintptr_t lo = a & ~(uintptr_t)(PAGE - 1), hi = (a + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    int i = seg_find(lo);
    if (i >= g_nseg || g_seg[i].lo > lo) { vm_unlock(); t_lasterr = ERROR_INVALID_ADDRESS; return NULL; }
    uintptr_t base = g_seg[i].alloc_base; DWORD aprot = g_seg[i].alloc_prot;
    for (uintptr_t p = lo; p < hi;) {                  /* the whole range must be reserved */
        int k = seg_find(p);
        if (k >= g_nseg || g_seg[k].lo > p) { vm_unlock(); t_lasterr = ERROR_INVALID_ADDRESS; return NULL; }
        p = g_seg[k].hi;
    }
    if (vm_mprotect(lo, hi, prot)) { vm_unlock(); fail_errno(); return NULL; }
    seg_set(lo, hi, base, MEM_COMMIT, prot, aprot);
    vm_sync(lo, hi);
    vm_unlock();
    return addr;
}
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    uintptr_t a = (uintptr_t)addr;
    vm_lock();
    int i = seg_find(a);
    if (i >= g_nseg || g_seg[i].lo > a) { vm_unlock(); t_lasterr = ERROR_INVALID_ADDRESS; return FALSE; }
    if (type & MEM_RELEASE) {
        uintptr_t base = g_seg[i].alloc_base, hi = base;
        int k = seg_find(base);
        while (k < g_nseg && g_seg[k].alloc_base == base) hi = g_seg[k++].hi;
        munmap((void *)base, hi - base);
        ww_drop(base);
        seg_set(base, hi, 0, MEM_FREE, 0, 0);
    } else {
        uintptr_t lo = a & ~(uintptr_t)(PAGE - 1), hi = size ? (a + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1) : lo;
        if (!size) { uintptr_t base = g_seg[i].alloc_base; int k = seg_find(base); lo = base; while (k < g_nseg && g_seg[k].alloc_base == base) hi = g_seg[k++].hi; }
        uintptr_t base = g_seg[i].alloc_base; DWORD aprot = g_seg[i].alloc_prot;
#ifdef __APPLE__
        /* MADV_DONTNEED keeps the pages on macOS: a new mapping over the whole host pages reads zeros; the 4 KB pieces of a
         * host page shared with other pieces are cleared by hand (the page gets read + write for that, vm_sync below
         * gives it its rights again) */
        uintptr_t H = hpage(), flo = (lo + H - 1) & ~(H - 1), fhi = hi & ~(H - 1);
        if (flo < fhi) mmap((void *)flo, fhi - flo, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        else flo = fhi = hi;
        for (uintptr_t p = lo; p < hi; p += PAGE) {
            if (p >= flo && p < fhi) continue;
            int k = seg_find(p);
            if (k >= g_nseg || g_seg[k].lo > p || g_seg[k].state != MEM_COMMIT) continue;   /* never committed: zero already */
            mprotect((void *)(p & ~(H - 1)), H, PROT_READ | PROT_WRITE);
            memset((void *)p, 0, PAGE);
        }
#else
        madvise((void *)lo, hi - lo, MADV_DONTNEED);     /* decommitted pages come back zero */
        mprotect((void *)lo, hi - lo, PROT_NONE);
#endif
        if (ww_find(lo) >= 0) ww_protect(lo, hi - lo);   /* emptied pages are watched again (not "written") */
        seg_set(lo, hi, base, MEM_RESERVE, 0, aprot);
        vm_sync(lo, hi);
    }
    vm_unlock();
    return TRUE;
}
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD *old) {
    uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)(PAGE - 1), hi = ((uintptr_t)addr + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    /* a page we did not map (code of the exe or of a .so): its real protection, from the kernel's list. Answering
     * PAGE_READWRITE here made a caller that restores the old value (the hook table, after patching a function's
     * entry) take the execute right away from a code page. */
    MEMORY_BASIC_INFORMATION mi;
    DWORD untracked_old = VirtualQuery((void *)lo, &mi, sizeof mi) && mi.State == MEM_COMMIT ? mi.Protect : PAGE_READWRITE;
    vm_lock();
    int i = seg_find(lo);
    int tracked = i < g_nseg && g_seg[i].lo <= lo;
    if (old) *old = tracked ? g_seg[i].prot : untracked_old;
#ifdef __APPLE__                                  /* memory we did not map: whole host pages (vm_sync does ours) */
    uintptr_t hlo = lo & ~(hpage() - 1), hhi = (hi + hpage() - 1) & ~(hpage() - 1);
    if (!tracked && mprotect((void *)hlo, hhi - hlo, prot_posix(prot))) { vm_unlock(); return fail_errno(); }
#endif
    if (vm_mprotect(lo, hi, prot)) { vm_unlock(); return fail_errno(); }
    if (tracked) {                                   /* per segment: keeps each one's allocation base */
        for (uintptr_t p = lo; p < hi;) {
            int k = seg_find(p);
            if (k >= g_nseg || g_seg[k].lo > p) break;
            uintptr_t e = g_seg[k].hi < hi ? g_seg[k].hi : hi;
            seg_set(p, e, g_seg[k].alloc_base, MEM_COMMIT, prot, g_seg[k].alloc_prot);
            p = e;
        }
        vm_sync(lo, hi);
    }
    vm_unlock();
    return TRUE;
}
/* The rollback's learned-set save (rb_learned.c) makes hundreds of thousands of pages read-only and opens them one by one
 * from its fault handler: the segment list would be split at every page (and its lock is no place for a fault handler).
 * Those pages stay committed read-write in the bookkeeping, which is their state for everything but the first write. */
int plat_vm_tracked(const void *p) {
    uintptr_t a = (uintptr_t)p;
    vm_lock();
    int i = seg_find(a), r = i < g_nseg && g_seg[i].lo <= a;
    vm_unlock();
    return r;
}
int plat_protect_raw(void *p, size_t n, DWORD prot) {
    uintptr_t lo = (uintptr_t)p & ~(uintptr_t)(PAGE - 1), hi = ((uintptr_t)p + n + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    return mprotect((void *)lo, hi - lo, prot_posix(prot)) == 0;
}
/* AddVectoredExceptionHandler (Linux, x86-64): the handlers run from a SIGSEGV handler, newest "first" ones first; the
 * stop signal of SuspendThread waits until the handler is done. When none continues, the SIGSEGV handler installed
 * before (hostrt.c's crash report) runs as if it had been called by the kernel. */
#if defined(__linux__) && defined(__x86_64__)
typedef struct { int n; PVECTORED_EXCEPTION_HANDLER fn[]; } VehList;
static VehList *g_veh;                                     /* replaced whole when one is added (the old list is kept) */
static pthread_mutex_t g_veh_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sigaction g_veh_old;
static void veh_signal(int sig, siginfo_t *si, void *uc) {
    const VehList *l = __atomic_load_n(&g_veh, __ATOMIC_ACQUIRE);
    const greg_t *g = ((ucontext_t *)uc)->uc_mcontext.gregs;
    EXCEPTION_RECORD r; CONTEXT c;
    EXCEPTION_POINTERS e = {&r, &c};
    memset(&r, 0, sizeof r);
    r.ExceptionCode = EXCEPTION_ACCESS_VIOLATION; r.ExceptionAddress = (PVOID)(uintptr_t)g[REG_RIP]; r.NumberParameters = 2;
    r.ExceptionInformation[0] = (g[REG_ERR] & 2) ? 1 : 0; r.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
    c.ContextFlags = CONTEXT_FULL; c.EFlags = (DWORD)g[REG_EFL];
    c.Rax = g[REG_RAX]; c.Rcx = g[REG_RCX]; c.Rdx = g[REG_RDX]; c.Rbx = g[REG_RBX]; c.Rsp = g[REG_RSP]; c.Rbp = g[REG_RBP];
    c.Rsi = g[REG_RSI]; c.Rdi = g[REG_RDI]; c.R8 = g[REG_R8]; c.R9 = g[REG_R9]; c.R10 = g[REG_R10]; c.R11 = g[REG_R11];
    c.R12 = g[REG_R12]; c.R13 = g[REG_R13]; c.R14 = g[REG_R14]; c.R15 = g[REG_R15]; c.Rip = g[REG_RIP];
    for (int i = 0; l && i < l->n; i++) if (l->fn[i](&e) == EXCEPTION_CONTINUE_EXECUTION) return;
    if (g_veh_old.sa_flags & SA_SIGINFO) { if (g_veh_old.sa_sigaction) { g_veh_old.sa_sigaction(sig, si, uc); return; } }
    else if (g_veh_old.sa_handler != SIG_DFL && g_veh_old.sa_handler != SIG_IGN) { g_veh_old.sa_handler(sig); return; }
    signal(sig, SIG_DFL);                                  /* nobody else: the default action when the instruction runs again */
}
PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER fn) {
    pthread_mutex_lock(&g_veh_lock);
    VehList *o = g_veh, *l = malloc(sizeof *l + (size_t)((o ? o->n : 0) + 1) * sizeof fn);
    if (!l) { pthread_mutex_unlock(&g_veh_lock); return NULL; }
    l->n = 0;
    if (first) l->fn[l->n++] = fn;
    for (int i = 0; o && i < o->n; i++) l->fn[l->n++] = o->fn[i];
    if (!first) l->fn[l->n++] = fn;
    __atomic_store_n(&g_veh, l, __ATOMIC_RELEASE);
    struct sigaction cur;
    if (!sigaction(SIGSEGV, NULL, &cur) && cur.sa_sigaction != veh_signal) {      /* (again if someone took SIGSEGV since) */
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        pthread_once(&g_sus_once, sus_install);
        sa.sa_sigaction = veh_signal;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaddset(&sa.sa_mask, g_sus_sig);
        sigaction(SIGSEGV, &sa, &g_veh_old);
    }
    pthread_mutex_unlock(&g_veh_lock);
    return (PVOID)fn;
}
#else
PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER fn) { (void)first; (void)fn; return NULL; }
#endif
static DWORD prot_from_maps(const char *perm) {
    int r = perm[0] == 'r', w = perm[1] == 'w', x = perm[2] == 'x';
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    return w ? PAGE_READWRITE : r ? PAGE_READONLY : PAGE_NOACCESS;
}
/* Memory from VirtualAlloc answers with its allocation base, state and protection as on Windows; any other mapping (the exe,
 * libraries, malloc, stacks) is one committed MEM_PRIVATE region with its real protection (Windows: MEM_IMAGE / MEM_MAPPED). */
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mi, SIZE_T len) {
    if (len < sizeof *mi) return 0;
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(PAGE - 1);
    memset(mi, 0, sizeof *mi);
    mi->BaseAddress = (void *)a;
    vm_lock();
    int i = seg_find(a);
    if (i < g_nseg && g_seg[i].lo <= a) {
        Seg s = g_seg[i];
        uintptr_t hi = s.hi;
        for (int k = i + 1; k < g_nseg && g_seg[k].lo == hi && g_seg[k].alloc_base == s.alloc_base && g_seg[k].state == s.state && g_seg[k].prot == s.prot; k++) hi = g_seg[k].hi;
        vm_unlock();
        mi->AllocationBase = (void *)s.alloc_base; mi->AllocationProtect = s.alloc_prot;
        mi->RegionSize = hi - a; mi->State = s.state; mi->Protect = s.state == MEM_COMMIT ? s.prot : 0; mi->Type = MEM_PRIVATE;
        return sizeof *mi;
    }
    uintptr_t next_tracked = i < g_nseg ? g_seg[i].lo : UINTPTR_MAX;
    vm_unlock();
    if (a >= 0x800000000000ULL) return 0;
#ifdef PROCMAP_QUERY
    /* Linux 6.11+: the mapping that holds a (or the next one) asked of the kernel directly. Reading and parsing the text
     * below (1,400+ lines) was 4.7% of the game thread in a 2-fighter match (2026-10-15). -2 = not opened yet,
     * -1 = the kernel does not have the query: the text is read as before. */
    static int pmq_fd = -2;
    int fd = __atomic_load_n(&pmq_fd, __ATOMIC_ACQUIRE);
    if (fd == -2) {
        int nfd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC), want = -2;
        if (!__atomic_compare_exchange_n(&pmq_fd, &want, nfd < 0 ? -1 : nfd, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && nfd >= 0) close(nfd);
        fd = __atomic_load_n(&pmq_fd, __ATOMIC_ACQUIRE);
    }
    if (fd >= 0) {
        struct procmap_query q;
        memset(&q, 0, sizeof q);
        q.size = sizeof q; q.query_addr = a; q.query_flags = PROCMAP_QUERY_COVERING_OR_NEXT_VMA;
        if (ioctl(fd, PROCMAP_QUERY, &q) == 0) {
            if (q.vma_start > a) {                         /* in a gap */
                uintptr_t end = q.vma_start < next_tracked ? (uintptr_t)q.vma_start : next_tracked;
                mi->AllocationBase = NULL; mi->State = MEM_FREE; mi->Protect = PAGE_NOACCESS; mi->RegionSize = end - a;
            } else {
                char perm[4] = { q.vma_flags & PROCMAP_QUERY_VMA_READABLE ? 'r' : '-', q.vma_flags & PROCMAP_QUERY_VMA_WRITABLE ? 'w' : '-',
                                 q.vma_flags & PROCMAP_QUERY_VMA_EXECUTABLE ? 'x' : '-', 0 };
                mi->AllocationBase = (void *)(uintptr_t)q.vma_start; mi->AllocationProtect = prot_from_maps(perm);
                mi->State = MEM_COMMIT; mi->Protect = prot_from_maps(perm); mi->RegionSize = q.vma_end - a; mi->Type = MEM_PRIVATE;
            }
            return sizeof *mi;
        }
        if (errno == ENOENT) {                             /* no mapping at or above a */
            mi->State = MEM_FREE; mi->Protect = PAGE_NOACCESS; mi->RegionSize = (next_tracked != UINTPTR_MAX ? next_tracked : 0x800000000000ULL) - a;
            return sizeof *mi;
        }
        if (errno == ENOTTY || errno == EINVAL || errno == EOPNOTSUPP) __atomic_store_n(&pmq_fd, -1, __ATOMIC_RELEASE);
    }
#endif
#ifdef __APPLE__
    {   /* macOS: the region at a, or the next one, from Mach */
        mach_vm_address_t ra = a;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t bi;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj) != KERN_SUCCESS) {
            mi->State = MEM_FREE; mi->Protect = PAGE_NOACCESS; mi->RegionSize = (next_tracked != UINTPTR_MAX ? next_tracked : 0x800000000000ULL) - a;
            return sizeof *mi;
        }
        if (ra > a) {                                  /* in a gap */
            uintptr_t end = ra < next_tracked ? (uintptr_t)ra : next_tracked;
            mi->AllocationBase = NULL; mi->State = MEM_FREE; mi->Protect = PAGE_NOACCESS; mi->RegionSize = end - a;
        } else {
            char perm[4] = { bi.protection & VM_PROT_READ ? 'r' : '-', bi.protection & VM_PROT_WRITE ? 'w' : '-', bi.protection & VM_PROT_EXECUTE ? 'x' : '-', 0 };
            mi->AllocationBase = (void *)(uintptr_t)ra; mi->AllocationProtect = prot_from_maps(perm);
            mi->State = MEM_COMMIT; mi->Protect = prot_from_maps(perm); mi->RegionSize = (uintptr_t)(ra + rs) - a; mi->Type = MEM_PRIVATE;
        }
        return sizeof *mi;
    }
#endif
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t prev_end = 0x10000;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi; char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) != 3) continue;
        if (a < lo) {                                  /* in a gap */
            uintptr_t end = lo < next_tracked ? lo : next_tracked;
            mi->AllocationBase = NULL; mi->State = MEM_FREE; mi->Protect = PAGE_NOACCESS; mi->RegionSize = end - a;
            found = 1; break;
        }
        if (a < hi) {
            mi->AllocationBase = (void *)lo; mi->AllocationProtect = prot_from_maps(perm);
            mi->State = MEM_COMMIT; mi->Protect = prot_from_maps(perm); mi->RegionSize = hi - a; mi->Type = MEM_PRIVATE;
            found = 1; break;
        }
        prev_end = hi;
    }
    fclose(f);
    (void)prev_end;
    if (!found) { mi->State = MEM_FREE; mi->Protect = PAGE_NOACCESS; mi->RegionSize = (next_tracked != UINTPTR_MAX ? next_tracked : 0x800000000000ULL) - a; }
    return sizeof *mi;
}
#if defined(__APPLE__) && defined(__aarch64__)      /* arm64 does not keep code coherent with data writes */
BOOL FlushInstructionCache(HANDLE p, LPCVOID base, SIZE_T size) { (void)p; if (base && size) sys_icache_invalidate((void *)base, size); return TRUE; }
#else
BOOL FlushInstructionCache(HANDLE p, LPCVOID base, SIZE_T size) { (void)p; (void)base; (void)size; return TRUE; }     /* x86-64 keeps code coherent */
#endif
BOOL ReadProcessMemory(HANDLE p, LPCVOID src, LPVOID dst, SIZE_T n, SIZE_T *got) {
    /* a read that does not fault: process_vm_readv on ourselves */
    (void)p;
#ifdef __APPLE__                                  /* Mach's read of our own task: fails instead of faulting */
    mach_vm_size_t k = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)src, n, (mach_vm_address_t)(uintptr_t)dst, &k);
    if (got) *got = kr == KERN_SUCCESS ? (SIZE_T)k : 0;
    return kr == KERN_SUCCESS && k == n;
#else
    struct iovec l = {dst, n}, r = {(void *)src, n};
    ssize_t k = process_vm_readv(getpid(), &l, 1, &r, 1, 0);      /* glibc's wrapper: through syscall() the int flags argument
                                                                        went on the stack with its upper half unset (EINVAL at random) */
    if (got) *got = k > 0 ? (SIZE_T)k : 0;
    return k == (ssize_t)n;
#endif
}
void GetSystemInfo(SYSTEM_INFO *si) {
    memset(si, 0, sizeof *si);
#ifdef __aarch64__
    si->wProcessorArchitecture = 12;                 /* PROCESSOR_ARCHITECTURE_ARM64 */
#else
    si->wProcessorArchitecture = 9;                  /* PROCESSOR_ARCHITECTURE_AMD64 */
#endif
    si->dwPageSize = PAGE;
    si->lpMinimumApplicationAddress = (void *)0x10000;
    si->lpMaximumApplicationAddress = (void *)0x7ffffffeffffULL;
    si->dwNumberOfProcessors = (DWORD)get_nprocs();
    si->dwActiveProcessorMask = si->dwNumberOfProcessors >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << si->dwNumberOfProcessors) - 1);
    si->dwAllocationGranularity = 0x10000;
}
BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *m) {
#ifdef __APPLE__                                  /* macOS: hw.memsize, and Mach's free + inactive pages; swap grows as needed */
    uint64_t total = 0;
    size_t len = sizeof total;
    vm_statistics64_data_t vs;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (sysctlbyname("hw.memsize", &total, &len, NULL, 0) || host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &cnt) != KERN_SUCCESS) return FALSE;
    m->ullTotalPhys = total;
    m->ullAvailPhys = ((ULONGLONG)vs.free_count + vs.inactive_count) * vm_page_size;
    m->ullTotalPageFile = m->ullTotalPhys;
    m->ullAvailPageFile = m->ullAvailPhys;
#else
    struct sysinfo s;
    if (sysinfo(&s)) return FALSE;
    m->ullTotalPhys = (ULONGLONG)s.totalram * s.mem_unit;
    m->ullAvailPhys = (ULONGLONG)(s.freeram + s.bufferram) * s.mem_unit;
    {   /* the kernel's own estimate (page cache that can be given back included): what the rollback's commit check needs */
        FILE *f = fopen("/proc/meminfo", "r");
        char line[128]; unsigned long long kb;
        while (f && fgets(line, sizeof line, f)) if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) { m->ullAvailPhys = kb * 1024; break; }
        if (f) fclose(f);
    }
    m->ullTotalPageFile = m->ullTotalPhys + (ULONGLONG)s.totalswap * s.mem_unit;
    m->ullAvailPageFile = m->ullAvailPhys + (ULONGLONG)s.freeswap * s.mem_unit;
#endif
    m->ullTotalVirtual = m->ullAvailVirtual = 0x7ffffffeffffULL;
    m->dwMemoryLoad = (DWORD)(m->ullTotalPhys ? 100 - m->ullAvailPhys * 100 / m->ullTotalPhys : 0);
    return TRUE;
}
BOOL GetProcessMemoryInfo(HANDLE p, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb) {
    (void)p;
    memset(pmc, 0, cb < sizeof *pmc ? cb : sizeof *pmc);
#ifdef __APPLE__                                  /* the resident size from Mach, counted in PAGE as statm counts it; ru_maxrss is in bytes here */
    unsigned long long res = 0;
    mach_task_basic_info_data_t ti;
    mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&ti, &cnt) == KERN_SUCCESS) res = ti.resident_size / PAGE;
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    ru.ru_maxrss /= 1024;
#else
    unsigned long long size = 0, res = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%llu %llu", &size, &res) != 2) size = res = 0; fclose(f); }
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
#endif
    pmc->cb = cb;
    pmc->WorkingSetSize = (SIZE_T)(res * PAGE);
    pmc->PeakWorkingSetSize = (SIZE_T)ru.ru_maxrss * 1024;
    pmc->PagefileUsage = pmc->PrivateUsage = (SIZE_T)(res * PAGE);
    pmc->PeakPagefileUsage = pmc->PeakWorkingSetSize;
    pmc->PageFaultCount = (DWORD)(ru.ru_minflt + ru.ru_majflt);
    return TRUE;
}
HANDLE CreateFileMappingA(HANDLE f, SECURITY_ATTRIBUTES *sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name) {
    (void)sa; (void)prot; (void)name;
    if (!is_obj(f)) return NULL;
    PlatObj *o = obj_new(PO_MAPPING);
    o->fd = dup(((PlatObj *)f)->fd);
    o->map_size = ((size_t)hi << 32) | lo;
    if (!o->map_size) { struct stat st; fstat(o->fd, &st); o->map_size = (size_t)st.st_size; }
    return o;
}
typedef struct { void *p; size_t n; } View;
static View g_views[64];
LPVOID MapViewOfFile(HANDLE m, DWORD access, DWORD hi, DWORD lo, SIZE_T n) {
    PlatObj *o = m;
    off_t off = (off_t)(((uint64_t)hi << 32) | lo);
    if (!n) n = o->map_size - (size_t)off;
    void *p = mmap(NULL, n, (access & FILE_MAP_WRITE) ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, o->fd, off);
    if (p == MAP_FAILED) { fail_errno(); return NULL; }
    for (int i = 0; i < 64; i++) if (!g_views[i].p) { g_views[i].p = p; g_views[i].n = n; break; }
    return p;
}
BOOL UnmapViewOfFile(LPCVOID p) {
    for (int i = 0; i < 64; i++) if (g_views[i].p == p) { munmap(g_views[i].p, g_views[i].n); g_views[i].p = NULL; return TRUE; }
    return FALSE;
}

/* ================================================================ C runtime */
int __argc;
char **__argv;
__attribute__((constructor)) static void plat_args(int argc, char **argv) { __argc = argc; __argv = argv; }   /* glibc passes main's arguments to constructors */
int _putenv(const char *s) {
    const char *eq = strchr(s, '=');
    if (!eq) return -1;
    char name[256];
    size_t n = (size_t)(eq - s) < sizeof name - 1 ? (size_t)(eq - s) : sizeof name - 1;
    memcpy(name, s, n); name[n] = 0;
    return eq[1] ? setenv(name, eq + 1, 1) : unsetenv(name);
}
char *_strlwr(char *s) { for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32; return s; }
char *_strupr(char *s) { for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; return s; }
/* aligned blocks keep the raw pointer and size in front of the returned one */
typedef struct { void *raw; size_t size; } AlHdr;
void *_aligned_malloc(size_t size, size_t align) {
    if (align < sizeof(void *)) align = sizeof(void *);
    uint8_t *raw = malloc(size + align + sizeof(AlHdr));
    if (!raw) return NULL;
    uintptr_t p = ((uintptr_t)raw + sizeof(AlHdr) + align - 1) & ~(uintptr_t)(align - 1);
    AlHdr *h = (AlHdr *)p - 1;
    h->raw = raw; h->size = size;
    return (void *)p;
}
void _aligned_free(void *p) { if (p) free(((AlHdr *)p - 1)->raw); }
size_t _aligned_msize(void *p, size_t align, size_t off) { (void)align; (void)off; return ((AlHdr *)p - 1)->size; }
void *_aligned_realloc(void *p, size_t size, size_t align) {
    if (!p) return _aligned_malloc(size, align);
    if (!size) { _aligned_free(p); return NULL; }
    void *q = _aligned_malloc(size, align);
    if (!q) return NULL;
    size_t old = ((AlHdr *)p - 1)->size;
    memcpy(q, p, old < size ? old : size);
    _aligned_free(p);
    return q;
}
/* Paths: '\\' becomes '/'. File names are case-sensitive here, unlike Windows (FindFirstFile patterns still match either case). */
const char *plat_path(const char *in, char *out, size_t n) {
    size_t i = 0;
    for (; in[i] && i + 1 < n; i++) out[i] = in[i] == '\\' ? '/' : in[i];
    out[i] = 0;
    return out;
}
#define P(in, buf) plat_path((in), (buf), sizeof (buf))
FILE *plat_fopen(const char *path, const char *mode) { char b[PATH_MAX]; return fopen(P(path, b), mode); }
int plat_remove(const char *path) { char b[PATH_MAX]; return remove(P(path, b)); }
int plat_rename(const char *a, const char *b) { char x[PATH_MAX], y[PATH_MAX]; return rename(P(a, x), P(b, y)); }
char *_fullpath(char *out, const char *path, size_t n) {
    char b[PATH_MAX], r[PATH_MAX];
    P(path, b);
    if (!realpath(b, r)) {                            /* the file need not exist: make it absolute without resolving */
        if (b[0] == '/') snprintf(r, sizeof r, "%s", b);
        else { char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0; snprintf(r, sizeof r, "%s/%s", cwd, b); }
    }
    if (!out) return strdup(r);
    snprintf(out, n, "%s", r);
    return out;
}
int _mkdir(const char *path) { char b[PATH_MAX]; return mkdir(P(path, b), 0777); }

/* ================================================================ files */
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)tmpl;               /* no share modes: another open of the file is never refused, as Windows may */
    char b[PATH_MAX];
    int of = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case CREATE_NEW: of |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: of |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: of |= O_CREAT; break;
    case TRUNCATE_EXISTING: of |= O_TRUNC; break;
    default: break;
    }
    int fd = open(P(path, b), of | O_CLOEXEC, 0666);
    if (fd < 0 && errno == EISDIR && !(access & GENERIC_WRITE)) fd = open(b, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { fail_errno(); return INVALID_HANDLE_VALUE; }
#ifdef __APPLE__
    if (flags & FILE_FLAG_SEQUENTIAL_SCAN) fcntl(fd, F_RDAHEAD, 1);
#else
    if (flags & FILE_FLAG_SEQUENTIAL_SCAN) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    PlatObj *o = obj_new(PO_FILE);
    o->fd = fd;
    t_lasterr = 0;
    return o;
}
int plat_handle_fd(HANDLE h) { if (is_tagged(h, FD_TAG)) return (int)((uintptr_t)h & 0xffffffff); return is_obj(h) ? ((PlatObj *)h)->fd : -1; }
intptr_t _get_osfhandle(int fd) { return fd < 0 ? -1 : (intptr_t)(FD_TAG + (uint64_t)fd); }
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *got, OVERLAPPED *ov) {
    int fd = plat_handle_fd(h);
    size_t done = 0;
    plat_prepare_write(buf, n);                      /* write-watched memory (macOS): the kernel's write would fail */
    while (done < n) {
        ssize_t k = ov ? pread(fd, (char *)buf + done, n - done, (off_t)(((uint64_t)ov->OffsetHigh << 32 | ov->Offset) + done))
                       : read(fd, (char *)buf + done, n - done);
        if (k < 0) { if (errno == EINTR) continue; if (got) *got = (DWORD)done; return fail_errno(); }
        if (!k) break;
        done += (size_t)k;
    }
    if (got) *got = (DWORD)done;
    return TRUE;
}
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *put, OVERLAPPED *ov) {
    int fd = plat_handle_fd(h);
    size_t done = 0;
    while (done < n) {
        ssize_t k = ov ? pwrite(fd, (const char *)buf + done, n - done, (off_t)(((uint64_t)ov->OffsetHigh << 32 | ov->Offset) + done))
                       : write(fd, (const char *)buf + done, n - done);
        if (k < 0) { if (errno == EINTR) continue; if (put) *put = (DWORD)done; return fail_errno(); }
        done += (size_t)k;
    }
    if (put) *put = (DWORD)done;
    return TRUE;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *pos, DWORD how) {
    off_t r = lseek(plat_handle_fd(h), (off_t)dist.QuadPart, how == FILE_BEGIN ? SEEK_SET : how == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    if (r < 0) return fail_errno();
    if (pos) pos->QuadPart = r;
    return TRUE;
}
DWORD SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD how) {
    LARGE_INTEGER d, p;
    d.QuadPart = hi ? ((LONGLONG)*hi << 32) | (DWORD)lo : lo;
    if (!SetFilePointerEx(h, d, &p, how)) return INVALID_FILE_SIZE;
    if (hi) *hi = p.HighPart;
    return p.LowPart;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) { struct stat st; if (fstat(plat_handle_fd(h), &st)) return fail_errno(); size->QuadPart = st.st_size; return TRUE; }
DWORD GetFileSize(HANDLE h, DWORD *hi) { LARGE_INTEGER s; if (!GetFileSizeEx(h, &s)) return INVALID_FILE_SIZE; if (hi) *hi = (DWORD)s.HighPart; return s.LowPart; }
BOOL SetEndOfFile(HANDLE h) { int fd = plat_handle_fd(h); off_t p = lseek(fd, 0, SEEK_CUR); return p >= 0 && !ftruncate(fd, p) ? TRUE : fail_errno(); }
BOOL FlushFileBuffers(HANDLE h) { return fsync(plat_handle_fd(h)) ? fail_errno() : TRUE; }
static DWORD attrs_of(const struct stat *st, const char *name) {
    DWORD a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    if (name && name[0] == '.' && name[1] && strcmp(name, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
    return a;
}
DWORD GetFileAttributesA(LPCSTR path) { char b[PATH_MAX]; struct stat st; if (stat(P(path, b), &st)) { fail_errno(); return INVALID_FILE_ATTRIBUTES; } return attrs_of(&st, NULL); }
static void st_times(const struct stat *st, FILETIME *c, FILETIME *a, FILETIME *w) {
    ft_set(c, ts_to_ft(&st->st_ctim)); ft_set(a, ts_to_ft(&st->st_atim)); ft_set(w, ts_to_ft(&st->st_mtim));
}
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID out) {
    (void)lvl;
    char b[PATH_MAX]; struct stat st;
    if (stat(P(path, b), &st)) return fail_errno();
    WIN32_FILE_ATTRIBUTE_DATA *d = out;
    d->dwFileAttributes = attrs_of(&st, NULL);
    st_times(&st, &d->ftCreationTime, &d->ftLastAccessTime, &d->ftLastWriteTime);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}
BOOL SetFileAttributesA(LPCSTR path, DWORD attr) { (void)path; (void)attr; return TRUE; }      /* no attributes kept; a file's own mode never stops its deletion here (nnfs.c clears read-only for that) */
BOOL CreateDirectoryA(LPCSTR path, SECURITY_ATTRIBUTES *sa) { (void)sa; char b[PATH_MAX]; return mkdir(P(path, b), 0777) ? fail_errno() : TRUE; }
BOOL RemoveDirectoryA(LPCSTR path) { char b[PATH_MAX]; return rmdir(P(path, b)) ? fail_errno() : TRUE; }
BOOL DeleteFileA(LPCSTR path) { char b[PATH_MAX]; return unlink(P(path, b)) ? fail_errno() : TRUE; }
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {      /* rename() replaces the target in one step, as MOVEFILE_REPLACE_EXISTING does */
    char a[PATH_MAX], b[PATH_MAX];
    P(from, a); P(to, b);
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && !access(b, F_OK)) { t_lasterr = ERROR_ALREADY_EXISTS; return FALSE; }
    if (rename(a, b)) return fail_errno();
    if (flags & MOVEFILE_WRITE_THROUGH) {                /* the file and the folder entry on disk before returning */
        int fd = open(b, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { fsync(fd); close(fd); }
        char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", b);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; fd = open(sl == dir ? "/" : dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC); }
        else fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) { fsync(fd); close(fd); }
    }
    return TRUE;
}
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    char a[PATH_MAX], b[PATH_MAX];
    int in = open(P(from, a), O_RDONLY | O_CLOEXEC);
    if (in < 0) return fail_errno();
    int out = open(P(to, b), O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0666);
    if (out < 0) { close(in); return fail_errno(); }
    char buf[1 << 16]; ssize_t k; BOOL ok = TRUE;
    while ((k = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)k) != k) { ok = fail_errno(); break; }
    close(in); close(out);
    return ok;
}
DWORD GetTempPathA(DWORD n, LPSTR out) { const char *t = getenv("TMPDIR"); int k = snprintf(out, n, "%s/", t && t[0] ? t : "/tmp"); return (DWORD)k; }
BOOL GetDiskFreeSpaceExA(LPCSTR path, ULARGE_INTEGER *avail, ULARGE_INTEGER *total, ULARGE_INTEGER *free_) {
    char b[PATH_MAX]; struct statvfs s;
    if (statvfs(path ? P(path, b) : ".", &s)) return fail_errno();
    if (avail) avail->QuadPart = (ULONGLONG)s.f_bavail * s.f_frsize;
    if (total) total->QuadPart = (ULONGLONG)s.f_blocks * s.f_frsize;
    if (free_) free_->QuadPart = (ULONGLONG)s.f_bfree * s.f_frsize;
    return TRUE;
}
DWORD GetCurrentDirectoryA(DWORD n, LPSTR out) { char b[PATH_MAX]; if (!getcwd(b, sizeof b)) return 0; DWORD k = (DWORD)strlen(b); if (k + 1 > n) return k + 1; memcpy(out, b, k + 1); return k; }
BOOL SetCurrentDirectoryA(LPCSTR path) { char b[PATH_MAX]; return chdir(P(path, b)) ? fail_errno() : TRUE; }
DWORD GetFullPathNameA(LPCSTR path, DWORD n, LPSTR out, LPSTR *file) {
    char r[PATH_MAX];
    _fullpath(r, path, sizeof r);
    DWORD k = (DWORD)strlen(r);
    if (k + 1 > n) return k + 1;
    memcpy(out, r, k + 1);
    if (file) { char *s = strrchr(out, '/'); *file = s ? s + 1 : out; }
    return k;
}
static BOOL find_next(PlatObj *o, WIN32_FIND_DATAA *fd) {
    struct dirent *e;
    while ((e = readdir(o->dir))) {
        if (fnmatch(o->pattern, e->d_name, FNM_CASEFOLD)) continue;
        char full[PATH_MAX]; struct stat st;
        snprintf(full, sizeof full, "%s/%s", o->dir_path, e->d_name);
        memset(fd, 0, sizeof *fd);
        if (!stat(full, &st)) {
            fd->dwFileAttributes = attrs_of(&st, e->d_name);
            st_times(&st, &fd->ftCreationTime, &fd->ftLastAccessTime, &fd->ftLastWriteTime);
            fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); fd->nFileSizeLow = (DWORD)st.st_size;
        } else fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        return TRUE;
    }
    t_lasterr = ERROR_NO_MORE_FILES;
    return FALSE;
}
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd) {
    char b[PATH_MAX];
    P(pattern, b);
    char *slash = strrchr(b, '/');
    PlatObj *o = obj_new(PO_FIND);
    if (slash) { *slash = 0; snprintf(o->dir_path, sizeof o->dir_path, "%s", b[0] ? b : "/"); snprintf(o->pattern, sizeof o->pattern, "%s", slash + 1); }
    else { snprintf(o->dir_path, sizeof o->dir_path, "."); snprintf(o->pattern, sizeof o->pattern, "%s", b); }
    if (!strcmp(o->pattern, "*.*")) snprintf(o->pattern, sizeof o->pattern, "*");
    o->dir = opendir(o->dir_path);
    if (!o->dir) { fail_errno(); if (t_lasterr == ERROR_FILE_NOT_FOUND) t_lasterr = ERROR_PATH_NOT_FOUND; obj_release(o); return INVALID_HANDLE_VALUE; }
    if (!find_next(o, fd)) { obj_release(o); t_lasterr = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
    return o;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) { return find_next(h, fd); }
BOOL FindClose(HANDLE h) { if (!is_obj(h)) return FALSE; obj_release(h); return TRUE; }

/* ================================================================ modules */
/* bcrypt.dll: nnro.c asks it for SHA-256; this is the one Windows DLL with no Linux library behind it. */
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; } Sha256;
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha256(const uint8_t *p, size_t n, uint8_t out[32]) {
    Sha256 s = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha_block(&s, p + i);
    uint8_t t[128] = {0};
    size_t r = n - i;
    memcpy(t, p + i, r);
    t[r] = 0x80;
    size_t tl = r + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) t[tl - 1 - k] = (uint8_t)(bits >> (k * 8));
    sha_block(&s, t);
    if (tl == 128) sha_block(&s, t + 64);
    for (int k = 0; k < 8; k++) { out[k * 4] = (uint8_t)(s.h[k] >> 24); out[k * 4 + 1] = (uint8_t)(s.h[k] >> 16); out[k * 4 + 2] = (uint8_t)(s.h[k] >> 8); out[k * 4 + 3] = (uint8_t)s.h[k]; }
}
static LONG bc_open(void **alg, LPCWSTR id, LPCWSTR impl, ULONG flags) { (void)id; (void)impl; (void)flags; *alg = (void *)1; return 0; }
static LONG bc_hash(void *alg, uint8_t *secret, ULONG ns, const uint8_t *in, ULONG n, uint8_t *out, ULONG nout) {
    (void)alg; (void)secret; (void)ns;
    if (nout < 32) return (LONG)0xC0000023;
    sha256(in, n, out);
    return 0;
}
static LONG bc_close(void *alg, ULONG flags) { (void)alg; (void)flags; return 0; }
static char g_bcrypt_tag;
#define BCRYPT_MODULE ((HMODULE)&g_bcrypt_tag)

static const char *exe_dir(void) {
    static char dir[PATH_MAX];
    if (!dir[0]) { ssize_t k = self_exe_path(dir, sizeof dir - 1); if (k > 0) { dir[k] = 0; char *s = strrchr(dir, '/'); if (s) *s = 0; } else strcpy(dir, "."); }
    return dir;
}
static void *sys_dlopen(const char *name) {        /* a system library by its plain name */
    void *h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
#ifdef __APPLE__                                  /* dyld does not search Homebrew's folders by itself */
    static const char *const dirs[] = {"/opt/homebrew/lib/", "/usr/local/lib/"};
    char p[PATH_MAX];
    for (int i = 0; !h && name[0] != '/' && i < 2; i++) { snprintf(p, sizeof p, "%s%s", dirs[i], name); h = dlopen(p, RTLD_NOW | RTLD_LOCAL); }
#endif
    return h;
}
HMODULE LoadLibraryA(LPCSTR name) {
    static const struct { const char *win, *linux_; } map[] = {
#ifdef __APPLE__                                  /* Homebrew's libraries; the Vulkan loader finds MoltenVK through its driver file */
        {"vulkan-1.dll", "libvulkan.1.dylib"}, {"libopus-0.dll", "libopus.0.dylib"}, {"opus.dll", "libopus.0.dylib"},
        {"SDL3.dll", "libSDL3.0.dylib"}, {"opengl32.dll", "/System/Library/Frameworks/OpenGL.framework/OpenGL"},
#else
        {"vulkan-1.dll", "libvulkan.so.1"}, {"libopus-0.dll", "libopus.so.0"}, {"opus.dll", "libopus.so.0"},
        {"SDL3.dll", "libSDL3.so.0"}, {"opengl32.dll", "libGL.so.1"},
#endif
    };
    char b[PATH_MAX], c[PATH_MAX];
    P(name, b);
    const char *base = strrchr(b, '/') ? strrchr(b, '/') + 1 : b;
    if (!strcasecmp(base, "bcrypt.dll")) return BCRYPT_MODULE;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcasecmp(base, map[i].win)) { void *h = sys_dlopen(map[i].linux_); if (!h) t_lasterr = ERROR_FILE_NOT_FOUND; return h; }
    size_t n = strlen(b);
    if (n > 4 && !strcasecmp(b + n - 4, ".dll")) strcpy(b + n - 4, PLAT_SO_EXT);     /* our own modules: x.dll -> x.so (.dylib) */
    /* Windows looks next to the exe first; dlopen only does that for a path */
    if (b[0] != '/') snprintf(c, sizeof c, "%s/%s", exe_dir(), b); else snprintf(c, sizeof c, "%s", b);
    void *h = dlopen(c, RTLD_NOW | RTLD_LOCAL);
    if (!h && b[0] != '/') h = dlopen(b, RTLD_NOW | RTLD_LOCAL);
    if (!h) { t_lasterr = ERROR_FILE_NOT_FOUND; if (getenv("SSBU_PLAT_DLERROR")) fprintf(stderr, "plat: dlopen %s: %s\n", c, dlerror()); }
    return h;
}
FARPROC GetProcAddress(HMODULE m, LPCSTR name) {
    if (m == BCRYPT_MODULE) {
        if (!strcmp(name, "BCryptOpenAlgorithmProvider")) return (FARPROC)bc_open;
        if (!strcmp(name, "BCryptHash")) return (FARPROC)bc_hash;
        if (!strcmp(name, "BCryptCloseAlgorithmProvider")) return (FARPROC)bc_close;
        return NULL;
    }
    return dlsym(m ? m : RTLD_DEFAULT, name);
}
BOOL FreeLibrary(HMODULE m) { if (m == BCRYPT_MODULE) return TRUE; return dlclose(m) == 0; }
HMODULE GetModuleHandleA(LPCSTR name) {
    if (!name) return dlopen(NULL, RTLD_NOW);
    char b[PATH_MAX];
    P(name, b);
    size_t n = strlen(b);
    if (n > 4 && !strcasecmp(b + n - 4, ".dll")) strcpy(b + n - 4, PLAT_SO_EXT);
    return dlopen(b, RTLD_NOW | RTLD_NOLOAD);
}
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out) {
    *out = NULL;
    if (!(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)) { *out = GetModuleHandleA(name); return *out != NULL; }
    Dl_info di;
    if (!dladdr((const void *)name, &di) || !di.dli_fname) return FALSE;
    void *self = dlopen(NULL, RTLD_NOW);
    Dl_info me;
    if (dladdr((void *)&GetModuleHandleExA, &me) && me.dli_fbase == di.dli_fbase) { *out = self; return TRUE; }
    *out = dlopen(di.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    return *out != NULL;
}
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n) {
    char b[PATH_MAX] = "";
    if (!m || m == dlopen(NULL, RTLD_NOW)) { ssize_t k = self_exe_path(b, sizeof b - 1); if (k < 0) k = 0; b[k] = 0; }
#ifdef __APPLE__
    else { const char *path; uintptr_t base; if (dyld_image_of(m, &path, &base)) snprintf(b, sizeof b, "%s", path); }
#else
    else { struct link_map *lm = NULL; if (!dlinfo(m, RTLD_DI_LINKMAP, &lm) && lm) snprintf(b, sizeof b, "%s", lm->l_name); }
#endif
    for (char *p = b; *p; p++) if (*p == '/') *p = '\\';      /* Windows form: callers cut at the last '\\'; every file function here turns it back */
    DWORD k = (DWORD)snprintf(out, n, "%s", b);
    return k < n ? k : n;
}
uintptr_t plat_module_base(HMODULE m) {
    Dl_info di;
    if (!m || m == dlopen(NULL, RTLD_NOW)) { if (dladdr((void *)&plat_module_base, &di)) return (uintptr_t)di.dli_fbase; return 0; }
#ifdef __APPLE__
    const char *path; uintptr_t base;
    return dyld_image_of(m, &path, &base) ? base : 0;
#else
    struct link_map *lm = NULL;
    if (!dlinfo(m, RTLD_DI_LINKMAP, &lm) && lm) return (uintptr_t)lm->l_addr;
    return 0;
#endif
}

/* ================================================================ text: UTF-8 <-> wchar_t (UTF-32 here)
 * wchar_t is 32 bits on Linux (16 on Windows: UTF-16), so a character is always one wchar_t; the code page is always UTF-8. */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR w, int wn) {
    (void)cp; (void)flags;
    const uint8_t *p = (const uint8_t *)s, *e = n < 0 ? NULL : p + n;
    int k = 0;
    for (;;) {
        if (e ? p >= e : 0) break;
        uint32_t c = *p;
        int len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (len > 1) { c &= 0x3f >> (len - 1); for (int i = 1; i < len && (!e || p + i < e); i++) c = c << 6 | (p[i] & 0x3f); }
        p += len;
        if (w && wn) { if (k >= wn) return 0; w[k] = (wchar_t)c; }
        k++;
        if (!e && c == 0) break;
    }
    return k;
}
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int wn, LPSTR s, int n, LPCSTR def, LPBOOL used) {
    (void)cp; (void)flags; (void)def;
    if (used) *used = FALSE;
    int k = 0;
    for (int i = 0; wn < 0 || i < wn; i++) {
        uint32_t c = (uint32_t)w[i];
        uint8_t t[4]; int len;
        if (c < 0x80) { t[0] = (uint8_t)c; len = 1; }
        else if (c < 0x800) { t[0] = (uint8_t)(0xc0 | c >> 6); t[1] = (uint8_t)(0x80 | (c & 0x3f)); len = 2; }
        else if (c < 0x10000) { t[0] = (uint8_t)(0xe0 | c >> 12); t[1] = (uint8_t)(0x80 | (c >> 6 & 0x3f)); t[2] = (uint8_t)(0x80 | (c & 0x3f)); len = 3; }
        else { t[0] = (uint8_t)(0xf0 | c >> 18); t[1] = (uint8_t)(0x80 | (c >> 12 & 0x3f)); t[2] = (uint8_t)(0x80 | (c >> 6 & 0x3f)); t[3] = (uint8_t)(0x80 | (c & 0x3f)); len = 4; }
        if (s && n) { if (k + len > n) return 0; memcpy(s + k, t, (size_t)len); }
        k += len;
        if (wn < 0 && c == 0) break;
    }
    return k;
}

/* ================================================================ keyboard, focus, message boxes (SDL) */
static SDL_Scancode vk_scancode(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk < VK_F1 + 12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD0 + 1 && vk <= VK_NUMPAD0 + 9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD0 - 1));
    switch (vk) {
    case VK_NUMPAD0: return SDL_SCANCODE_KP_0;
    case VK_BACK: return SDL_SCANCODE_BACKSPACE;
    case VK_TAB: return SDL_SCANCODE_TAB;
    case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_SPACE: return SDL_SCANCODE_SPACE;
    case VK_PRIOR: return SDL_SCANCODE_PAGEUP;
    case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;
    case VK_END: return SDL_SCANCODE_END;
    case VK_HOME: return SDL_SCANCODE_HOME;
    case VK_LEFT: return SDL_SCANCODE_LEFT;
    case VK_UP: return SDL_SCANCODE_UP;
    case VK_RIGHT: return SDL_SCANCODE_RIGHT;
    case VK_DOWN: return SDL_SCANCODE_DOWN;
    case VK_INSERT: return SDL_SCANCODE_INSERT;
    case VK_DELETE: return SDL_SCANCODE_DELETE;
    case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
    case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
    case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
    case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
    case 0xBA: return SDL_SCANCODE_SEMICOLON;
    case 0xBC: return SDL_SCANCODE_COMMA;
    case 0xBE: return SDL_SCANCODE_PERIOD;
    case 0xBF: return SDL_SCANCODE_SLASH;
    case 0xC0: return SDL_SCANCODE_GRAVE;
    case 0xDB: return SDL_SCANCODE_LEFTBRACKET;
    case 0xDC: return SDL_SCANCODE_BACKSLASH;
    case 0xDD: return SDL_SCANCODE_RIGHTBRACKET;
    case 0xDE: return SDL_SCANCODE_APOSTROPHE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}
SHORT GetAsyncKeyState(int vk) {
    /* SDL's keyboard state is updated by the window thread's event loop (display.c) */
    int n = 0;
    const bool *ks = SDL_GetKeyboardState(&n);
    if (!ks) return 0;
    int down;
    if (vk == VK_SHIFT) down = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
    else if (vk == VK_CONTROL) down = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL];
    else if (vk == VK_MENU) down = ks[SDL_SCANCODE_LALT] || ks[SDL_SCANCODE_RALT];
    else { SDL_Scancode sc = vk_scancode(vk); down = sc != SDL_SCANCODE_UNKNOWN && (int)sc < n && ks[sc]; }
    return down ? (SHORT)0x8000 : 0;
}
HWND plat_game_window;                               /* display.c: the SDL_Window */
HWND GetForegroundWindow(void) { return plat_game_window && SDL_GetKeyboardFocus() == (SDL_Window *)plat_game_window ? plat_game_window : NULL; }
int MessageBoxA(HWND w, LPCSTR text, LPCSTR title, UINT type) {
    (void)w;
    fprintf(stderr, "[%s] %s\n", title ? title : "", text ? text : "");
    if ((type & 0xf) == MB_YESNO) {
        const SDL_MessageBoxButtonData b[2] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, IDYES, "Yes"}, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDNO, "No"}};
        const SDL_MessageBoxData d = {SDL_MESSAGEBOX_WARNING, NULL, title, text, 2, b, NULL};
        int id = IDNO;
        if (!SDL_ShowMessageBox(&d, &id)) return IDNO;
        return id;
    }
    SDL_ShowSimpleMessageBox((type & 0xf0) == MB_ICONERROR ? SDL_MESSAGEBOX_ERROR : (type & 0xf0) == MB_ICONWARNING ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION, title, text, NULL);
    return IDOK;
}
int MessageBoxW(HWND w, LPCWSTR text, LPCWSTR title, UINT type) {
    char t[2048], h[256];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, t, sizeof t, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, h, sizeof h, NULL, NULL);
    return MessageBoxA(w, t, h, type);
}

/* ================================================================ odds and ends */
DWORD GetCurrentProcessorNumber(void) { int c = sched_getcpu(); return c < 0 ? 0 : (DWORD)c; }
BOOLEAN SystemFunction036(PVOID buf, ULONG n) {
    for (ULONG done = 0; done < n;) { ssize_t k = getrandom((char *)buf + done, n - done, 0); if (k < 0) { if (errno == EINTR) continue; return FALSE; } done += (ULONG)k; }
    return TRUE;
}
BOOL QueryThreadCycleTime(HANDLE h, ULONG64 *cycles) {
#ifdef __APPLE__                                  /* the thread's CPU time from Mach (no pthread_getcpuclockid on macOS) */
    pthread_t th = is_obj(h) && ((PlatObj *)h)->kind == PO_THREAD ? ((PlatObj *)h)->th : pthread_self();
    thread_basic_info_data_t bi;
    mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(th), THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) != KERN_SUCCESS) return FALSE;
    *cycles = ((ULONG64)bi.user_time.seconds + (ULONG64)bi.system_time.seconds) * 1000000000ULL
            + ((ULONG64)bi.user_time.microseconds + (ULONG64)bi.system_time.microseconds) * 1000ULL;
    return TRUE;
#else
    clockid_t id = CLOCK_THREAD_CPUTIME_ID;
    if (is_obj(h) && ((PlatObj *)h)->kind == PO_THREAD && pthread_getcpuclockid(((PlatObj *)h)->th, &id)) return FALSE;
    struct timespec ts;
    if (clock_gettime(id, &ts)) return FALSE;
    *cycles = (ULONG64)ts.tv_sec * 1000000000ULL + (ULONG64)ts.tv_nsec;
    return TRUE;
#endif
}
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    (void)access; (void)inherit;
    if (kill((pid_t)pid, 0) && errno == ESRCH) { t_lasterr = ERROR_INVALID_PARAMETER; return NULL; }
    return (HANDLE)(uintptr_t)(PID_TAG + pid);
}
BOOL GetExitCodeProcess(HANDLE p, DWORD *code) {
    if (!is_tagged(p, PID_TAG)) return FALSE;
    *code = kill((pid_t)((uintptr_t)p & 0xffffffff), 0) && errno == ESRCH ? 0 : STILL_ACTIVE;
    return TRUE;
}
static const char *user_name(void) {
    const char *u = getenv("USER");
    if (!u || !u[0]) { struct passwd *pw = getpwuid(getuid()); u = pw ? pw->pw_name : ""; }
    return u;
}
BOOL GetUserNameA(LPSTR out, DWORD *n) { const char *u = user_name(); DWORD k = (DWORD)strlen(u) + 1; if (k > *n) { *n = k; return FALSE; } memcpy(out, u, k); *n = k; return TRUE; }
BOOL GetUserNameW(LPWSTR out, DWORD *n) { int k = MultiByteToWideChar(CP_UTF8, 0, user_name(), -1, out, (int)*n); if (!k) return FALSE; *n = (DWORD)k; return TRUE; }
int GetUserDefaultGeoName(LPWSTR out, int n) {
    const char *l = getenv("LC_ALL");
    if (!l || !l[0]) l = getenv("LANG");
    const char *us = l ? strchr(l, '_') : NULL;
    char cc[3] = "US";
    if (us && us[1] && us[2]) { cc[0] = us[1]; cc[1] = us[2]; }
    return MultiByteToWideChar(CP_UTF8, 0, cc, -1, out, n);
}
static void find_w(const WIN32_FIND_DATAA *a, WIN32_FIND_DATAW *w) {
    w->dwFileAttributes = a->dwFileAttributes;
    w->ftCreationTime = a->ftCreationTime; w->ftLastAccessTime = a->ftLastAccessTime; w->ftLastWriteTime = a->ftLastWriteTime;
    w->nFileSizeHigh = a->nFileSizeHigh; w->nFileSizeLow = a->nFileSizeLow;
    MultiByteToWideChar(CP_UTF8, 0, a->cFileName, -1, w->cFileName, MAX_PATH);
    w->cAlternateFileName[0] = 0;
}
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd) {
    char p[PATH_MAX * 2];
    WIN32_FIND_DATAA a;
    if (!WideCharToMultiByte(CP_UTF8, 0, pattern, -1, p, sizeof p, NULL, NULL)) return INVALID_HANDLE_VALUE;
    HANDLE h = FindFirstFileA(p, &a);
    if (h != INVALID_HANDLE_VALUE) find_w(&a, fd);
    return h;
}
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd) { WIN32_FIND_DATAA a; if (!FindNextFileA(h, &a)) return FALSE; find_w(&a, fd); return TRUE; }
typedef struct { uintptr_t *pcs; int n, max; } Bt;
static _Unwind_Reason_Code bt_step(struct _Unwind_Context *uc, void *arg) {
    Bt *b = arg;
    if (b->n >= b->max) return _URC_END_OF_STACK;
    uintptr_t pc = (uintptr_t)_Unwind_GetIP(uc);
    if (!pc) return _URC_END_OF_STACK;
    b->pcs[b->n++] = pc;
    return _URC_NO_REASON;
}
int plat_backtrace(uintptr_t *pcs, int max) { Bt b = {pcs, 0, max}; _Unwind_Backtrace(bt_step, &b); return b.n; }
void *wglGetProcAddress(const char *name) { return (void *)SDL_GL_GetProcAddress(name); }

/* ================================================================ the game window, locale */
BOOL IsIconic(HWND h) { return h && (SDL_GetWindowFlags((SDL_Window *)h) & SDL_WINDOW_MINIMIZED) != 0; }
BOOL IsWindowVisible(HWND h) { return h && !(SDL_GetWindowFlags((SDL_Window *)h) & SDL_WINDOW_HIDDEN); }
BOOL IsWindow(HWND h) { return h && h == plat_game_window; }
BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp) { if (plat_game_window) fn(plat_game_window, lp); return TRUE; }
/* Window changes asked from other threads are applied by the window's own thread (plat_window_service, display.c):
 * SDL wants a window touched by the thread that made it. */
static pthread_mutex_t g_win_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_win_title[256];
static int g_win_title_new, g_win_show = -1;
BOOL ShowWindow(HWND h, int cmd) {
    if (!h) return FALSE;
    pthread_mutex_lock(&g_win_lock); g_win_show = cmd; pthread_mutex_unlock(&g_win_lock);
    return TRUE;
}
BOOL SetWindowTextA(HWND h, LPCSTR text) {
    if (!h) return FALSE;
    pthread_mutex_lock(&g_win_lock); snprintf(g_win_title, sizeof g_win_title, "%s", text); g_win_title_new = 1; pthread_mutex_unlock(&g_win_lock);
    return TRUE;
}
void plat_window_service(void) {
    SDL_Window *w = (SDL_Window *)plat_game_window;
    char title[256] = ""; int set = 0, show;
    if (!w) return;
    pthread_mutex_lock(&g_win_lock);
    if (g_win_title_new) { memcpy(title, g_win_title, sizeof title); g_win_title_new = 0; set = 1; }
    show = g_win_show; g_win_show = -1;
    pthread_mutex_unlock(&g_win_lock);
    if (set) SDL_SetWindowTitle(w, title);
    if (show == SW_HIDE) SDL_HideWindow(w);
    else if (show == SW_MINIMIZE || show == SW_SHOWMINIMIZED || show == SW_SHOWMINNOACTIVE) SDL_MinimizeWindow(w);
    else if (show >= 0) { SDL_ShowWindow(w); SDL_RestoreWindow(w); }
}
DWORD SetThreadExecutionState(DWORD flags) {
    static DWORD prev = ES_CONTINUOUS;
    DWORD was = prev;
    if (flags & ES_DISPLAY_REQUIRED) SDL_DisableScreenSaver(); else SDL_EnableScreenSaver();
    prev = flags;
    return was;
}
static void locale_tag(char *out, size_t n) {           /* "en_US.UTF-8" -> "en-US" */
    const char *v = NULL, *names[4] = {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"};
    for (int i = 0; i < 4 && !v; i++) { const char *e = getenv(names[i]); if (e && e[0] && strcmp(e, "C") && strcmp(e, "POSIX")) v = e; }
    if (!v) v = "en_US";
    size_t k = 0;
    for (; v[k] && v[k] != '.' && v[k] != '@' && v[k] != ':' && k + 1 < n; k++) out[k] = v[k] == '_' ? '-' : v[k];
    out[k] = 0;
}
BOOL GetUserPreferredUILanguages(DWORD flags, ULONG *count, LPWSTR list, ULONG *size) {
    (void)flags;
    char tag[64];
    locale_tag(tag, sizeof tag);
    int k = MultiByteToWideChar(CP_UTF8, 0, tag, -1, NULL, 0);
    if (!list || *size < (ULONG)k + 1) { *size = (ULONG)k + 1; *count = 1; return list == NULL; }
    MultiByteToWideChar(CP_UTF8, 0, tag, -1, list, (int)*size);
    list[k] = 0;                                         /* the list ends with an empty string */
    *count = 1; *size = (ULONG)k + 1;
    return TRUE;
}
int GetUserDefaultLocaleName(LPWSTR out, int n) { char tag[64]; locale_tag(tag, sizeof tag); return MultiByteToWideChar(CP_UTF8, 0, tag, -1, out, n); }

/* ================================================================ CryptoAPI subset (plat/wincrypt.h) on OpenSSL */
#include "plat/wincrypt.h"
#include <openssl/evp.h>
BOOL CryptAcquireContextA(HCRYPTPROV *prov, LPCSTR container, LPCSTR provider, DWORD type, DWORD flags) { (void)container; (void)provider; (void)type; (void)flags; *prov = 1; return TRUE; }
BOOL CryptReleaseContext(HCRYPTPROV prov, DWORD flags) { (void)prov; (void)flags; return TRUE; }
BOOL CryptGenRandom(HCRYPTPROV prov, DWORD n, BYTE *buf) { (void)prov; return SystemFunction036(buf, n); }
BOOL CryptCreateHash(HCRYPTPROV prov, ALG_ID alg, HCRYPTKEY key, DWORD flags, HCRYPTHASH *out) {
    (void)prov; (void)key; (void)flags;
    const EVP_MD *md = alg == CALG_MD5 ? EVP_md5() : alg == CALG_SHA1 ? EVP_sha1() : alg == CALG_SHA_256 ? EVP_sha256() : NULL;
    if (!md) { t_lasterr = ERROR_INVALID_PARAMETER; return FALSE; }
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    if (!c || !EVP_DigestInit_ex(c, md, NULL)) { EVP_MD_CTX_free(c); return FALSE; }
    *out = (HCRYPTHASH)c;
    return TRUE;
}
BOOL CryptHashData(HCRYPTHASH h, const BYTE *data, DWORD n, DWORD flags) { (void)flags; return EVP_DigestUpdate((EVP_MD_CTX *)h, data, n) == 1; }
BOOL CryptGetHashParam(HCRYPTHASH h, DWORD param, BYTE *out, DWORD *n, DWORD flags) {
    (void)flags;
    EVP_MD_CTX *c = (EVP_MD_CTX *)h;
    DWORD size = (DWORD)EVP_MD_CTX_get_size(c);
    if (param == HP_HASHSIZE) { if (*n < 4) return FALSE; memcpy(out, &size, 4); *n = 4; return TRUE; }
    if (param != HP_HASHVAL || *n < size) { *n = size; return FALSE; }
    EVP_MD_CTX *copy = EVP_MD_CTX_new();               /* Windows lets the hash be read and then closed: finish a copy */
    unsigned int len = 0;
    BOOL ok = copy && EVP_MD_CTX_copy_ex(copy, c) && EVP_DigestFinal_ex(copy, out, &len);
    EVP_MD_CTX_free(copy);
    *n = len;
    return ok;
}
BOOL CryptDestroyHash(HCRYPTHASH h) { EVP_MD_CTX_free((EVP_MD_CTX *)h); return TRUE; }

static BOOL bad_ptr(const void *p, UINT_PTR n, int write) {
    if (!p) return n != 0;
    uintptr_t a = (uintptr_t)p, e = a + (n ? n : 1);
    while (a < e) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return TRUE;
        if (write && !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) return TRUE;
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return FALSE;
}
BOOL IsBadReadPtr(const void *p, UINT_PTR n) { return bad_ptr(p, n, 0); }
BOOL IsBadWritePtr(void *p, UINT_PTR n) { return bad_ptr(p, n, 1); }
BOOL GetClientRect(HWND h, RECT *r) {
    int w = 0, hh = 0;
    r->left = r->top = 0;
    if (h) SDL_GetWindowSizeInPixels((SDL_Window *)h, &w, &hh);
    r->right = w; r->bottom = hh;
    return h != NULL;
}
int GetWindowTextA(HWND h, LPSTR out, int n) {
    if (n <= 0) return 0;
    pthread_mutex_lock(&g_win_lock);
    if (g_win_title[0] || !h) snprintf(out, (size_t)n, "%s", g_win_title);
    else { const char *t = SDL_GetWindowTitle((SDL_Window *)h); snprintf(out, (size_t)n, "%s", t ? t : ""); }
    pthread_mutex_unlock(&g_win_lock);
    return (int)strlen(out);
}

/* ================================================================ waveOut on SDL3 audio (plat/mmsystem.h) */
#include "plat/mmsystem.h"
/* waveOut: buffers play in order; one is marked WHDR_DONE (and the event set) when SDL has pulled all of it, a little before
 * it is heard; waveOutGetPosition subtracts what SDL still holds. */
struct HWAVEOUT__ {
    SDL_AudioStream *stream;
    pthread_mutex_t lock;
    WAVEHDR *head, *tail;                /* written, not yet fully pulled */
    DWORD offset;                        /* bytes of head already pulled */
    HANDLE event;
    int block;                           /* bytes per frame */
    uint64_t pulled;                     /* bytes handed to SDL since open / reset */
};
static void SDLCALL wave_pull(void *user, SDL_AudioStream *st, int additional, int total) {
    (void)total;
    HWAVEOUT w = user;
    uint8_t buf[16384];
    pthread_mutex_lock(&w->lock);
    while (additional > 0) {
        int n = 0;
        while (n < (int)sizeof buf && n < additional && w->head) {
            DWORD left = w->head->dwBufferLength - w->offset;
            int k = (int)left;
            if (k > (int)sizeof buf - n) k = (int)sizeof buf - n;
            if (k > additional - n) k = additional - n;
            memcpy(buf + n, w->head->lpData + w->offset, (size_t)k);
            n += k; w->offset += (DWORD)k;
            if (w->offset >= w->head->dwBufferLength) {
                WAVEHDR *h = w->head;
                w->head = h->lpNext; if (!w->head) w->tail = NULL;
                w->offset = 0;
                h->lpNext = NULL;
                __atomic_store_n(&h->dwFlags, (h->dwFlags & ~(DWORD)WHDR_INQUEUE) | WHDR_DONE, __ATOMIC_RELEASE);
                if (w->event) SetEvent(w->event);
            }
        }
        if (!n) break;                                   /* nothing queued: SDL plays silence */
        SDL_PutAudioStreamData(st, buf, n);
        w->pulled += (uint64_t)n;
        additional -= n;
    }
    pthread_mutex_unlock(&w->lock);
}
MMRESULT waveOutOpen(HWAVEOUT *out, UINT dev, const WAVEFORMATEX *f, DWORD_PTR cb, DWORD_PTR inst, DWORD flags) {
    (void)dev; (void)inst;
    *out = NULL;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "plat: SDL audio cannot start: %s\n", SDL_GetError()); return MMSYSERR_NODRIVER; }
    HWAVEOUT w = calloc(1, sizeof *w);
    pthread_mutex_init(&w->lock, NULL);
    w->event = (flags & CALLBACK_EVENT) ? (HANDLE)cb : NULL;
    w->block = f->nBlockAlign;
    SDL_AudioSpec spec = {f->wBitsPerSample == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE, f->nChannels, (int)f->nSamplesPerSec};
    w->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, wave_pull, w);
    if (!w->stream) { fprintf(stderr, "plat: no audio output: %s\n", SDL_GetError()); pthread_mutex_destroy(&w->lock); free(w); return MMSYSERR_NODRIVER; }
    SDL_ResumeAudioStreamDevice(w->stream);              /* waveOut starts playing at once */
    *out = w;
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutPrepareHeader(HWAVEOUT w, WAVEHDR *h, UINT size) { (void)w; (void)size; h->dwFlags |= WHDR_PREPARED; h->dwFlags &= ~(DWORD)WHDR_DONE; return MMSYSERR_NOERROR; }
MMRESULT waveOutUnprepareHeader(HWAVEOUT w, WAVEHDR *h, UINT size) { (void)w; (void)size; if (h->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING; h->dwFlags &= ~(DWORD)WHDR_PREPARED; return MMSYSERR_NOERROR; }
MMRESULT waveOutWrite(HWAVEOUT w, WAVEHDR *h, UINT size) {
    (void)size;
    if (!w) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&w->lock);
    h->lpNext = NULL;
    h->dwFlags = (h->dwFlags & ~(DWORD)WHDR_DONE) | WHDR_INQUEUE;
    if (w->tail) w->tail->lpNext = h; else w->head = h;
    w->tail = h;
    pthread_mutex_unlock(&w->lock);
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutPause(HWAVEOUT w) { return w && SDL_PauseAudioStreamDevice(w->stream) ? MMSYSERR_NOERROR : MMSYSERR_ERROR; }
MMRESULT waveOutRestart(HWAVEOUT w) { return w && SDL_ResumeAudioStreamDevice(w->stream) ? MMSYSERR_NOERROR : MMSYSERR_ERROR; }
MMRESULT waveOutReset(HWAVEOUT w) {
    if (!w) return MMSYSERR_INVALHANDLE;
    SDL_LockAudioStream(w->stream);
    pthread_mutex_lock(&w->lock);
    for (WAVEHDR *h = w->head; h;) { WAVEHDR *n = h->lpNext; h->lpNext = NULL; h->dwFlags = (h->dwFlags & ~(DWORD)WHDR_INQUEUE) | WHDR_DONE; h = n; }
    w->head = w->tail = NULL; w->offset = 0; w->pulled = 0;
    SDL_ClearAudioStream(w->stream);
    pthread_mutex_unlock(&w->lock);
    SDL_UnlockAudioStream(w->stream);
    if (w->event) SetEvent(w->event);
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutClose(HWAVEOUT w) {
    if (!w) return MMSYSERR_INVALHANDLE;
    waveOutReset(w);
    SDL_DestroyAudioStream(w->stream);
    pthread_mutex_destroy(&w->lock);
    free(w);
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutGetPosition(HWAVEOUT w, MMTIME *t, UINT size) {
    (void)size;
    if (!w) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&w->lock);
    uint64_t pulled = w->pulled;
    pthread_mutex_unlock(&w->lock);
    int queued = SDL_GetAudioStreamQueued(w->stream);    /* pulled but not yet played */
    uint64_t played = pulled > (uint64_t)(queued > 0 ? queued : 0) ? pulled - (uint64_t)queued : 0;
    if (t->wType == TIME_BYTES) t->u.cb = (DWORD)played;
    else { t->wType = TIME_SAMPLES; t->u.sample = (DWORD)(played / (uint64_t)(w->block ? w->block : 4)); }
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutSetVolume(HWAVEOUT w, DWORD vol) {
    if (!w) return MMSYSERR_INVALHANDLE;
    float l = (float)(vol & 0xffff) / 65535.0f, r = (float)(vol >> 16) / 65535.0f;
    SDL_SetAudioStreamGain(w->stream, l > r ? l : r);
    return MMSYSERR_NOERROR;
}

/* boot.c print_build_identity: the Windows build names the link time stamp of the exe and of each DLL beside it; an ELF
 * file has none, so the Linux build names each of its own files (the exe's folder) with its modification time, size and
 * GNU build id when the linker wrote one. File names only, as there. */
#ifndef __APPLE__
typedef struct { const char *dir; size_t dl; } BuildIdArg;
static int build_identity_one(struct dl_phdr_info *info, size_t size, void *p) {
    (void)size;
    const BuildIdArg *a = p;
    const char *name = info->dlpi_name && info->dlpi_name[0] ? info->dlpi_name : NULL;
    char self[PATH_MAX];
    if (!name) { ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1); if (k <= 0) return 0; self[k] = 0; name = self; }
    if (strncmp(name, a->dir, a->dl)) return 0;          /* ours: the exe's folder */
    struct stat st;
    char when[32] = "?", id[48] = "";
    if (!stat(name, &st)) { struct tm tm; gmtime_r(&st.st_mtime, &tm); strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", &tm); }
    for (int i = 0; i < info->dlpi_phnum && !id[0]; i++) {
        if (info->dlpi_phdr[i].p_type != PT_NOTE) continue;
        const uint8_t *n = (const uint8_t *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr), *end = n + info->dlpi_phdr[i].p_memsz;
        while (n + 12 <= end) {
            uint32_t nsz = ((const uint32_t *)n)[0], dsz = ((const uint32_t *)n)[1], type = ((const uint32_t *)n)[2];
            const uint8_t *desc = n + 12 + ((nsz + 3) & ~3u);
            if (type == 3 && nsz == 4 && !memcmp(n + 12, "GNU", 4)) {
                for (uint32_t k = 0; k < dsz && k < 20; k++) snprintf(id + 2 * k, sizeof id - 2 * k, "%02x", desc[k]);
                break;
            }
            n = desc + ((dsz + 3) & ~3u);
        }
    }
    printf("build: %-22s modified %s (%lld bytes%s%s)\n", name + a->dl, when, (long long)st.st_size, id[0] ? ", build id " : "", id);
    return 0;
}
void plat_print_build_identity(const char *exe, size_t dir_len) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s", (int)dir_len, exe);
    for (char *q = dir; *q; q++) if (*q == '\\') *q = '/';
    BuildIdArg a = {dir, strlen(dir)};
    dl_iterate_phdr(build_identity_one, &a);
}
#else
/* macOS: each loaded image of the exe's folder with its modification time, size and the linker's UUID (LC_UUID) */
void plat_print_build_identity(const char *exe, size_t dir_len) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s", (int)dir_len, exe);
    for (char *q = dir; *q; q++) if (*q == '\\') *q = '/';
    size_t dl = strlen(dir);
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name || strncmp(name, dir, dl)) continue;
        struct stat st;
        char when[32] = "?", id[48] = "";
        memset(&st, 0, sizeof st);
        if (!stat(name, &st)) { struct tm tm; gmtime_r(&st.st_mtime, &tm); strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", &tm); }
        const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(i);
        const uint8_t *lc = (const uint8_t *)(h + 1);
        for (uint32_t k = 0; h && k < h->ncmds; k++) {
            const struct load_command *c = (const struct load_command *)lc;
            if (c->cmd == LC_UUID) {
                const struct uuid_command *u = (const struct uuid_command *)c;
                for (int j = 0; j < 16; j++) snprintf(id + 2 * j, sizeof id - 2 * j, "%02x", u->uuid[j]);
                break;
            }
            lc += c->cmdsize;
        }
        printf("build: %-22s modified %s (%lld bytes%s%s)\n", name + dl, when, (long long)st.st_size, id[0] ? ", uuid " : "", id);
    }
}
#endif
/* There are no 8.3 names: callers fall back to the long name. */
DWORD GetShortPathNameW(LPCWSTR path, LPWSTR out, DWORD n) { (void)path; (void)out; (void)n; SetLastError(1 /* ERROR_INVALID_FUNCTION */); return 0; }
#ifdef __APPLE__
#undef fread
size_t plat_fread(void *p, size_t size, size_t n, FILE *f) {
    plat_prepare_write(p, size * n);                 /* stdio reads large requests straight into p */
    return fread(p, size, n, f);
}
#endif
