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

/*  pthread.h's functions on Windows' slim locks (namespace draft 51, ACT). */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <sys/timeb.h>
#include <stdint.h>

#include "pthread.h"

typedef struct
{
    void* (*start) (void*);
    void* arg;
} wfg_thread_start;

static unsigned __stdcall wfg_thread_main (void* p)
{
    wfg_thread_start s = *(wfg_thread_start*) p;
    HeapFree (GetProcessHeap(), 0, p);
    s.start (s.arg);
    return 0;
}

int pthread_create (pthread_t* thread, const pthread_attr_t* attr, void* (*start) (void*), void* arg)
{
    unsigned id = 0;
    uintptr_t handle;
    wfg_thread_start* s = (wfg_thread_start*) HeapAlloc (GetProcessHeap(), 0, sizeof (*s));
    (void) attr;
    if (s == NULL)
        return EAGAIN;
    s->start = start;
    s->arg = arg;
    handle = _beginthreadex (NULL, 0, wfg_thread_main, s, 0, &id);
    if (handle == 0)
    {
        HeapFree (GetProcessHeap(), 0, s);
        return EAGAIN;
    }
    thread->handle = (void*) handle;
    thread->id = id;
    return 0;
}

int pthread_join (pthread_t thread, void** result)
{
    if (result != NULL)
        *result = NULL;
    if (thread.handle == NULL)
        return EINVAL;
    WaitForSingleObject ((HANDLE) thread.handle, INFINITE);
    CloseHandle ((HANDLE) thread.handle);
    return 0;
}

pthread_t pthread_self (void)
{
    pthread_t t;
    t.handle = NULL;
    t.id = GetCurrentThreadId();
    return t;
}

int pthread_equal (pthread_t a, pthread_t b)
{
    return a.id == b.id;
}

/*  Pd asks for a real-time policy only on macOS; Go.dot sets its own threads'
    priorities, and a patch's thread is not one Pd's scheduler runs. */
int pthread_setschedparam (pthread_t thread, int policy, const struct sched_param* param)
{
    (void) thread;
    (void) policy;
    (void) param;
    return 0;
}

int pthread_mutex_init (pthread_mutex_t* mutex, const pthread_mutexattr_t* attr)
{
    (void) attr;
    InitializeSRWLock ((PSRWLOCK) &mutex->lock);
    return 0;
}

int pthread_mutex_destroy (pthread_mutex_t* mutex)
{
    (void) mutex;
    return 0;
}

int pthread_mutex_lock (pthread_mutex_t* mutex)
{
    AcquireSRWLockExclusive ((PSRWLOCK) &mutex->lock);
    return 0;
}

int pthread_mutex_trylock (pthread_mutex_t* mutex)
{
    return TryAcquireSRWLockExclusive ((PSRWLOCK) &mutex->lock) ? 0 : EBUSY;
}

int pthread_mutex_unlock (pthread_mutex_t* mutex)
{
    ReleaseSRWLockExclusive ((PSRWLOCK) &mutex->lock);
    return 0;
}

int pthread_cond_init (pthread_cond_t* cond, const pthread_condattr_t* attr)
{
    (void) attr;
    InitializeConditionVariable ((PCONDITION_VARIABLE) &cond->cond);
    return 0;
}

int pthread_cond_destroy (pthread_cond_t* cond)
{
    (void) cond;
    return 0;
}

int pthread_cond_signal (pthread_cond_t* cond)
{
    WakeConditionVariable ((PCONDITION_VARIABLE) &cond->cond);
    return 0;
}

int pthread_cond_broadcast (pthread_cond_t* cond)
{
    WakeAllConditionVariable ((PCONDITION_VARIABLE) &cond->cond);
    return 0;
}

int pthread_cond_wait (pthread_cond_t* cond, pthread_mutex_t* mutex)
{
    SleepConditionVariableSRW ((PCONDITION_VARIABLE) &cond->cond, (PSRWLOCK) &mutex->lock, INFINITE, 0);
    return 0;
}

/*  POSIX gives the moment to wake on the wall clock; Windows wants how long. */
int pthread_cond_timedwait (pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* until)
{
    struct __timeb64 now;
    long long nowMs, untilMs;
    DWORD waitMs;
    _ftime64_s (&now);
    nowMs = now.time * 1000LL + now.millitm;
    untilMs = (long long) until->tv_sec * 1000LL + until->tv_nsec / 1000000L;
    waitMs = untilMs > nowMs ? (DWORD) (untilMs - nowMs) : 0;
    if (! SleepConditionVariableSRW ((PCONDITION_VARIABLE) &cond->cond, (PSRWLOCK) &mutex->lock, waitMs, 0))
        return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
    return 0;
}

int pthread_rwlock_init (pthread_rwlock_t* lock, const pthread_rwlockattr_t* attr)
{
    (void) attr;
    InitializeSRWLock ((PSRWLOCK) &lock->lock);
    lock->writer = 0;
    return 0;
}

int pthread_rwlock_destroy (pthread_rwlock_t* lock)
{
    (void) lock;
    return 0;
}

int pthread_rwlock_rdlock (pthread_rwlock_t* lock)
{
    AcquireSRWLockShared ((PSRWLOCK) &lock->lock);
    return 0;
}

int pthread_rwlock_tryrdlock (pthread_rwlock_t* lock)
{
    return TryAcquireSRWLockShared ((PSRWLOCK) &lock->lock) ? 0 : EBUSY;
}

int pthread_rwlock_wrlock (pthread_rwlock_t* lock)
{
    AcquireSRWLockExclusive ((PSRWLOCK) &lock->lock);
    InterlockedExchange (&lock->writer, (long) GetCurrentThreadId());
    return 0;
}

/*  POSIX unlocks whichever way the lock is held; an SRWLOCK must be told. The
    thread holding it exclusively is the only one that can find its own id in
    `writer`, so a reader asking is never fooled. */
int pthread_rwlock_unlock (pthread_rwlock_t* lock)
{
    if (InterlockedCompareExchange (&lock->writer, 0, (long) GetCurrentThreadId()) == (long) GetCurrentThreadId())
        ReleaseSRWLockExclusive ((PSRWLOCK) &lock->lock);
    else
        ReleaseSRWLockShared ((PSRWLOCK) &lock->lock);
    return 0;
}
