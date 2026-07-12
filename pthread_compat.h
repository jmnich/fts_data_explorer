#pragma once

// Ensure pthread_cond_clockwait/pthread_mutex_clocklock are declared before
// <future> which includes <mutex> (referenced by C++ stdlib on glibc >= 2.30).
// These are __USE_GNU functions, hidden when _GNU_SOURCE is undefined.
// Implementations are in glibc_compat.cpp (falling back to basic versions).

#include <pthread.h>

#ifndef pthread_cond_clockwait
extern "C" int pthread_cond_clockwait(pthread_cond_t *, pthread_mutex_t *, clockid_t, const struct timespec *);
#endif

#ifndef pthread_mutex_clocklock
extern "C" int pthread_mutex_clocklock(pthread_mutex_t *, clockid_t, const struct timespec *);
#endif
