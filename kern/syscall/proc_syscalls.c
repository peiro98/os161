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

#include "opt-wait_pid.h"

static void enter_child_process(void * args, long unsigned int nargs);


int sys_exit(struct thread* calling_thread, int exit_code) {
    /*calling_thread->exit_code = exit_code;

    kprintf("PROCESS RETURNED WITH STATUS %d\n", exit_code);*/

    struct proc *proc;
    proc = calling_thread->t_proc;

    // detach a thread 
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

pid_t sys_waitpid (pid_t pid, int *returncode, int flags) {

#if OPT_WAIT_PID
    struct proc *p = proc_get(pid);
    if (p == NULL) {
        return -1;
    }

    *returncode = proc_wait(p);
#else 
    (void)pid;
    (void)returncode;
#endif
    (void)flags;
    
    return pid;
}

pid_t sys_fork (struct trapframe *tf) {

#if OPT_WAIT_PID
    int result;
    struct proc* old, *new;
    struct trapframe *child_tf;
    pid_t pid;

    old = curproc;

    new = proc_fork(old);

    if (new == NULL) {
        // failure
        return -1;
    }

    child_tf = (struct trapframe*) kmalloc(sizeof(struct trapframe));
    memcpy(child_tf, tf, sizeof(struct trapframe));

    pid = new->pid;

    result = thread_fork(new->p_name /* thread name */,
			new /* new process */,
			enter_child_process /* thread function */,
			(void*) child_tf /* thread arg */, 1 /* thread arg */);
	if (result) {
		kprintf("thread_fork failed: %s\n", strerror(result));
		proc_destroy(new);
		return -1;
    }

    //proc_wait(new);
    //kprintf("\n%d dead\n", new->pid);

    return pid;
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
