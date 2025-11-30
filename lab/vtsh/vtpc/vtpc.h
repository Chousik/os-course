#ifndef VTPC_VTPC_H
#define VTPC_VTPC_H

#include <sys/types.h>

typedef struct vtpc_file vtpc_file_t;

int vtpc_open(const char *path);
int vtpc_close(int fd);
ssize_t vtpc_read(int fd, void *buf, size_t count);
ssize_t vtpc_write(int fd, const void *buf, size_t count);
off_t vtpc_lseek(int fd, off_t offset, int whence);
int vtpc_fsync(int fd);

#endif // VTPC_VTPC_H
