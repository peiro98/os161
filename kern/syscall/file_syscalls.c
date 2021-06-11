#include <types.h>
#include <lib.h>
#include <syscall.h>
#include <proc.h>
#include <spinlock.h>
#include <kern/errno.h>
#include <current.h>
#include <vfs.h>
#include <limits.h>
#include <uio.h>
#include <vnode.h>
#include <copyinout.h>

#include <kern/unistd.h>

#include "opt-lab05.h"

#define MIN_FD (STDERR_FILENO + 1)

// define the maximum number of open files
#define SYS_MAX_OPEN_FILES_NUM (10 * OPEN_MAX)

#if OPT_LAB05
static struct openfile openfiles[SYS_MAX_OPEN_FILES_NUM];
static struct spinlock lock_openfiles = SPINLOCK_INITIALIZER;

static int add_openfile (struct vnode *v) {
    int index = -1;

    spinlock_acquire(&lock_openfiles);
    for (int i = 0; i < SYS_MAX_OPEN_FILES_NUM; i++) {
        if (openfiles[i].of_reference_count == 0) {
            index = i;

            openfiles[i].of_v = v;
            openfiles[i].of_offset = 0;
            openfiles[i].of_reference_count = 1;

            // spot found, return
            break;
        }
    }
    spinlock_release(&lock_openfiles);

    return index;
}

static bool is_file_descriptor_open (int index) {
    if (index < 0 || index >= SYS_MAX_OPEN_FILES_NUM) {
        return false;
    }

    return openfiles[index].of_v != NULL;
}

/*
 * Remove an openfile from the system table
 * Decrement its reference count and close the vfs. 
 */
static void remove_openfile (int index) {
    struct vnode *v = NULL;

    spinlock_acquire(&lock_openfiles);
    if (--openfiles[index].of_reference_count == 0) {
        v = openfiles[index].of_v;
    }
    spinlock_release(&lock_openfiles);

    // cpu must not hold a spinlock before calling vfs_close
    if (v) vfs_close(v);
}

#endif


int sys_open(userptr_t pathname, int flags, int *fd) {
#if OPT_LAB05
    struct vnode *v;
    int result, i;
    char *path = (char*) pathname;

    /* Open the file. */
	result = vfs_open(path, flags, 0, &v);
	if (result) {
		return result;
	}

    // fill the openfile structure
    *fd = add_openfile(v);
    // no space in the system table
    if (*fd == -1) {
        // close the vnode and return ENFILE
        vfs_close(v);
        return ENFILE;
    }

    // attach the openfile to the in-process table
    spinlock_acquire(&curproc->p_lock);
    for (i = 0; i < OPEN_MAX && curproc->p_openfiles[i]; i++)
    if (i == OPEN_MAX) {
        // process has already opened too many files
        remove_openfile(*fd);
        spinlock_release(&curproc->p_lock);
        return ENFILE;  
    }
    curproc->p_openfiles[i] = openfiles + *fd;
    spinlock_release(&curproc->p_lock);

    // skip STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO
    *fd += MIN_FD;
    return 0;
#else 
    (void)pathname;
    (void)flags;
    (void)fd;
    return -1;
#endif
}

int sys_close(int fd) { 
#if OPT_LAB05
    int i, of_index;
    struct openfile *of;

    of_index = fd - MIN_FD;

    if (of_index < 0 || of_index >= SYS_MAX_OPEN_FILES_NUM) return EBADF;

    of = &openfiles[of_index];
    if (of == NULL) return EBADF;

    // remove the open file from the current process
    spinlock_acquire(&curproc->p_lock);
    for (i = 0; i < OPEN_MAX && curproc->p_openfiles[i] != of; i++);
    if (i == OPEN_MAX) {
        spinlock_release(&curproc->p_lock);
        return EBADF;
    }
    curproc->p_openfiles[i] = NULL;
    spinlock_release(&curproc->p_lock);

    remove_openfile(of_index);

    return 0;

#else
    (void)fd;
    return ENOSYS;
#endif
}

int sys_write(int filehandle, const void *user_buffer, size_t size, ssize_t *retval)
{
#if OPT_LAB05
    int of_index, result;
    struct iovec iov;
	struct uio ku;
    void *kbuffer;

    *retval = size;

    // allocate a kernel buffer
    kbuffer = kmalloc(size);
    if (kbuffer == NULL) {
        return -1;
    }

    // and copy the data to write into the kernel buffer
    if (copyin(user_buffer, kbuffer, size)) {
        kfree(kbuffer);
        return -1;
    }

    if (filehandle == STDOUT_FILENO || filehandle == STDERR_FILENO) {
        // if the destination is either stdout or stderr, put the characters on the console
        size_t i = 0;
        while (i < size) putch(*((char *)kbuffer + (i++)));
    } else {
        // verify the file descriptor is valid and opened by curproc
        of_index = filehandle - MIN_FD;
        if (!is_file_descriptor_open(of_index) || !proc_opened(curproc, &openfiles[of_index])) {
            kfree(kbuffer);
            return EBADF;
        }

        // write the data using uio_kinit and VOP_WRITE
        // NOTE: uio_kinit assumes a kernel buffer as src/dest region
        uio_kinit(&iov, &ku, kbuffer, size, openfiles[of_index].of_offset, UIO_WRITE);
        result = VOP_WRITE(openfiles[of_index].of_v, &ku);
        if (result) {
            kfree(kbuffer);
            return result;
        }

        *retval = size - ku.uio_resid;

        openfiles[of_index].of_offset += size;
    }

    // free the kernel buffer and return
    kfree(kbuffer);
    return 0;
    
#else 
    // check if the file handle is STDOUT_FILENO
    KASSERT(filehandle == STDOUT_FILENO || filehandle == STDERR_FILENO);

    size_t i = 0;
    while (i < size)
        putch(*((char *)buf + (i++)));

    *retval = size;
    return 0;
#endif
}

ssize_t sys_read(int filehandle, void *user_buffer, size_t size, ssize_t *retval) {
#if OPT_LAB05
    int of_index, result;
    struct iovec iov;
	struct uio ku;
    void *kbuffer;

    *retval = size;

    // allocate a kernel buffer
    kbuffer = kmalloc(size);
    if (kbuffer == NULL) {
        return -1;
    }

    if (filehandle == STDIN_FILENO) {
        // if the destination is either stdout or stderr, put the characters on the console
        size_t i = 0;
        while (i < size) *((char *)kbuffer + (i++)) = getch();
    } else {
        // verify the file descriptor is valid and opened by curproc
        of_index = filehandle - MIN_FD;
        if (!is_file_descriptor_open(of_index) || !proc_opened(curproc, &openfiles[of_index])) {
            kfree(kbuffer);
            return EBADF;
        }

        // the process opened the file
        uio_kinit(&iov, &ku, kbuffer, size, openfiles[of_index].of_offset, UIO_READ);
        result = VOP_READ(openfiles[of_index].of_v, &ku);
        if (result || ku.uio_resid) {
            kfree(kbuffer);
            return EIO;
        }

        *retval = size - ku.uio_resid;

        openfiles[of_index].of_offset += size;
    }

    // copy the read data to the user buffer
    if (copyout(kbuffer, user_buffer, size)) {
        kfree(kbuffer);
        return -1;
    }

    // free the kernel buffer and return
    kfree(kbuffer);
    return 0;
#else
    // check if the file handle is STDOUT_FILENO
    KASSERT(filehandle == STDIN_FILENO);

    size_t i = 0;
    while (i < size)
        *((char *)buf + (i++)) = getch();

    return size;
#endif
}