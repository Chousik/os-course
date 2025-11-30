#include "ema_replace.h"
#include "vtpc/vtpc.h"

#include <errno.h>
#include <unistd.h>

static int vtpc_iface_open(const char *path, int flags) {
    (void)flags;
    return vtpc_open(path);
}

static int vtpc_iface_close(int fd) {
    return vtpc_close(fd);
}

static ssize_t vtpc_iface_pread(int fd, void *buf, size_t count, off_t offset) {
    off_t current = vtpc_lseek(fd, 0, SEEK_CUR);
    if (current < 0) {
        return -1;
    }
    if (vtpc_lseek(fd, offset, SEEK_SET) < 0) {
        return -1;
    }
    ssize_t result = vtpc_read(fd, buf, count);
    int saved_errno = errno;
    if (vtpc_lseek(fd, current, SEEK_SET) < 0) {
        errno = ESPIPE;
        return -1;
    }
    if (result < 0) {
        errno = saved_errno;
    }
    return result;
}

static ssize_t vtpc_iface_pwrite(int fd, const void *buf, size_t count, off_t offset) {
    off_t current = vtpc_lseek(fd, 0, SEEK_CUR);
    if (current < 0) {
        return -1;
    }
    if (vtpc_lseek(fd, offset, SEEK_SET) < 0) {
        return -1;
    }
    ssize_t result = vtpc_write(fd, buf, count);
    int saved_errno = errno;
    if (vtpc_lseek(fd, current, SEEK_SET) < 0) {
        errno = ESPIPE;
        return -1;
    }
    if (result < 0) {
        errno = saved_errno;
    }
    return result;
}

static off_t vtpc_iface_lseek(int fd, off_t offset, int whence) {
    return vtpc_lseek(fd, offset, whence);
}

static int vtpc_iface_fsync(int fd) {
    return vtpc_fsync(fd);
}

static const struct ema_io_iface VTPC_IO = {
    .name = "vtpc",
    .open = vtpc_iface_open,
    .close = vtpc_iface_close,
    .pread = vtpc_iface_pread,
    .pwrite = vtpc_iface_pwrite,
    .lseek = vtpc_iface_lseek,
    .fsync = vtpc_iface_fsync,
};

int main(int argc, char **argv) {
    return ema_replace_main(&VTPC_IO, argc, argv);
}
