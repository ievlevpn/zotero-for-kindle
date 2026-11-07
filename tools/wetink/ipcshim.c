// Research tool, not built: LD_PRELOAD logger for SysV IPC (see docs/scribe-pen.md).
// LD_PRELOAD logger for SysV IPC: msgget/msgsnd/msgrcv/shmget/shmat/ftok -> /mnt/us/wetink-trace.log
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static FILE* out;

static void logf_(const char* fmt, ...) {
    pthread_mutex_lock(&mu);
    if (!out) out = fopen("/mnt/us/wetink-trace.log", "a");
    if (out) {
        struct timeval tv;
        gettimeofday(&tv, 0);
        fprintf(out, "%ld.%03ld [%d] ", (long)tv.tv_sec, (long)tv.tv_usec / 1000, getpid());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(out, fmt, ap);
        va_end(ap);
        fflush(out);
    }
    pthread_mutex_unlock(&mu);
}

static void hex(const char* tag, const void* p, size_t n) {
    char buf[3 * 512 + 1];
    size_t m = n > 512 ? 512 : n, k = 0;
    for (size_t i = 0; i < m; ++i) k += sprintf(buf + k, "%02x%s", ((const unsigned char*)p)[i], (i % 4 == 3) ? " " : "");
    buf[k] = 0;
    logf_("%s len=%zu: %s\n", tag, n, buf);
}

#define REAL(name) static __typeof__(name)* real_##name; if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name)

key_t ftok(const char* path, int id) {
    REAL(ftok);
    key_t k = real_ftok(path, id);
    logf_("ftok(%s, %d) = 0x%x\n", path, id, k);
    return k;
}
int msgget(key_t key, int flg) {
    REAL(msgget);
    int r = real_msgget(key, flg);
    logf_("msgget(0x%x, 0%o) = %d\n", key, flg, r);
    return r;
}
int msgsnd(int id, const void* p, size_t n, int flg) {
    REAL(msgsnd);
    logf_("msgsnd(q=%d, type=%ld, flg=%d)\n", id, *(const long*)p, flg);
    hex("  snd", (const char*)p + sizeof(long), n);
    return real_msgsnd(id, p, n, flg);
}
ssize_t msgrcv(int id, void* p, size_t n, long type, int flg) {
    REAL(msgrcv);
    ssize_t r = real_msgrcv(id, p, n, type, flg);
    if (r >= 0) {
        logf_("msgrcv(q=%d, want=%ld, max=%zu) -> type=%ld\n", id, type, n, *(long*)p);
        hex("  rcv", (char*)p + sizeof(long), (size_t)r);
    }
    return r;
}
int shmget(key_t key, size_t size, int flg) {
    REAL(shmget);
    int r = real_shmget(key, size, flg);
    logf_("shmget(0x%x, %zu, 0%o) = %d\n", key, size, flg, r);
    return r;
}
void* shmat(int id, const void* a, int flg) {
    REAL(shmat);
    void* r = real_shmat(id, a, flg);
    logf_("shmat(%d) = %p\n", id, r);
    return r;
}

int semget(key_t key, int n, int flg) {
    REAL(semget);
    int r = real_semget(key, n, flg);
    logf_("semget(0x%x, %d, 0%o) = %d\n", key, n, flg, r);
    return r;
}
static void logops(const char* tag, int id, struct sembuf* ops, size_t n, int r) {
    char buf[256];
    size_t k = 0;
    for (size_t i = 0; i < n && k < 200; ++i)
        k += sprintf(buf + k, " [%d %+d %s]", ops[i].sem_num, ops[i].sem_op, (ops[i].sem_flg & IPC_NOWAIT) ? "nw" : "");
    buf[k] = 0;
    logf_("%s(%d)%s = %d\n", tag, id, buf, r);
}
int semop(int id, struct sembuf* ops, size_t n) {
    REAL(semop);
    logops("semop-begin", id, ops, n, 0);
    int r = real_semop(id, ops, n);
    logops("semop-end", id, ops, n, r);
    return r;
}
int semtimedop(int id, struct sembuf* ops, size_t n, const struct timespec* t) {
    REAL(semtimedop);
    int r = real_semtimedop(id, ops, n, t);
    logops("semtimedop", id, ops, n, r);
    return r;
}
int semctl(int id, int num, int cmd, ...) {
    REAL(semctl);
    va_list ap;
    va_start(ap, cmd);
    unsigned long arg = va_arg(ap, unsigned long);
    va_end(ap);
    int r = real_semctl(id, num, cmd, arg);
    logf_("semctl(%d, %d, cmd=%d, arg=%lu) = %d\n", id, num, cmd, arg, r);
    return r;
}
int shmdt(const void* a) {
    REAL(shmdt);
    hex("  shm-at-detach", a, 64);
    return real_shmdt(a);
}
