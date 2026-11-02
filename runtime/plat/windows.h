/* Linux build: the subset of the Win32 API the runtime uses, on POSIX (plat_posix.c).
 * This folder is on the include path only off Windows, so <windows.h> and the few other SDK headers the runtime
 * includes resolve here. Types keep their Windows sizes (LONG and DWORD are 32-bit); handles are PlatObj pointers.
 * What has no meaning here (window messages, GDI, SEH, debug registers) is not declared: the files that use it
 * keep that code under #ifdef _WIN32. */
#ifndef PLAT_WINDOWS_H
#define PLAT_WINDOWS_H
#ifdef _WIN32
#error "plat/windows.h is the Linux stand-in; it must not be on the include path of a Windows build"
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>

#include "plat_pre.h"

#ifdef __cplusplus                                       /* menu.cpp (the settings menu) is C++: these are the C functions of plat_posix.c */
extern "C" {
#endif

#define WINAPI
#define CALLBACK
#define APIENTRY
#define WINAPIV
#define FAR
#define NEAR
#define CONST const
#define VOID void

typedef int BOOL;
typedef uint8_t BOOLEAN;
typedef uint8_t BYTE, UCHAR;
typedef uint16_t WORD, USHORT;
typedef int16_t SHORT;
typedef char CHAR;
typedef int32_t INT, LONG, INT32;
typedef uint32_t UINT, DWORD, ULONG, UINT32, DWORD32;
typedef int64_t LONG64, LONGLONG, INT64;
typedef uint64_t ULONG64, ULONGLONG, UINT64, DWORD64, QWORD;
typedef intptr_t INT_PTR, LONG_PTR, SSIZE_T;
typedef uintptr_t UINT_PTR, ULONG_PTR, DWORD_PTR, SIZE_T;
typedef float FLOAT;
typedef wchar_t WCHAR;
typedef void *PVOID, *LPVOID, *HANDLE, *HMODULE, *HINSTANCE, *HWND, *HDC, *HFONT, *HKEY, *HMONITOR, *FARPROC;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef DWORD *LPDWORD;
typedef BOOL *LPBOOL;
typedef LONG HRESULT;
typedef LONG NTSTATUS;
typedef UINT MMRESULT;

#define TRUE 1
#define FALSE 0
#define MAX_PATH 1024
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define STILL_ACTIVE 259
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_NO_MORE_FILES 18
#define ERROR_SHARING_VIOLATION 32
#define ERROR_HANDLE_EOF 38
#define ERROR_TIMEOUT 1460                 /* SleepConditionVariable* on a timeout (plat_posix.c cv_wait) */
#define ERROR_FILE_EXISTS 80
#define ERROR_INVALID_PARAMETER 87
#define ERROR_DISK_FULL 112
#define ERROR_DIR_NOT_EMPTY 145
#define ERROR_ALREADY_EXISTS 183
#define ERROR_INVALID_ADDRESS 487
#define ERROR_NOACCESS 998                 /* a read into memory that is not writable (EFAULT) */
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)(((DWORD_PTR)(l) >> 16) & 0xffff))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

typedef union { struct { DWORD LowPart; LONG HighPart; }; struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER;
typedef union { struct { DWORD LowPart; DWORD HighPart; }; struct { DWORD LowPart; DWORD HighPart; } u; ULONGLONG QuadPart; } ULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES;
typedef struct { ULONG_PTR Internal, InternalHigh; union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; }; HANDLE hEvent; } OVERLAPPED;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef GUID IID, CLSID;
#define IsEqualGUID(a, b) (memcmp((a), (b), sizeof(GUID)) == 0)

/* ---- time ---- */
BOOL QueryPerformanceCounter(LARGE_INTEGER *t);          /* 100 ns ticks, CLOCK_MONOTONIC */
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);        /* 10 000 000 */
ULONGLONG GetTickCount64(void);
DWORD GetTickCount(void);
void GetSystemTimeAsFileTime(FILETIME *ft);
void GetSystemTime(SYSTEMTIME *st);
void GetLocalTime(SYSTEMTIME *st);
BOOL GetSystemTimes(FILETIME *idle, FILETIME *kernel, FILETIME *user);
BOOL GetProcessTimes(HANDLE p, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user);
void Sleep(DWORD ms);
#define SwitchToThread() (sched_yield(), 1)
#ifdef __aarch64__                                      /* macOS on Apple Silicon */
#define YieldProcessor() __builtin_arm_yield()
#else
#define YieldProcessor() __builtin_ia32_pause()
#endif
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define TIMERR_NOERROR 0
#define timeBeginPeriod(ms) ((MMRESULT)TIMERR_NOERROR)
#define timeEndPeriod(ms) ((MMRESULT)TIMERR_NOERROR)

/* ---- interlocked: full barriers, like the Windows ones; type-generic on the pointee ---- */
#define PLAT_SC __ATOMIC_SEQ_CST
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, PLAT_SC)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, PLAT_SC)
#define InterlockedIncrement64 InterlockedIncrement
#define InterlockedDecrement64 InterlockedDecrement
#define InterlockedIncrementAcquire InterlockedIncrement
#define InterlockedIncrementRelease InterlockedIncrement
#define InterlockedAdd(p, v) __atomic_add_fetch((p), (v), PLAT_SC)
#define InterlockedAdd64 InterlockedAdd
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), PLAT_SC)
#define InterlockedExchangeAdd64 InterlockedExchangeAdd
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), PLAT_SC)
#define InterlockedExchange64 InterlockedExchange
#define InterlockedExchange8 InterlockedExchange
#define InterlockedExchangePointer(p, v) ((PVOID)__atomic_exchange_n((void *volatile *)(p), (void *)(v), PLAT_SC))
#define InterlockedOr(p, v) __atomic_fetch_or((p), (v), PLAT_SC)
#define InterlockedOr8 InterlockedOr
#define InterlockedOr64 InterlockedOr
#define InterlockedAnd(p, v) __atomic_fetch_and((p), (v), PLAT_SC)
#define InterlockedAnd64 InterlockedAnd
#define InterlockedXor(p, v) __atomic_fetch_xor((p), (v), PLAT_SC)
#ifdef __APPLE__              /* Apple's clang has no __typeof_unqual__: a volatile local serves the exchange as well */
#define PLAT_TYPEOF_UNQUAL __typeof__
#else
#define PLAT_TYPEOF_UNQUAL __typeof_unqual__
#endif
#define InterlockedCompareExchange(p, x, c) \
    ({ PLAT_TYPEOF_UNQUAL(*(p)) plat_c_ = (c); __atomic_compare_exchange_n((p), &plat_c_, (x), 0, PLAT_SC, PLAT_SC); plat_c_; })
#define InterlockedCompareExchange64 InterlockedCompareExchange
#define InterlockedCompareExchangeAcquire InterlockedCompareExchange
#define InterlockedCompareExchangeRelease InterlockedCompareExchange
#define InterlockedCompareExchangePointer(p, x, c) \
    ({ void *plat_c_ = (void *)(c); __atomic_compare_exchange_n((void *volatile *)(p), &plat_c_, (void *)(x), 0, PLAT_SC, PLAT_SC); (PVOID)plat_c_; })

/* ---- locks and condition variables ---- */
typedef pthread_mutex_t CRITICAL_SECTION;                /* recursive, like the Windows one */
typedef CRITICAL_SECTION *LPCRITICAL_SECTION;
void InitializeCriticalSection(CRITICAL_SECTION *cs);
#define InitializeCriticalSectionAndSpinCount(cs, n) (InitializeCriticalSection(cs), TRUE)
#define InitializeCriticalSectionEx(cs, n, f) (InitializeCriticalSection(cs), TRUE)
#define EnterCriticalSection(cs) pthread_mutex_lock(cs)
#define LeaveCriticalSection(cs) pthread_mutex_unlock(cs)
#define TryEnterCriticalSection(cs) (pthread_mutex_trylock(cs) == 0)
#define DeleteCriticalSection(cs) pthread_mutex_destroy(cs)
typedef pthread_rwlock_t SRWLOCK;
typedef SRWLOCK *PSRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
#define InitializeSRWLock(l) pthread_rwlock_init((l), NULL)
#define AcquireSRWLockExclusive(l) pthread_rwlock_wrlock(l)
#define ReleaseSRWLockExclusive(l) pthread_rwlock_unlock(l)
#define AcquireSRWLockShared(l) pthread_rwlock_rdlock(l)
#define ReleaseSRWLockShared(l) pthread_rwlock_unlock(l)
#define TryAcquireSRWLockExclusive(l) (pthread_rwlock_trywrlock(l) == 0)
#define TryAcquireSRWLockShared(l) (pthread_rwlock_tryrdlock(l) == 0)
/* A futex sequence counter: works with either kind of lock, as the Windows one does. */
typedef struct { volatile uint32_t seq; } CONDITION_VARIABLE;
typedef CONDITION_VARIABLE *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {0}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 1
#define InitializeConditionVariable(cv) ((cv)->seq = 0)
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms);
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags);
void WakeConditionVariable(CONDITION_VARIABLE *cv);
void WakeAllConditionVariable(CONDITION_VARIABLE *cv);
typedef struct { volatile int state; } INIT_ONCE;
typedef INIT_ONCE *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT {0}
typedef BOOL (*PINIT_ONCE_FN)(PINIT_ONCE, PVOID, PVOID *);
BOOL InitOnceExecuteOnce(INIT_ONCE *once, PINIT_ONCE_FN fn, PVOID param, PVOID *ctx);
#define INIT_ONCE_INIT_FAILED 0x4
BOOL InitOnceBeginInitialize(INIT_ONCE *once, DWORD flags, BOOL *pending, PVOID *ctx);   /* synchronous form only (flags 0) */
BOOL InitOnceComplete(INIT_ONCE *once, DWORD flags, PVOID ctx);

/* ---- handles: threads, events, semaphores, mutexes, timers, files ---- */
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
#define CREATE_SUSPENDED 0x4
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000
HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *tid);
/* SuspendThread (Linux, x86-64): a signal to the thread, which waits in its handler until ResumeThread; counted as on
 * Windows. A thread inside plat's memory bookkeeping (VirtualAlloc ...) stops when it leaves it. Fails ((DWORD)-1) for
 * the calling thread, a thread not started or ended, and on macOS. */
DWORD ResumeThread(HANDLE h);
DWORD SuspendThread(HANDLE h);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
DWORD GetThreadId(HANDLE h);
BOOL GetExitCodeThread(HANDLE h, DWORD *code);
#define THREAD_PRIORITY_IDLE -15
#define THREAD_PRIORITY_LOWEST -2
#define THREAD_PRIORITY_BELOW_NORMAL -1
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
BOOL SetThreadPriority(HANDLE h, int prio);
int GetThreadPriority(HANDLE h);
#define SetThreadPriorityBoost(h, off) TRUE       /* no priority boost to turn off on Linux */
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);
HRESULT SetThreadDescription(HANDLE h, LPCWSTR name);
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys);
HANDLE CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, LPCSTR name);
#define CreateEvent CreateEventA
#define CreateEventW(sa, m, i, n) CreateEventA(sa, m, i, NULL)
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
HANDLE CreateSemaphoreA(SECURITY_ATTRIBUTES *sa, LONG initial, LONG maximum, LPCSTR name);
#define CreateSemaphore CreateSemaphoreA
#define CreateSemaphoreW(sa, i, m, n) CreateSemaphoreA(sa, i, m, NULL)
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *prev);
HANDLE CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL owned, LPCSTR name);
#define CreateMutex CreateMutexA
BOOL ReleaseMutex(HANDLE h);
#define TIMER_ALL_ACCESS 0x1F0003
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
HANDLE CreateWaitableTimerExW(SECURITY_ATTRIBUTES *sa, LPCWSTR name, DWORD flags, DWORD access);
#define CreateWaitableTimerA(sa, manual, name) CreateWaitableTimerExW(sa, NULL, 0, TIMER_ALL_ACCESS)
#define CreateWaitableTimer CreateWaitableTimerA
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, LPVOID arg, BOOL resume);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
#define WaitForSingleObjectEx(h, ms, a) WaitForSingleObject(h, ms)
BOOL CloseHandle(HANDLE h);
#define DUPLICATE_SAME_ACCESS 2
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *dst, DWORD access, BOOL inherit, DWORD opts);
DWORD GetLastError(void);
void SetLastError(DWORD e);
__attribute__((noreturn)) void ExitProcess(UINT code);
BOOL TerminateProcess(HANDLE p, UINT code);

/* thread-local slots */
#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu
#define FLS_OUT_OF_INDEXES 0xFFFFFFFFu
typedef void (*PFLS_CALLBACK_FUNCTION)(PVOID);
DWORD TlsAlloc(void);
BOOL TlsFree(DWORD i);
LPVOID TlsGetValue(DWORD i);
BOOL TlsSetValue(DWORD i, LPVOID v);
DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION cb);
BOOL FlsFree(DWORD i);
PVOID FlsGetValue(DWORD i);
BOOL FlsSetValue(DWORD i, PVOID v);

/* fibers (ucontext) */
typedef void (*LPFIBER_START_ROUTINE)(LPVOID);
LPVOID ConvertThreadToFiber(LPVOID param);
BOOL ConvertFiberToThread(void);
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID param);
#define CreateFiber(stack, fn, param) CreateFiberEx(stack, stack, 0, fn, param)
void SwitchToFiber(LPVOID fiber);
void DeleteFiber(LPVOID fiber);
LPVOID GetCurrentFiber(void);
LPVOID GetFiberData(void);
/* What Windows keeps in its fiber block and TEB, for the rollback's save of switched-away fibers (r1_state.c). A fiber
 * block is PLAT_FIBER_BLOCK bytes (the registers saved at its last switch included); the functions also read a saved copy. */
#define PLAT_FIBER_BLOCK 0x500
int plat_fiber_stack(const void *fiber, uint64_t *lo, uint64_t *hi);   /* its whole host stack; 0 = none (a thread's own) */
uint64_t plat_fiber_saved_sp(const void *fiber);         /* host stack pointer at its last switch away */
void *plat_thread_fiber(HANDLE h);                       /* the fiber that thread runs now (Windows: TEB FiberData) */
int plat_thread_stack(HANDLE h, uint64_t *lo, uint64_t *hi);           /* a thread's own stack (CreateThread threads) */

/* ---- thread context and vectored exception handlers (x86-64 Linux; r1_state.c, rb_learned.c) ----
 * CONTEXT: the integer registers only. GetThreadContext: from the suspended thread's signal frame (a running thread is
 * suspended for the read). Vectored handlers run from plat's SIGSEGV / SIGBUS handler for a memory fault (code
 * EXCEPTION_ACCESS_VIOLATION, ExceptionInformation[0] 1 = write, [1] = address); a change of ContextRecord is not applied.
 * EXCEPTION_CONTINUE_SEARCH from all of them: the handler that was installed before (hostrt.c's crash report). */
#define CONTEXT_AMD64 0x100000
#define CONTEXT_CONTROL (CONTEXT_AMD64 | 0x1)
#define CONTEXT_INTEGER (CONTEXT_AMD64 | 0x2)
#define CONTEXT_FULL (CONTEXT_AMD64 | 0xb)
typedef struct {
    DWORD ContextFlags, EFlags;
    DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15, Rip;
} CONTEXT;
BOOL GetThreadContext(HANDLE h, CONTEXT *c);
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct EXCEPTION_RECORD {
    DWORD ExceptionCode, ExceptionFlags;
    struct EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD;
typedef struct EXCEPTION_POINTERS { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS;
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *e);
PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER fn);
int plat_protect_raw(void *p, size_t n, DWORD prot);    /* mprotect alone: VirtualQuery keeps the old protection (rb_learned.c) */
int plat_vm_tracked(const void *p);                     /* p lies in memory made by VirtualAlloc (the page-state map), not in a host mapping VirtualQuery reads from the kernel (r1_state.c) */

/* ---- virtual memory (mmap, with a page-state map for VirtualQuery) ---- */
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define MEM_PRIVATE 0x20000
#define MEM_MAPPED 0x40000
#define MEM_RESET 0x80000
#define MEM_WRITE_WATCH 0x200000
#define MEM_LARGE_PAGES 0x20000000
#define MEM_IMAGE 0x1000000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_GUARD 0x100
#define WRITE_WATCH_FLAG_RESET 1
typedef struct {
    PVOID BaseAddress, AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION;
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD *old);
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mi, SIZE_T len);
UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addrs, ULONG_PTR *count, ULONG *gran);   /* not available: 1 (callers fall back) */
UINT ResetWriteWatch(LPVOID base, SIZE_T size);
int plat_write_fault(void *addr);                        /* macOS write watch: 1 = a fault on a watched page, resolved (hostrt.c's crash handler asks first) */
void plat_prepare_write(void *p, size_t n);              /* before the kernel writes into [p, p+n) (read, fread): marks watched pages written */
BOOL FlushInstructionCache(HANDLE p, LPCVOID base, SIZE_T size);
BOOL ReadProcessMemory(HANDLE p, LPCVOID src, LPVOID dst, SIZE_T n, SIZE_T *got);
typedef struct {
    union { DWORD dwOemId; struct { WORD wProcessorArchitecture, wReserved; }; };
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO;
void GetSystemInfo(SYSTEM_INFO *si);
typedef struct { DWORD dwLength, dwMemoryLoad; ULONGLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual; } MEMORYSTATUSEX;
BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *m);
typedef struct {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
           QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX;
typedef PROCESS_MEMORY_COUNTERS_EX PROCESS_MEMORY_COUNTERS;
BOOL GetProcessMemoryInfo(HANDLE p, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb);
#define K32GetProcessMemoryInfo GetProcessMemoryInfo
HANDLE CreateFileMappingA(HANDLE f, SECURITY_ATTRIBUTES *sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name);
#define FILE_MAP_READ 4
#define FILE_MAP_WRITE 2
LPVOID MapViewOfFile(HANDLE m, DWORD access, DWORD hi, DWORD lo, SIZE_T n);
BOOL UnmapViewOfFile(LPCVOID p);

/* ---- C runtime names ---- */
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define _strdup strdup
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _wcsicmp wcscasecmp
#define lstrcmpiW wcscasecmp
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _fileno fileno
#define _isatty isatty
#define __time64_t time_t
#define _time64 time
#define _mkgmtime64 timegm
#define _mkgmtime timegm
#define _localtime64_s(tm, t) (localtime_r((t), (tm)) ? 0 : 22)
#define _gmtime64_s(tm, t) (gmtime_r((t), (tm)) ? 0 : 22)
#define localtime_s(tm, t) (localtime_r((t), (tm)) ? 0 : 22)
#define gmtime_s(tm, t) (gmtime_r((t), (tm)) ? 0 : 22)
#define _putenv_s(n, v) setenv((n), (v), 1)
#define _getpid getpid
#define _byteswap_ulong __builtin_bswap32
#define _byteswap_uint64 __builtin_bswap64
#define _byteswap_ushort __builtin_bswap16
#define _rotl(v, s) ((uint32_t)(((uint32_t)(v) << ((s) & 31)) | ((uint32_t)(v) >> ((32 - ((s) & 31)) & 31))))
#define _rotl64(v, s) ((uint64_t)(((uint64_t)(v) << ((s) & 63)) | ((uint64_t)(v) >> ((64 - ((s) & 63)) & 63))))
int _putenv(const char *s);
char *_strlwr(char *s);
char *_strupr(char *s);
void *_aligned_malloc(size_t size, size_t align);
void *_aligned_realloc(void *p, size_t size, size_t align);
size_t _aligned_msize(void *p, size_t align, size_t off);
void _aligned_free(void *p);
char *_fullpath(char *out, const char *path, size_t n);
int _mkdir(const char *path);
extern int __argc;
extern char **__argv;
/* Windows paths: every path the runtime builds uses '\'. These open and name files after turning it into '/'. */
FILE *plat_fopen(const char *path, const char *mode);
#define fopen plat_fopen
int plat_remove(const char *path);
#define remove plat_remove
#define _unlink plat_remove
int plat_rename(const char *a, const char *b);
#define rename plat_rename
#ifdef __APPLE__                                         /* fread into write-watched memory: the kernel's write would fail, not fault */
size_t plat_fread(void *p, size_t size, size_t n, FILE *f);
#define fread plat_fread
#endif
const char *plat_path(const char *in, char *out, size_t n);        /* '\' -> '/' */

/* ---- files ---- */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_FILE_SIZE 0xFFFFFFFFu
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 8
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disp, DWORD flags, HANDLE tmpl);
#define CreateFile CreateFileA
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *got, OVERLAPPED *ov);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *put, OVERLAPPED *ov);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *pos, DWORD how);
DWORD SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD how);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
DWORD GetFileSize(HANDLE h, DWORD *hi);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
int plat_handle_fd(HANDLE h);                            /* the file descriptor behind a file HANDLE, or -1 */
DWORD GetFileAttributesA(LPCSTR path);
#define GetFileAttributes GetFileAttributesA
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID out);
BOOL SetFileAttributesA(LPCSTR path, DWORD attr);
BOOL CreateDirectoryA(LPCSTR path, SECURITY_ATTRIBUTES *sa);
#define CreateDirectory CreateDirectoryA
BOOL RemoveDirectoryA(LPCSTR path);
BOOL DeleteFileA(LPCSTR path);
#define DeleteFile DeleteFileA
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
#define MoveFileA(a, b) MoveFileExA((a), (b), 0)
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
DWORD GetTempPathA(DWORD n, LPSTR out);
BOOL GetDiskFreeSpaceExA(LPCSTR path, ULARGE_INTEGER *avail, ULARGE_INTEGER *total, ULARGE_INTEGER *free_);
DWORD GetCurrentDirectoryA(DWORD n, LPSTR out);
BOOL SetCurrentDirectoryA(LPCSTR path);
DWORD GetFullPathNameA(LPCSTR path, DWORD n, LPSTR out, LPSTR *file);
DWORD GetShortPathNameW(LPCWSTR path, LPWSTR out, DWORD n);   /* always 0: no 8.3 names */
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA;
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
BOOL FindClose(HANDLE h);
#define WIN32_FIND_DATA WIN32_FIND_DATAA
#define FindFirstFile FindFirstFileA
#define FindNextFile FindNextFileA

/* ---- modules (dlopen): "x.dll" names are mapped to the Linux libraries / our .so files ---- */
HMODULE LoadLibraryA(LPCSTR name);
#define LoadLibrary LoadLibraryA
#define LoadLibraryExA(n, f, fl) LoadLibraryA(n)
FARPROC GetProcAddress(HMODULE m, LPCSTR name);
BOOL FreeLibrary(HMODULE m);
HMODULE GetModuleHandleA(LPCSTR name);
#define GetModuleHandle GetModuleHandleA
#define GetModuleHandleW(n) GetModuleHandleA(NULL)
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out);
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n);          /* with '\\' separators, as on Windows */
#define GetModuleFileName GetModuleFileNameA
uintptr_t plat_module_base(HMODULE m);                   /* load address (on Windows the HMODULE itself) */

/* ---- text ---- */
#define CP_ACP 0
#define CP_UTF8 65001
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR w, int wn);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int wn, LPSTR s, int n, LPCSTR def, LPBOOL used);

/* ---- input (SDL keyboard state; plat_posix.c) ---- */
#define WM_KEYDOWN 0x0100                  /* window messages: named by shared code, never sent (the window is SDL's) */
#define WM_CHAR 0x0102
#define WHEEL_DELTA 120                    /* one notch of a mouse wheel (menu.cpp gets SDL's wheel in these units) */
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_NUMPAD0 0x60
#define VK_NUMPAD9 0x69
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_OEM_PLUS 0xBB
#define VK_OEM_MINUS 0xBD
SHORT GetAsyncKeyState(int vk);
HWND GetForegroundWindow(void);                          /* the game window when it has the keyboard focus */
#define SetForegroundWindow(h) TRUE                /* does nothing: the window manager decides (nnapplet.c's text dialog) */
#define KEYEVENTF_KEYUP 2
#define keybd_event(vk, scan, flags, extra) ((void)0)        /* no synthetic input here */

/* ---- message boxes (SDL) ---- */
#define MB_OK 0x0
#define MB_OKCANCEL 0x1
#define MB_YESNO 0x4
#define MB_ICONERROR 0x10
#define MB_ICONQUESTION 0x20
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#define MB_TOPMOST 0x40000
#define MB_TASKMODAL 0x2000
#define MB_SYSTEMMODAL 0x1000
#define MB_SETFOREGROUND 0x10000
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7
int MessageBoxA(HWND w, LPCSTR text, LPCSTR title, UINT type);
int MessageBoxW(HWND w, LPCWSTR text, LPCWSTR title, UINT type);
#define MessageBox MessageBoxA

/* ---- odds and ends ---- */
#define NTAPI
DWORD GetCurrentProcessorNumber(void);
BOOLEAN SystemFunction036(PVOID buf, ULONG n);           /* RtlGenRandom: getrandom() */
#define RtlGenRandom SystemFunction036
BOOL QueryThreadCycleTime(HANDLE h, ULONG64 *cycles);   /* thread CPU time in ns (not cycles) */
#define SYNCHRONIZE 0x00100000
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define PROCESS_QUERY_INFORMATION 0x0400
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid); /* a handle that knows the pid: waits poll kill(pid, 0) */
BOOL GetExitCodeProcess(HANDLE p, DWORD *code);          /* STILL_ACTIVE while kill(pid, 0) succeeds */
intptr_t _get_osfhandle(int fd);                         /* a HANDLE that only names the descriptor (CloseHandle does not close it) */
BOOL GetUserNameA(LPSTR out, DWORD *n);
BOOL GetUserNameW(LPWSTR out, DWORD *n);
int GetUserDefaultGeoName(LPWSTR out, int n);            /* region from $LANG (en_US -> "US") */
#define GetWindowThreadProcessId(h, pid) ((*(pid) = GetCurrentProcessId()), (DWORD)1)    /* the only window is ours */
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW;
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd);
/* stack walking for the diagnostics: PCs of the calling thread, innermost first (_Unwind_Backtrace) */
int plat_backtrace(uintptr_t *pcs, int max);
int plat_current_stack(uintptr_t *lo, uintptr_t *hi);    /* the stack this thread runs on now (a fiber's or the thread's) */

/* OpenGL renderer: extension entry points from the current context (SDL) */
void *wglGetProcAddress(const char *name);

/* ---- the game window (an SDL_Window; display.c sets plat_game_window) ---- */
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM, LRESULT;
typedef BOOL (*WNDENUMPROC)(HWND, LPARAM);
extern HWND plat_game_window;
BOOL IsIconic(HWND h);
BOOL IsWindowVisible(HWND h);
BOOL IsWindow(HWND h);
#define GW_OWNER 4
#define GetWindow(h, cmd) ((HWND)NULL)
BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp);
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_RESTORE 9
BOOL ShowWindow(HWND h, int cmd);
BOOL SetWindowTextA(HWND h, LPCSTR text);
void plat_window_service(void);                          /* the window thread applies title / show requests */
#define ES_SYSTEM_REQUIRED 0x1
#define ES_DISPLAY_REQUIRED 0x2
#define ES_CONTINUOUS 0x80000000u
DWORD SetThreadExecutionState(DWORD flags);              /* display required = SDL screen saver off */
#define LOCALE_NAME_MAX_LENGTH 85
#define MUI_LANGUAGE_NAME 0x8
BOOL GetUserPreferredUILanguages(DWORD flags, ULONG *count, LPWSTR list, ULONG *size);   /* from $LANGUAGE / $LC_ALL / $LC_MESSAGES / $LANG */
int GetUserDefaultLocaleName(LPWSTR out, int n);

BOOL IsBadReadPtr(const void *p, UINT_PTR n);           /* VirtualQuery over the range */
BOOL IsBadWritePtr(void *p, UINT_PTR n);

/* ---- BMP files ---- */
#pragma pack(push, 2)
typedef struct { WORD bfType; DWORD bfSize; WORD bfReserved1, bfReserved2; DWORD bfOffBits; } BITMAPFILEHEADER;
#pragma pack(pop)
typedef struct { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER;
#define BI_RGB 0

BOOL GetClientRect(HWND h, RECT *r);                     /* the window's size in pixels */
int GetWindowTextA(HWND h, LPSTR out, int n);

#ifdef __cplusplus
}
#endif
#endif
