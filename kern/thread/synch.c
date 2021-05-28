/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Synchronization primitives.
 * The specifications of the functions are in synch.h.
 */

#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <wchan.h>
#include <thread.h>
#include <current.h>
#include <synch.h>

#include "opt-lock_with_semaphores.h"
#include "opt-lock_wchan_spinlock.h"
#include "opt-cv_implementation.h"

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *name, unsigned initial_count)
{
        struct semaphore *sem;

        sem = kmalloc(sizeof(*sem));
        if (sem == NULL) {
                return NULL;
        }

        sem->sem_name = kstrdup(name);
        if (sem->sem_name == NULL) {
                kfree(sem);
                return NULL;
        }

	sem->sem_wchan = wchan_create(sem->sem_name);
	if (sem->sem_wchan == NULL) {
		kfree(sem->sem_name);
		kfree(sem);
		return NULL;
	}

	spinlock_init(&sem->sem_lock);
        sem->sem_count = initial_count;

        return sem;
}

void
sem_destroy(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	/* wchan_cleanup will assert if anyone's waiting on it */
	spinlock_cleanup(&sem->sem_lock);
	wchan_destroy(sem->sem_wchan);
        kfree(sem->sem_name);
        kfree(sem);
}

void
P(struct semaphore *sem)
{
        KASSERT(sem != NULL);

        /*
         * May not block in an interrupt handler.
         *
         * For robustness, always check, even if we can actually
         * complete the P without blocking.
         */
        KASSERT(curthread->t_in_interrupt == false);

	/* Use the semaphore spinlock to protect the wchan as well. */
	spinlock_acquire(&sem->sem_lock);
        while (sem->sem_count == 0) {
		/*
		 *
		 * Note that we don't maintain strict FIFO ordering of
		 * threads going through the semaphore; that is, we
		 * might "get" it on the first try even if other
		 * threads are waiting. Apparently according to some
		 * textbooks semaphores must for some reason have
		 * strict ordering. Too bad. :-)
		 *
		 * Exercise: how would you implement strict FIFO
		 * ordering?
		 */
		wchan_sleep(sem->sem_wchan, &sem->sem_lock);
        }
        KASSERT(sem->sem_count > 0);
        sem->sem_count--;
	spinlock_release(&sem->sem_lock);
}

void
V(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	spinlock_acquire(&sem->sem_lock);

        sem->sem_count++;
        KASSERT(sem->sem_count > 0);
	wchan_wakeone(sem->sem_wchan, &sem->sem_lock);

	spinlock_release(&sem->sem_lock);
}

////////////////////////////////////////////////////////////
//
// Lock.

struct lock *
lock_create(const char *name)
{
        struct lock *lock;

        lock = kmalloc(sizeof(*lock));
        if (lock == NULL) {
                return NULL;
        }

        lock->lk_name = kstrdup(name);
        if (lock->lk_name == NULL) {
                kfree(lock);
                return NULL;
        }

	HANGMAN_LOCKABLEINIT(&lock->lk_hangman, lock->lk_name);

        // add stuff here as needed
#if OPT_LOCK_WITH_SEMAPHORES || OPT_LOCK_WCHAN_SPINLOCK

        // initially no one is holding the lock
        lock->owner = NULL;

        // initialize the spinlock
        spinlock_init(&lock->spinlock);

#if OPT_LOCK_WITH_SEMAPHORES
        // initialize the binary semaphore
        lock->sem = sem_create(name, 1);
        if (lock->sem == NULL)
        {
                kfree(lock->lk_name);
                kfree(lock);
                return NULL;
        }
#else
        // initialize the wait channel
        lock->lk_wchan = wchan_create(lock->lk_name);
	if (lock->lk_wchan == NULL) {
		kfree(lock->lk_name);
		kfree(lock);
		return NULL;
	}
#endif

#endif

        return lock;
}

void
lock_destroy(struct lock *lock)
{
        KASSERT(lock != NULL);

        // add stuff here as needed
#if OPT_LOCK_WITH_SEMAPHORES || OPT_LOCK_WCHAN_SPINLOCK

        if (lock->owner != NULL)
        {
                panic("Called lock_destroy on an acquired lock");
                return;
        }

#if OPT_LOCK_WITH_SEMAPHORES
        sem_destroy(lock->sem);
#else
        wchan_destroy(lock->lk_wchan);
#endif
        spinlock_cleanup(&lock->spinlock);
        kfree(&lock->spinlock);
#endif

        kfree(lock->lk_name);
        kfree(lock);
}

void
lock_acquire(struct lock *lock)
{

#if OPT_LOCK_WITH_SEMAPHORES || OPT_LOCK_WCHAN_SPINLOCK

        // do not block inside interrupts
        KASSERT(curthread->t_in_interrupt == false);

        // veriy i do not hold the lock
        KASSERT(!(lock_do_i_hold(lock)));

#if OPT_LOCK_WITH_SEMAPHORES
        P(lock->sem);

        // acquire the spinlock and modify the owner thread
        spinlock_acquire(&lock->spinlock);
        lock->owner = curthread;
        spinlock_release(&lock->spinlock);
#else
        // acquire the spinlock and wait
        spinlock_acquire(&lock->spinlock);
        while (lock->owner) {
                wchan_sleep(lock->lk_wchan, &lock->spinlock);
        }
        lock->owner = curthread;
        spinlock_release(&lock->spinlock);
#endif

#else
    (void)lock;
#endif
}

void
lock_release(struct lock *lock)
{
#if OPT_LOCK_WITH_SEMAPHORES || OPT_LOCK_WCHAN_SPINLOCK
        spinlock_acquire(&lock->spinlock);
        
        if (lock->owner != curthread) {
                spinlock_release(&lock->spinlock);
                panic("How dare you?!");
                return;
        }

#if OPT_LOCK_WITH_SEMAPHORES
        // release the semaphore
        V(lock->sem);
#else
        wchan_wakeone(lock->lk_wchan, &lock->spinlock);
#endif

        // set owner to NULL
        lock->owner = NULL;

        spinlock_release(&lock->spinlock);
#else
    (void) lock;
#endif
}

bool
lock_do_i_hold(struct lock *lock)
{
#if OPT_LOCK_WITH_SEMAPHORES || OPT_LOCK_WCHAN_SPINLOCK
        bool own = false;

        spinlock_acquire(&lock->spinlock);
        own = lock->owner == curthread;
        spinlock_release(&lock->spinlock);

        return own;
#else

    (void)lock;  // suppress warning until code gets written

    return true; // dummy until code gets written
#endif
}

////////////////////////////////////////////////////////////
//
// CV


struct cv *
cv_create(const char *name)
{
        struct cv *cv;

        cv = kmalloc(sizeof(*cv));
        if (cv == NULL) {
                return NULL;
        }

        cv->cv_name = kstrdup(name);
        if (cv->cv_name==NULL) {
                kfree(cv);
                return NULL;
        }

#if (OPT_LOCK_WITH_SEMAPHORES || OPT_LOCK_WCHAN_SPINLOCK) && OPT_CV_IMPLEMENTATION
        // initialize the spinlock
        spinlock_init(&cv->spinlock);

        cv->cv_wchan = wchan_create(cv->cv_name);
	if (cv->cv_wchan == NULL) {
		kfree(cv->cv_name);
		kfree(cv);
		return NULL;
	}
#endif

        return cv;
}

void
cv_destroy(struct cv *cv)
{
        KASSERT(cv != NULL);

#if (OPT_LOCK_WITH_SEMAPHORES || OPT_LOCK_WCHAN_SPINLOCK) && OPT_CV_IMPLEMENTATION

        wchan_destroy(cv->cv_wchan);
        spinlock_cleanup(&cv->spinlock);
        
#endif

        kfree(cv->cv_name);
        kfree(cv);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{

#if (OPT_LOCK_WITH_SEMAPHORES || OPT_LOCK_WCHAN_SPINLOCK) && OPT_CV_IMPLEMENTATION
        // verify current thread is holding the lock
        KASSERT(lock_do_i_hold(lock));

        // acquire the spinlock
        spinlock_acquire(&cv->spinlock);

        // release the lock and put curthread to sleep
        lock_release(lock);
        wchan_sleep(cv->cv_wchan, &cv->spinlock);
        spinlock_release(&cv->spinlock);

        // reacquire the lock
        lock_acquire(lock);

#else
        // Write this
        (void)cv;    // suppress warning until code gets written
        (void)lock;  // suppress warning until code gets written

#endif
}

void
cv_signal(struct cv *cv, struct lock *lock)
{

#if (OPT_LOCK_WITH_SEMAPHORES || OPT_LOCK_WCHAN_SPINLOCK) && OPT_CV_IMPLEMENTATION
        // verify current thread is holding the lock
        KASSERT(lock_do_i_hold(lock));

        // acquire the spinlock and wake one sleeping thread
        spinlock_acquire(&cv->spinlock);
        wchan_wakeone(cv->cv_wchan, &cv->spinlock);
        spinlock_release(&cv->spinlock);
#else
        // Write this
	(void)cv;    // suppress warning until code gets written
	(void)lock;  // suppress warning until code gets written
#endif
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
#if (OPT_LOCK_WITH_SEMAPHORES || OPT_LOCK_WCHAN_SPINLOCK) && OPT_CV_IMPLEMENTATION
        // verify current thread is holding the lock
        KASSERT(lock_do_i_hold(lock));

        // acquire the spinlock and wake all the sleeping threads
        spinlock_acquire(&cv->spinlock);
        wchan_wakeall(cv->cv_wchan, &cv->spinlock);
        spinlock_release(&cv->spinlock);
#else
	// Write this
	(void)cv;    // suppress warning until code gets written
	(void)lock;  // suppress warning until code gets written
#endif
}
