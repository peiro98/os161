#include <types.h>
#include <lib.h>
#include <syscall.h>
#include <thread.h>
#include <proc.h>
#include <addrspace.h>
#include <synch.h>

#include <kern/unistd.h>

#include "opt-wait_pid.h"


int sys_exit(struct thread* calling_thread, int exit_code) {
    /*calling_thread->exit_code = exit_code;

    kprintf("PROCESS RETURNED WITH STATUS %d\n", exit_code);*/

    struct proc *proc;
    proc = calling_thread->t_proc;

#if OPT_WAIT_PID

    // set the exit code and signal the conditional variable
    lock_acquire(proc->p_exit_cv_lock);
    proc->p_exit_code = exit_code & 0x0377;
    cv_broadcast(proc->p_exit_cv, proc->p_exit_cv_lock);
    lock_release(proc->p_exit_cv_lock);

#endif
    
    // get the address space of the current process
    as_destroy(proc->p_addrspace);
    thread_exit();

    return 0;
}