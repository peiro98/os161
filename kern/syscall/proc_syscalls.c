#include <types.h>
#include <lib.h>
#include <syscall.h>
#include <thread.h>
#include <proc.h>
#include <addrspace.h>

#include <kern/unistd.h>


int sys_exit(struct thread* calling_thread, int exit_code) {
    calling_thread->exit_code = exit_code;

    kprintf("PROCESS RETURNED WITH STATUS %d\n", exit_code);

    // get the address space of the current process
    as_destroy(calling_thread->t_proc->p_addrspace);
    thread_exit();

    return 0;
}