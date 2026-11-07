// pthreads over Win32. Every pthread object lives in game memory with glibc's size; the Win32 primitive is stored
// inside it at an offset glibc's zero static initializers leave zero, and SRWLOCK / CONDITION_VARIABLE are
// themselves zero-initialized, so PTHREAD_*_INITIALIZER objects work with no setup.
//
//   pthread_mutex_t (40): +0x04 recursion count, +0x08 owner tid, +0x10 kind (1 = recursive), +0x18 SRWLOCK
//   pthread_cond_t  (48): +0x00 CONDITION_VARIABLE
//   pthread_rwlock_t(56): +0x00 SRWLOCK, +0x08 writer tid
//   sem_t           (32): +0x00 HANDLE (semaphore)
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "linux/lnx.h"
#include "log.h"

using namespace raven::lnx;

namespace {

struct Thread {
    HANDLE h = nullptr;
    DWORD tid = 0;
    SYSV void* (*start)(void*) = nullptr;
    void* arg = nullptr;
    void* ret = nullptr;
    bool detached = false;
    std::vector<std::pair<SYSV void (*)(void*), void*>> cleanup;
};
thread_local Thread* t_self = nullptr;

Thread* self() {
    if (!t_self) {
        t_self = new Thread;  // a thread the game didn't create (main, FMOD, SDL)
        t_self->tid = GetCurrentThreadId();
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t_self->h, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    }
    return t_self;
}

// pthread keys
constexpr int kMaxKeys = 1024;
struct Key {
    bool used = false;
    SYSV void (*dtor)(void*) = nullptr;
};
Key g_keys[kMaxKeys];
std::mutex g_keys_mutex;
thread_local void* t_key_values[kMaxKeys];

void run_key_destructors() {
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (int k = 0; k < kMaxKeys; k++) {
            void* v = t_key_values[k];
            if (v && g_keys[k].used && g_keys[k].dtor) {
                t_key_values[k] = nullptr;
                g_keys[k].dtor(v);
                any = true;
            }
        }
        if (!any) break;
    }
}

DWORD WINAPI thread_main(void* p) {
    auto t = static_cast<Thread*>(p);
    t_self = t;
    t->ret = t->start(t->arg);
    run_key_destructors();
    return 0;
}

struct MutexView {
    uint8_t* m;
    LONG& count() { return *reinterpret_cast<LONG*>(m + 4); }
    LONG& owner() { return *reinterpret_cast<LONG*>(m + 8); }
    int kind() { return *reinterpret_cast<int*>(m + 16); }
    SRWLOCK* lock() { return reinterpret_cast<SRWLOCK*>(m + 24); }
};

}  // namespace

// ---- threads
struct lpthread_attr { uint64_t stack; int detach; int pad; uint64_t stackaddr; uint8_t rest[32]; };
SHIM(int, pthread_attr_init, (lpthread_attr* a)) { std::memset(a, 0, 56); return 0; }
SHIM(int, pthread_attr_destroy, (lpthread_attr*)) { return 0; }
SHIM(int, pthread_attr_setdetachstate, (lpthread_attr* a, int d)) { a->detach = d; return 0; }
SHIM(int, pthread_attr_setstacksize, (lpthread_attr* a, size_t s)) { a->stack = s; return 0; }
SHIM(int, pthread_attr_setstack, (lpthread_attr* a, void* addr, size_t s)) { a->stack = s; return 0; }
SHIM(int, pthread_attr_setinheritsched, (lpthread_attr*, int)) { return 0; }
SHIM(int, pthread_attr_setschedpolicy, (lpthread_attr*, int)) { return 0; }
SHIM(int, pthread_attr_setschedparam, (lpthread_attr*, const void*)) { return 0; }
SHIM(int, pthread_attr_getstacksize, (const lpthread_attr* a, size_t* s)) { *s = a->stack ? a->stack : 8 << 20; return 0; }

SHIM(int, pthread_create, (uint64_t* out, const lpthread_attr* attr, SYSV void* (*start)(void*), void* arg)) {
    auto t = new Thread;
    t->start = start;
    t->arg = arg;
    t->detached = attr && attr->detach;
    size_t stack = attr && attr->stack ? attr->stack : (8u << 20);
    if (stack < (1u << 20)) stack = 1u << 20;  // the shims (and Windows) need more headroom than Linux code budgets
    // Fully committed: coroutine switches (src/linux/ucontext.cpp) move the TEB stack bounds, which defeats Windows'
    // grow-on-demand guard page, so game thread stacks never rely on it.
    t->h = CreateThread(nullptr, stack, thread_main, t, CREATE_SUSPENDED, &t->tid);
    if (!t->h) { delete t; return L_EAGAIN; }
    *out = reinterpret_cast<uint64_t>(t);
    ResumeThread(t->h);
    return 0;
}
SHIM(int, pthread_join, (uint64_t th, void** ret)) {
    auto t = reinterpret_cast<Thread*>(th);
    if (!t) return L_ESRCH;
    WaitForSingleObject(t->h, INFINITE);
    if (ret) *ret = t->ret;
    CloseHandle(t->h);
    delete t;
    return 0;
}
SHIM(int, pthread_timedjoin_np, (uint64_t th, void** ret, const ltimespec* abs)) {
    auto t = reinterpret_cast<Thread*>(th);
    if (!t) return L_ESRCH;
    int64_t ms = abs ? (abs->tv_sec * 1000000000ll + abs->tv_nsec - wall_ns()) / 1000000 : INFINITE;
    if (WaitForSingleObject(t->h, ms < 0 ? 0 : DWORD(ms)) == WAIT_TIMEOUT) return L_ETIMEDOUT;
    return lx_pthread_join(th, ret);
}
SHIM(int, pthread_detach, (uint64_t th)) { reinterpret_cast<Thread*>(th)->detached = true; return 0; }
SHIM(uint64_t, pthread_self, ()) { return reinterpret_cast<uint64_t>(self()); }
SHIM(int, pthread_equal, (uint64_t a, uint64_t b)) { return a == b; }
SHIM(void, pthread_exit, (void* ret)) {
    self()->ret = ret;
    run_key_destructors();
    ExitThread(0);
}
SHIM(int, pthread_cancel, (uint64_t)) { return 0; }
SHIM(int, pthread_setcanceltype, (int, int* old)) { if (old) *old = 0; return 0; }
SHIM(int, pthread_setcancelstate, (int, int* old)) { if (old) *old = 0; return 0; }
SHIM(int, pthread_kill, (uint64_t, int)) { return 0; }
SHIM(int, pthread_sigmask, (int, const void*, void*)) { return 0; }
SHIM(int, pthread_setname_np, (uint64_t, const char*)) { return 0; }
SHIM(int, pthread_setschedparam, (uint64_t, int, const void*)) { return 0; }
SHIM(int, pthread_getschedparam, (uint64_t, int* p, int* s)) { if (p) *p = 0; if (s) *s = 0; return 0; }

// ---- mutexes
SHIM(int, pthread_mutexattr_init, (int* a)) { *a = 0; return 0; }
SHIM(int, pthread_mutexattr_destroy, (int*)) { return 0; }
SHIM(int, pthread_mutexattr_settype, (int* a, int t)) { *a = t; return 0; }
SHIM(int, pthread_mutexattr_setpshared, (int*, int)) { return 0; }
SHIM(int, pthread_mutex_init, (uint8_t* m, const int* attr)) {
    std::memset(m, 0, 40);
    *reinterpret_cast<int*>(m + 16) = attr ? *attr : 0;
    return 0;
}
SHIM(int, pthread_mutex_destroy, (uint8_t*)) { return 0; }
SHIM(int, pthread_mutex_lock, (uint8_t* m)) {
    MutexView v{m};
    LONG me = LONG(GetCurrentThreadId());
    if (v.kind() == 1 && v.owner() == me) { v.count()++; return 0; }
    AcquireSRWLockExclusive(v.lock());
    v.owner() = me;
    v.count() = 1;
    return 0;
}
SHIM(int, pthread_mutex_trylock, (uint8_t* m)) {
    MutexView v{m};
    LONG me = LONG(GetCurrentThreadId());
    if (v.kind() == 1 && v.owner() == me) { v.count()++; return 0; }
    if (!TryAcquireSRWLockExclusive(v.lock())) return L_EBUSY;
    v.owner() = me;
    v.count() = 1;
    return 0;
}
SHIM(int, pthread_mutex_unlock, (uint8_t* m)) {
    MutexView v{m};
    if (v.kind() == 1 && --v.count() > 0) return 0;
    v.owner() = 0;
    v.count() = 0;
    ReleaseSRWLockExclusive(v.lock());
    return 0;
}

// ---- condition variables
SHIM(int, pthread_condattr_init, (int* a)) { *a = 0; return 0; }
SHIM(int, pthread_condattr_destroy, (int*)) { return 0; }
SHIM(int, pthread_condattr_setclock, (int* a, int c)) { *a = c; return 0; }
SHIM(int, pthread_cond_init, (uint8_t* c, const int*)) { std::memset(c, 0, 48); return 0; }
SHIM(int, pthread_cond_destroy, (uint8_t*)) { return 0; }
SHIM(int, pthread_cond_signal, (uint8_t* c)) { WakeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(c)); return 0; }
SHIM(int, pthread_cond_broadcast, (uint8_t* c)) { WakeAllConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(c)); return 0; }
static int cond_wait_ms(uint8_t* c, uint8_t* m, DWORD ms) {
    MutexView v{m};
    LONG saved = v.count();
    v.owner() = 0;
    v.count() = 0;
    BOOL ok = SleepConditionVariableSRW(reinterpret_cast<CONDITION_VARIABLE*>(c), v.lock(), ms, 0);
    v.owner() = LONG(GetCurrentThreadId());
    v.count() = saved;
    return ok ? 0 : (GetLastError() == ERROR_TIMEOUT ? L_ETIMEDOUT : L_EINVAL);
}
SHIM(int, pthread_cond_wait, (uint8_t* c, uint8_t* m)) { return cond_wait_ms(c, m, INFINITE); }
SHIM(int, pthread_cond_timedwait, (uint8_t* c, uint8_t* m, const ltimespec* abs)) {
    // CLOCK_REALTIME deadline (the game never sets a monotonic condattr clock that matters here)
    int64_t ns = abs->tv_sec * 1000000000ll + abs->tv_nsec - wall_ns();
    DWORD ms = ns <= 0 ? 0 : DWORD((ns + 999999) / 1000000);
    return cond_wait_ms(c, m, ms);
}

// ---- rwlocks
SHIM(int, pthread_rwlock_init, (uint8_t* l, const void*)) { std::memset(l, 0, 56); return 0; }
SHIM(int, pthread_rwlock_destroy, (uint8_t*)) { return 0; }
SHIM(int, pthread_rwlock_rdlock, (uint8_t* l)) { AcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(l)); return 0; }
SHIM(int, pthread_rwlock_wrlock, (uint8_t* l)) {
    AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(l));
    *reinterpret_cast<DWORD*>(l + 8) = GetCurrentThreadId();
    return 0;
}
SHIM(int, pthread_rwlock_tryrdlock, (uint8_t* l)) { return TryAcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(l)) ? 0 : L_EBUSY; }
SHIM(int, pthread_rwlock_trywrlock, (uint8_t* l)) {
    if (!TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(l))) return L_EBUSY;
    *reinterpret_cast<DWORD*>(l + 8) = GetCurrentThreadId();
    return 0;
}
SHIM(int, pthread_rwlock_unlock, (uint8_t* l)) {
    auto& w = *reinterpret_cast<DWORD*>(l + 8);
    if (w == GetCurrentThreadId()) {
        w = 0;
        ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(l));
    } else {
        ReleaseSRWLockShared(reinterpret_cast<SRWLOCK*>(l));
    }
    return 0;
}

// ---- once / keys
SHIM(int, pthread_once, (std::atomic<int>* once, SYSV void (*fn)())) {
    int s = once->load(std::memory_order_acquire);
    if (s == 2) return 0;
    int expected = 0;
    if (once->compare_exchange_strong(expected, 1)) {
        fn();
        once->store(2, std::memory_order_release);
        WakeByAddressAll(once);
        return 0;
    }
    while ((s = once->load(std::memory_order_acquire)) != 2) WaitOnAddress(once, &s, sizeof s, INFINITE);
    return 0;
}
SHIM(int, pthread_key_create, (unsigned* key, SYSV void (*dtor)(void*))) {
    std::lock_guard<std::mutex> lk(g_keys_mutex);
    for (int k = 0; k < kMaxKeys; k++)
        if (!g_keys[k].used) {
            g_keys[k] = {true, dtor};
            *key = unsigned(k);
            return 0;
        }
    return L_EAGAIN;
}
SHIM_ALIAS(__pthread_key_create, pthread_key_create);
SHIM(int, pthread_key_delete, (unsigned k)) {
    std::lock_guard<std::mutex> lk(g_keys_mutex);
    if (k < kMaxKeys) g_keys[k].used = false;
    return 0;
}
SHIM(void*, pthread_getspecific, (unsigned k)) { return k < kMaxKeys ? t_key_values[k] : nullptr; }
SHIM(int, pthread_setspecific, (unsigned k, const void* v)) {
    if (k >= kMaxKeys) return L_EINVAL;
    t_key_values[k] = const_cast<void*>(v);
    return 0;
}

// ---- semaphores
SHIM(int, sem_init, (HANDLE* s, int, unsigned value)) {
    *s = CreateSemaphoreW(nullptr, LONG(value), 0x7fffffff, nullptr);
    return *s ? 0 : fail(L_ENOMEM);
}
SHIM(int, sem_destroy, (HANDLE* s)) { CloseHandle(*s); return 0; }
SHIM(int, sem_post, (HANDLE* s)) { ReleaseSemaphore(*s, 1, nullptr); return 0; }
SHIM(int, sem_wait, (HANDLE* s)) { WaitForSingleObject(*s, INFINITE); return 0; }
SHIM(int, sem_trywait, (HANDLE* s)) { return WaitForSingleObject(*s, 0) == WAIT_OBJECT_0 ? 0 : fail(L_EAGAIN); }
SHIM(int, sem_timedwait, (HANDLE* s, const ltimespec* abs)) {
    int64_t ns = abs->tv_sec * 1000000000ll + abs->tv_nsec - wall_ns();
    DWORD ms = ns <= 0 ? 0 : DWORD((ns + 999999) / 1000000);
    return WaitForSingleObject(*s, ms) == WAIT_OBJECT_0 ? 0 : fail(L_ETIMEDOUT);
}
SHIM(int, sem_getvalue, (HANDLE* s, int* v)) {
    LONG prev = 0;
    if (WaitForSingleObject(*s, 0) == WAIT_OBJECT_0) {
        ReleaseSemaphore(*s, 1, &prev);
        *v = int(prev + 1);
    } else {
        *v = 0;
    }
    return 0;
}
// Named semaphores: sem_open returns a sem_t* we allocate (the game only uses them for its single-instance check).
SHIM(HANDLE*, sem_open, (const char* name, int oflag, int mode, unsigned value)) {
    std::wstring w = L"Local\\raven_sem";
    for (const char* p = name; *p; p++) w += wchar_t(*p == '/' ? '_' : *p);
    HANDLE h = CreateSemaphoreW(nullptr, (oflag & L_O_CREAT) ? LONG(value) : 0, 0x7fffffff, w.c_str());
    if (!h) { fail(L_ENOENT); return reinterpret_cast<HANDLE*>(0); }
    auto s = static_cast<HANDLE*>(lx_malloc(32));
    *s = h;
    return s;
}
SHIM(int, sem_close, (HANDLE* s)) { CloseHandle(*s); lx_free(s); return 0; }
SHIM(int, sem_unlink, (const char*)) { return 0; }

SHIM(int, sched_yield, ()) { SwitchToThread(); return 0; }
SHIM(int, sched_getaffinity, (int, size_t n, uint8_t* set)) {
    std::memset(set, 0, n);
    DWORD_PTR proc, sys;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    std::memcpy(set, &proc, std::min<size_t>(n, sizeof proc));
    return 0;
}
SHIM(int, sched_setaffinity, (int, size_t, const void*)) { return 0; }
SHIM(int, get_nprocs, ()) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return int(si.dwNumberOfProcessors);
}
SHIM_ALIAS(get_nprocs_conf, get_nprocs);
