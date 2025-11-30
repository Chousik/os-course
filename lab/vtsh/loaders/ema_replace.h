#ifndef LOADERS_EMA_REPLACE_H
#define LOADERS_EMA_REPLACE_H

#include <stddef.h>
#include <sys/types.h>

typedef ssize_t (*ema_pread_func_t)(int fd, void *buf, size_t count, off_t offset);
typedef ssize_t (*ema_pwrite_func_t)(int fd, const void *buf, size_t count, off_t offset);
typedef off_t (*ema_lseek_func_t)(int fd, off_t offset, int whence);
typedef int (*ema_fsync_func_t)(int fd);
typedef int (*ema_open_func_t)(const char *path, int flags);
typedef int (*ema_close_func_t)(int fd);

struct ema_io_iface {
    const char *name;
    ema_open_func_t open;
    ema_close_func_t close;
    ema_pread_func_t pread;
    ema_pwrite_func_t pwrite;
    ema_lseek_func_t lseek;
    ema_fsync_func_t fsync;
};

int ema_replace_main(const struct ema_io_iface *iface, int argc, char **argv);

#endif // LOADERS_EMA_REPLACE_H
