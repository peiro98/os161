#include <types.h>
#include <lib.h>
#include <syscall.h>

#include <kern/unistd.h>

ssize_t sys_write(int filehandle, const void *buf, size_t size)
{
    // check if the file handle is STDOUT_FILENO
    KASSERT(filehandle == STDOUT_FILENO);

    size_t i = 0;
    while (i < size)
        putch(*((char *)buf + (i++)));

    return size;
}
