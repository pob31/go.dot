/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

/*
    THE PART OF POSIX THREADS PURE DATA USES, FOR MSVC (namespace draft 51, ACT).

    Pure Data takes its locks and threads from <pthread.h>, which Windows' own
    compiler has not got; libpd's build asks for pthreads4w. This is the part Pd
    calls and nothing more: a mutex and a read-write lock are SRWLOCKs, a
    condition a CONDITION_VARIABLE, a thread _beginthreadex. Only Pd's sources
    see this directory on their include path, and only on MSVC.

    The header names no Windows type, so it may come before or after winsock2.h
    in any of Pd's files: each lock is one pointer, which is what Windows' are,
    and a zero is their static initialiser (SRWLOCK_INIT and
    CONDITION_VARIABLE_INIT are both a null pointer).
*/

#ifndef WFG_PTHREAD_WIN32_H
#define WFG_PTHREAD_WIN32_H

#include <errno.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { void* handle; unsigned long id; } pthread_t;
typedef struct { int unused; } pthread_attr_t;
typedef struct { int unused; } pthread_mutexattr_t;
typedef struct { int unused; } pthread_condattr_t;
typedef struct { int unused; } pthread_rwlockattr_t;
typedef struct { void* lock; } pthread_mutex_t;
typedef struct { void* cond; } pthread_cond_t;
typedef struct { void* lock; volatile long writer; } pthread_rwlock_t;

struct sched_param { int sched_priority; };

#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER { 0 }
#define PTHREAD_RWLOCK_INITIALIZER { 0, 0 }
#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2

int pthread_create (pthread_t* thread, const pthread_attr_t* attr, void* (*start) (void*), void* arg);
int pthread_join (pthread_t thread, void** result);
pthread_t pthread_self (void);
int pthread_equal (pthread_t a, pthread_t b);
int pthread_setschedparam (pthread_t thread, int policy, const struct sched_param* param);

int pthread_mutex_init (pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int pthread_mutex_destroy (pthread_mutex_t* mutex);
int pthread_mutex_lock (pthread_mutex_t* mutex);
int pthread_mutex_trylock (pthread_mutex_t* mutex);
int pthread_mutex_unlock (pthread_mutex_t* mutex);

int pthread_cond_init (pthread_cond_t* cond, const pthread_condattr_t* attr);
int pthread_cond_destroy (pthread_cond_t* cond);
int pthread_cond_signal (pthread_cond_t* cond);
int pthread_cond_broadcast (pthread_cond_t* cond);
int pthread_cond_wait (pthread_cond_t* cond, pthread_mutex_t* mutex);
int pthread_cond_timedwait (pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* until);

int pthread_rwlock_init (pthread_rwlock_t* lock, const pthread_rwlockattr_t* attr);
int pthread_rwlock_destroy (pthread_rwlock_t* lock);
int pthread_rwlock_rdlock (pthread_rwlock_t* lock);
int pthread_rwlock_tryrdlock (pthread_rwlock_t* lock);
int pthread_rwlock_wrlock (pthread_rwlock_t* lock);
int pthread_rwlock_unlock (pthread_rwlock_t* lock);

#ifdef __cplusplus
}
#endif

#endif
