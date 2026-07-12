// glibc_compat.cpp - Binary compatibility with older glibc via --wrap + .symver
// Bindings redirect newer GLIBC symbol versions (2.32+) to GLIBC_2.2.5 equivalents.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

// ---- __libc_single_threaded (GLIBC_2.32) ----

extern "C" { char __libc_single_threaded = 1; }

// ---- arc4random (GLIBC_2.36) ----
// Called by static libstdc++ for std::random_device on glibc 2.36+.
// Provide a fallback using standard rand() which is GLIBC_2.2.5.

extern "C" unsigned int arc4random(void)
{
    static int seeded = 0;
    if (!seeded) { std::srand(static_cast<unsigned>(std::time(nullptr))); seeded = 1; }
    unsigned int a = static_cast<unsigned int>(std::rand()) << 16;
    unsigned int b = static_cast<unsigned int>(std::rand()) & 0xFFFF;
    return a | b;
}

// ---- __libc_start_main (GLIBC_2.34) ----

extern "C" int __libc_start_main_old(int (*main)(int, char**, char**),
                                     int argc, char** argv,
                                     void (*init)(void), void (*fini)(void),
                                     void (*rtld_fini)(void), void* stack_end);
__asm__(".symver __libc_start_main_old, __libc_start_main@GLIBC_2.2.5");

extern "C" int __wrap___libc_start_main(int (*main)(int, char**, char**),
                                        int argc, char** argv,
                                        void (*init)(void), void (*fini)(void),
                                        void (*rtld_fini)(void), void* stack_end)
{
    return __libc_start_main_old(main, argc, argv, init, fini, rtld_fini, stack_end);
}

// ---- Math functions @ GLIBC_2.2.5 instead of GLIBC_2.43 / GLIBC_2.38 ----

#define WRAP_MATH1(name)                                                       \
    extern "C" float name##_old(float);                                        \
    __asm__(".symver " #name "_old, " #name "@GLIBC_2.2.5");                  \
    extern "C" float __wrap_##name(float x) { return name##_old(x); }

WRAP_MATH1(sqrtf)
WRAP_MATH1(acosf)

#define WRAP_MATH2(name)                                                       \
    extern "C" float name##_old(float, float);                                 \
    __asm__(".symver " #name "_old, " #name "@GLIBC_2.2.5");                  \
    extern "C" float __wrap_##name(float x, float y) { return name##_old(x, y); }

WRAP_MATH2(atan2f)
WRAP_MATH2(fmodf)
WRAP_MATH1(logf)
WRAP_MATH2(powf)

// ---- Double-precision math @ GLIBC_2.2.5 instead of GLIBC_2.29 ----

#define WRAP_MATH_D1(name)                                                     \
    extern "C" double name##_old(double);                                      \
    __asm__(".symver " #name "_old, " #name "@GLIBC_2.2.5");                  \
    extern "C" double __wrap_##name(double x) { return name##_old(x); }

#define WRAP_MATH_D2(name)                                                     \
    extern "C" double name##_old(double, double);                              \
    __asm__(".symver " #name "_old, " #name "@GLIBC_2.2.5");                  \
    extern "C" double __wrap_##name(double x, double y) { return name##_old(x, y); }

WRAP_MATH_D1(exp)
WRAP_MATH_D1(log)
WRAP_MATH_D1(log2)
WRAP_MATH_D2(pow)

// ---- getentropy @ GLIBC_2.2.5 instead of GLIBC_2.25 ----
// Use the getrandom syscall directly, wrapped in our own implementation.

#include <unistd.h>
#include <sys/syscall.h>

extern "C" int getentropy(void *buffer, size_t length)
{
    if (length > 256)
        return -1;
    unsigned char *buf = static_cast<unsigned char *>(buffer);
    size_t total = 0;
    while (total < length) {
        ssize_t ret = syscall(SYS_getrandom, buf + total, length - total, 0);
        if (ret == -1)
            return -1;
        total += static_cast<size_t>(ret);
    }
    return 0;
}

#include <pthread.h>

// ---- pthread functions @ GLIBC_2.2.5 instead of GLIBC_2.34 ----

extern "C" int pthread_key_create_old(unsigned int* key, void (*destructor)(void*));
__asm__(".symver pthread_key_create_old, pthread_key_create@GLIBC_2.2.5");
extern "C" int __wrap_pthread_key_create(unsigned int* key, void (*destructor)(void*))
    { return pthread_key_create_old(key, destructor); }

extern "C" int pthread_key_delete_old(unsigned int key);
__asm__(".symver pthread_key_delete_old, pthread_key_delete@GLIBC_2.2.5");
extern "C" int __wrap_pthread_key_delete(unsigned int key)
    { return pthread_key_delete_old(key); }

extern "C" void* pthread_getspecific_old(unsigned int key);
__asm__(".symver pthread_getspecific_old, pthread_getspecific@GLIBC_2.2.5");
extern "C" void* __wrap_pthread_getspecific(unsigned int key)
    { return pthread_getspecific_old(key); }

extern "C" int pthread_setspecific_old(unsigned int key, const void* value);
__asm__(".symver pthread_setspecific_old, pthread_setspecific@GLIBC_2.2.5");
extern "C" int __wrap_pthread_setspecific(unsigned int key, const void* value)
    { return pthread_setspecific_old(key, value); }

extern "C" int pthread_once_old(int* once_control, void (*init_routine)(void));
__asm__(".symver pthread_once_old, pthread_once@GLIBC_2.2.5");
extern "C" int __wrap_pthread_once(int* once_control, void (*init_routine)(void))
    { return pthread_once_old(once_control, init_routine); }

// ---- dl functions @ GLIBC_2.2.5 instead of GLIBC_2.34 ----

extern "C" void* dlopen_old(const char* filename, int flags);
__asm__(".symver dlopen_old, dlopen@GLIBC_2.2.5");
extern "C" void* __wrap_dlopen(const char* filename, int flags)
    { return dlopen_old(filename, flags); }

extern "C" void* dlsym_old(void* handle, const char* symbol);
__asm__(".symver dlsym_old, dlsym@GLIBC_2.2.5");
extern "C" void* __wrap_dlsym(void* handle, const char* symbol)
    { return dlsym_old(handle, symbol); }

extern "C" int dlclose_old(void* handle);
__asm__(".symver dlclose_old, dlclose@GLIBC_2.2.5");
extern "C" int __wrap_dlclose(void* handle)
    { return dlclose_old(handle); }

// ---- C23 scanf / strtoul functions @ GLIBC_2.2.5 instead of GLIBC_2.38 ----
// Safety-net wraps, though -std=gnu11 + -U_GNU_SOURCE should prevent these.

extern "C" int __wrap___isoc23_sscanf(const char *s, const char *fmt, ...)
{
    std::va_list args;
    va_start(args, fmt);
    int ret = std::vsscanf(s, fmt, args);
    va_end(args);
    return ret;
}

extern "C" unsigned long strtoul_old(const char *nptr, char **endptr, int base);
__asm__(".symver strtoul_old, strtoul@GLIBC_2.2.5");

extern "C" unsigned long __wrap___isoc23_strtoul(const char *nptr, char **endptr, int base)
{
    return strtoul_old(nptr, endptr, base);
}

// ---- stat/lstat/fstat @ GLIBC_2.2.5 (via __xstat64) instead of GLIBC_2.33 ----

#include <sys/stat.h>

extern "C" int __xstat64_old(int ver, const char* path, struct stat* buf);
__asm__(".symver __xstat64_old, __xstat64@GLIBC_2.2.5");

extern "C" int stat(const char* path, struct stat* buf)
    { return __xstat64_old(1, path, buf); }

extern "C" int __lxstat64_old(int ver, const char* path, struct stat* buf);
__asm__(".symver __lxstat64_old, __lxstat64@GLIBC_2.2.5");

extern "C" int lstat(const char* path, struct stat* buf)
    { return __lxstat64_old(1, path, buf); }

extern "C" int __fxstat64_old(int ver, int fd, struct stat* buf);
__asm__(".symver __fxstat64_old, __fxstat64@GLIBC_2.2.5");

extern "C" int fstat(int fd, struct stat* buf)
    { return __fxstat64_old(1, fd, buf); }

// ---- pthread_cond_clockwait / pthread_mutex_clocklock (GLIBC_2.30) ----
// The C++ stdlib uses them internally but they aren't declared when
// _GNU_SOURCE is undefined. Provide declarations + fallback implementations
// that delegate to the basic versions.
// These are only used internally by std::mutex (not by our ThreadPool).

extern "C" int pthread_mutex_clocklock(pthread_mutex_t *mutex,
                                       clockid_t clk_id,
                                       const struct timespec *abstime)
{
    (void)clk_id;
    return pthread_mutex_timedlock(mutex, abstime);
}

extern "C" int pthread_cond_clockwait(pthread_cond_t *cond,
                                       pthread_mutex_t *mutex,
                                       clockid_t clk_id,
                                       const struct timespec *abstime)
{
    (void)clk_id;
    return pthread_cond_timedwait(cond, mutex, abstime);
}
