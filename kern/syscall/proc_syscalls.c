#include <types.h>
#include <lib.h>
#include <syscall.h>
#include <thread.h>
#include <proc.h>
#include <addrspace.h>
#include <synch.h>
#include <proc.h>
#include <current.h>
#include <mips/trapframe.h>

#include <kern/unistd.h>
#include <kern/errno.h>

#include "opt-wait_pid.h"

static void enter_child_process(void * args, long unsigned int nargs);


int sys_exit(struct thread* calling_thread, int exit_code) {
    struct proc *proc;
    proc = calling_thread->t_proc;

    // remove the calling thread
    // from its process
    proc_remthread(calling_thread);

#if OPT_WAIT_PID

    // set the exit code and signal the conditional variable
    lock_acquire(proc->p_exit_cv_lock);
    proc->p_exit_code = exit_code & 0x0377;
    cv_broadcast(proc->p_exit_cv, proc->p_exit_cv_lock);
    lock_release(proc->p_exit_cv_lock);

#else 
    
    // destroy the address space
    as_destroy(proc->p_addrspace);

#endif

    thread_exit();

    return 0;
}

int sys_waitpid (pid_t pid, int *returncode, int flags) {

#if OPT_WAIT_PID
    int status;

    (void)flags;

    struct proc *p = proc_get(pid);
    if (p == NULL) {
        return -1;
    }

    status = proc_wait(p);
    if (returncode) {
        *returncode = status;
    }

    return pid;
#else 
    (void)pid;
    (void)returncode;
    (void)flags;

    return -1;
#endif
}

int sys_fork (struct trapframe *tf, pid_t *child_pid) {

#if OPT_WAIT_PID
    int result;
    struct proc *new;
    struct trapframe *child_tf;

    new = proc_fork(curproc);

    if (new == NULL) {
        // failure
        return ENOMEM;
    }

    child_tf = (struct trapframe*) kmalloc(sizeof(struct trapframe));
    if (child_tf == NULL) {
        proc_destroy(new);
        return ENOMEM;
    }

    // copy the trapframe
    memcpy(child_tf, tf, sizeof(struct trapframe));

    *child_pid = new->pid;

    result = thread_fork(new->p_name /* thread name */,
			new /* new process */,
			enter_child_process /* thread function */,
			(void*) child_tf /* thread arg */, 1 /* thread arg */);
	if (result) {
		kprintf("thread_fork failed: %s\n", strerror(result));
		proc_destroy(new);
		return ENOMEM;
    }

    return 0;
#endif

}

static void enter_child_process(void * args, long unsigned int nargs) {
#if OPT_WAIT_PID
    struct trapframe *tf = (struct trapframe*)args;
    enter_forked_process(tf);
#else 
    (void)args;
#endif
    (void)nargs;
}

pid_t sys_getpid (void) {

#if OPT_WAIT_PID

    return curproc->pid;

#endif

}
