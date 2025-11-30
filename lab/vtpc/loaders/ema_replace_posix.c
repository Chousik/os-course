#include "ema_replace.h"

#include <fcntl.h>
#include <unistd.h>

static int posix_open(const char *path, int flags) {
    return open(path, flags);
}

static int posix_close(int fd) {
    return close(fd);
}

static ssize_t posix_pread(int fd, void *buf, size_t count, off_t offset) {
    return pread(fd, buf, count, offset);
}

static ssize_t posix_pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return pwrite(fd, buf, count, offset);
}

static off_t posix_lseek(int fd, off_t offset, int whence) {
    return lseek(fd, offset, whence);
}

static int posix_fsync(int fd) {
    return fsync(fd);
}

static const struct ema_io_iface POSIX_IO = {
    .name = "posix",
    .open = posix_open,
    .close = posix_close,
    .pread = posix_pread,
    .pwrite = posix_pwrite,
    .lseek = posix_lseek,
    .fsync = posix_fsync,
};

int main(int argc, char **argv) {
    return ema_replace_main(&POSIX_IO, argc, argv);
}
