#include "vtpc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define VTPC_DEFAULT_BLOCK_SIZE 4096
#define VTPC_DEFAULT_CACHE_BLOCKS 128

struct vtpc_file {
    int os_fd;
    off_t offset;
    off_t size;
};

struct cache_block {
    struct vtpc_file* owner;
    off_t block_index;
    size_t valid_bytes;
    bool dirty;
    size_t dirty_start;
    size_t dirty_end;
    unsigned char* data;
    struct cache_block* prev;
    struct cache_block* next;
};

static struct vtpc_file** g_files = NULL;
static size_t g_file_capacity = 0;
static size_t g_block_size = 0;
static size_t g_cache_capacity = 0;
static struct cache_block* g_blocks = NULL;
static struct cache_block* g_lru_head = NULL;
static struct cache_block* g_lru_tail = NULL;

static size_t parse_env_size(const char* name, size_t fallback) {
    const char* value = getenv(name);
    if (!value || *value == '\0') {
        return fallback;
    }

    char* end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
        return fallback;
    }
    return (size_t)parsed;
}

static size_t next_power_of_two(size_t value) {
    if (value == 0) {
        return 1;
    }
    size_t result = 1;
    while (result < value && result <= (SIZE_MAX >> 1U)) {
        result <<= 1U;
    }
    if (result < value) {
        result = value;
    }
    return result;
}

static void cache_init(void) {
    if (g_blocks) {
        return;
    }

    g_block_size = parse_env_size("VTPC_BLOCK_SIZE", VTPC_DEFAULT_BLOCK_SIZE);
    g_cache_capacity =
            parse_env_size("VTPC_CACHE_BLOCKS", VTPC_DEFAULT_CACHE_BLOCKS);

    if (g_block_size == 0) {
        g_block_size = VTPC_DEFAULT_BLOCK_SIZE;
    }
    if (g_cache_capacity == 0) {
        g_cache_capacity = VTPC_DEFAULT_CACHE_BLOCKS;
    }

    g_blocks = calloc(g_cache_capacity, sizeof(*g_blocks));
    if (!g_blocks) {
        fprintf(stderr, "vtpc: failed to allocate cache metadata\n");
        abort();
    }

    size_t alignment = next_power_of_two(g_block_size);
    if (alignment < sizeof(void*)) {
        alignment = sizeof(void*);
    }

    for (size_t i = 0; i < g_cache_capacity; ++i) {
        void* buf = NULL;
        if (posix_memalign(&buf, alignment, g_block_size) != 0) {
            fprintf(stderr, "vtpc: posix_memalign failed\n");
            abort();
        }

        g_blocks[i].data = buf;
        g_blocks[i].owner = NULL;
        g_blocks[i].block_index = 0;
        g_blocks[i].valid_bytes = 0;
        g_blocks[i].dirty = false;
        g_blocks[i].dirty_start = 0;
        g_blocks[i].dirty_end = 0;
        g_blocks[i].prev = (i == 0) ? NULL : &g_blocks[i - 1];
        g_blocks[i].next = (i + 1 < g_cache_capacity) ? &g_blocks[i + 1] : NULL;
    }

    g_lru_head = g_cache_capacity ? &g_blocks[0] : NULL;
    g_lru_tail = g_cache_capacity ? &g_blocks[g_cache_capacity - 1] : NULL;
}

static struct vtpc_file* lookup_file(int fd) {
    const int index = fd - 1;
    if (index < 0 || (size_t)index >= g_file_capacity || !g_files[index]) {
        errno = EBADF;
        return NULL;
    }
    return g_files[index];
}

static int register_file(struct vtpc_file* file) {
    for (size_t i = 0; i < g_file_capacity; ++i) {
        if (!g_files[i]) {
            g_files[i] = file;
            return (int)(i + 1);
        }
    }

    const size_t old_capacity = g_file_capacity;
    const size_t new_capacity = old_capacity ? old_capacity * 2 : 16;
    struct vtpc_file** new_table =
            realloc(g_files, new_capacity * sizeof(*new_table));
    if (!new_table) {
        errno = EMFILE;
        return -1;
    }
    for (size_t i = old_capacity; i < new_capacity; ++i) {
        new_table[i] = NULL;
    }
    g_files = new_table;
    g_file_capacity = new_capacity;
    g_files[old_capacity] = file;
    return (int)(old_capacity + 1);
}

static void lru_remove(struct cache_block* block) {
    if (block->prev) {
        block->prev->next = block->next;
    }
    if (block->next) {
        block->next->prev = block->prev;
    }
    if (block == g_lru_head) {
        g_lru_head = block->next;
    }
    if (block == g_lru_tail) {
        g_lru_tail = block->prev;
    }
    block->prev = NULL;
    block->next = NULL;
}

static void lru_push_front(struct cache_block* block) {
    block->prev = NULL;
    block->next = g_lru_head;
    if (g_lru_head) {
        g_lru_head->prev = block;
    }
    g_lru_head = block;
    if (!g_lru_tail) {
        g_lru_tail = block;
    }
}

static int flush_block(struct cache_block* block) {
    if (!block->dirty || !block->owner) {
        return 0;
    }

    size_t start = block->dirty_start;
    size_t end = block->dirty_end;
    if (end <= start) {
        block->dirty = false;
        block->dirty_start = 0;
        block->dirty_end = 0;
        return 0;
    }

    const off_t base_offset = block->block_index * (off_t)g_block_size;
    size_t remaining = end - start;
    size_t local_offset = start;

    while (remaining > 0) {
        const ssize_t written = pwrite(block->owner->os_fd,
                                       block->data + local_offset,
                                       remaining,
                                       base_offset + (off_t)local_offset);
        if (written < 0) {
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        remaining -= (size_t)written;
        local_offset += (size_t)written;
    }

    block->dirty = false;
    block->dirty_start = 0;
    block->dirty_end = 0;
    return 0;
}

static void reset_block(struct cache_block* block) {
    block->owner = NULL;
    block->block_index = 0;
    block->valid_bytes = 0;
    block->dirty = false;
    block->dirty_start = 0;
    block->dirty_end = 0;
    if (block->data) {
        memset(block->data, 0, g_block_size);
    }
}

static struct cache_block* find_block(struct vtpc_file* file,
                                      off_t block_index) {
    for (size_t i = 0; i < g_cache_capacity; ++i) {
        if (g_blocks[i].owner == file && g_blocks[i].block_index == block_index) {
            return &g_blocks[i];
        }
    }
    return NULL;
}

static int load_block(struct cache_block* block) {
    const off_t base_offset = block->block_index * (off_t)g_block_size;
    size_t expected = 0;
    if (block->owner->size > base_offset) {
        const off_t remaining = block->owner->size - base_offset;
        expected = remaining >= (off_t)g_block_size ? g_block_size
                                                    : (size_t)remaining;
    }

    if (expected > 0) {
        ssize_t rd = pread(block->owner->os_fd, block->data, g_block_size, base_offset);
        if (rd < 0) {
            return -1;
        }
        if ((size_t)rd < g_block_size) {
            memset(block->data + rd, 0, g_block_size - (size_t)rd);
        }
        block->valid_bytes = (size_t)rd;
    } else {
        memset(block->data, 0, g_block_size);
        block->valid_bytes = 0;
    }

    block->dirty = false;
    block->dirty_start = 0;
    block->dirty_end = 0;
    return 0;
}

static struct cache_block* acquire_block(struct vtpc_file* file,
                                         off_t block_index) {
    struct cache_block* block = find_block(file, block_index);
    if (block) {
        lru_remove(block);
        lru_push_front(block);
        return block;
    }

    struct cache_block* victim = NULL;
    for (size_t i = 0; i < g_cache_capacity; ++i) {
        if (g_blocks[i].owner == NULL) {
            victim = &g_blocks[i];
            break;
        }
    }
    if (!victim) {
        victim = g_lru_tail;
    }
    if (!victim) {
        errno = ENOBUFS;
        return NULL;
    }

    if (flush_block(victim) != 0) {
        return NULL;
    }

    victim->owner = file;
    victim->block_index = block_index;
    if (load_block(victim) != 0) {
        reset_block(victim);
        return NULL;
    }

    lru_remove(victim);
    lru_push_front(victim);
    return victim;
}

static int flush_file_blocks(struct vtpc_file* file) {
    for (size_t i = 0; i < g_cache_capacity; ++i) {
        if (g_blocks[i].owner == file) {
            if (flush_block(&g_blocks[i]) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static void drop_file_blocks(struct vtpc_file* file) {
    for (size_t i = 0; i < g_cache_capacity; ++i) {
        if (g_blocks[i].owner == file) {
            reset_block(&g_blocks[i]);
        }
    }
}

int vtpc_open(const char* path, int mode, int access) {
    cache_init();

    const int fd = open(path, mode, access);
    if (fd < 0) {
        return -1;
    }

    struct stat st = {0};
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

#ifdef __APPLE__
    (void)fcntl(fd, F_NOCACHE, 1);
#endif

    struct vtpc_file* file = calloc(1, sizeof(*file));
    if (!file) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }

    file->os_fd = fd;
    file->offset = 0;
    file->size = st.st_size;

    const int handle = register_file(file);
    if (handle < 0) {
        free(file);
        close(fd);
        return -1;
    }

    return handle;
}

int vtpc_close(int fd) {
    cache_init();
    struct vtpc_file* file = lookup_file(fd);
    if (!file) {
        return -1;
    }

    int result = 0;
    int saved_errno = 0;
    if (flush_file_blocks(file) != 0) {
        result = -1;
        saved_errno = errno;
    }
    if (fsync(file->os_fd) != 0) {
        result = -1;
        if (saved_errno == 0) {
            saved_errno = errno;
        }
    }

    drop_file_blocks(file);
    close(file->os_fd);
    g_files[fd - 1] = NULL;
    free(file);

    if (result != 0 && saved_errno != 0) {
        errno = saved_errno;
    }
    return result;
}

ssize_t vtpc_read(int fd, void* buf, size_t count) {
    cache_init();
    struct vtpc_file* file = lookup_file(fd);
    if (!file) {
        return -1;
    }
    unsigned char* out = buf;
    size_t total = 0;

    while (total < count) {
        if (file->offset >= file->size) {
            break;
        }
        const off_t block_index = file->offset / (off_t)g_block_size;
        const size_t block_offset = (size_t)(file->offset % (off_t)g_block_size);
        struct cache_block* block = acquire_block(file, block_index);
        if (!block) {
            return (total == 0) ? -1 : (ssize_t)total;
        }

        size_t available = block->valid_bytes;
        if (available > g_block_size) {
            available = g_block_size;
        }
        if (available <= block_offset) {
            break;
        }
        available -= block_offset;
        if (available > count - total) {
            available = count - total;
        }

        memcpy(out + total, block->data + block_offset, available);
        total += available;
        file->offset += (off_t)available;
    }

    return (ssize_t)total;
}

ssize_t vtpc_write(int fd, const void* buf, size_t count) {
    cache_init();
    struct vtpc_file* file = lookup_file(fd);
    if (!file) {
        return -1;
    }
    const unsigned char* in = buf;
    size_t total = 0;

    while (total < count) {
        const off_t block_index = file->offset / (off_t)g_block_size;
        const size_t block_offset = (size_t)(file->offset % (off_t)g_block_size);
        struct cache_block* block = acquire_block(file, block_index);
        if (!block) {
            return (total == 0) ? -1 : (ssize_t)total;
        }

        size_t space = g_block_size - block_offset;
        size_t chunk = count - total;
        if (chunk > space) {
            chunk = space;
        }

        memcpy(block->data + block_offset, in + total, chunk);
        const size_t tail = block_offset + chunk;
        if (!block->dirty) {
            block->dirty = true;
            block->dirty_start = block_offset;
            block->dirty_end = tail;
        } else {
            if (block_offset < block->dirty_start) {
                block->dirty_start = block_offset;
            }
            if (tail > block->dirty_end) {
                block->dirty_end = tail;
            }
        }

        if (tail > block->valid_bytes) {
            block->valid_bytes = tail;
            if (block->valid_bytes > g_block_size) {
                block->valid_bytes = g_block_size;
            }
        }

        file->offset += (off_t)chunk;
        if (file->offset > file->size) {
            file->size = file->offset;
        }
        total += chunk;
    }

    return (ssize_t)total;
}

off_t vtpc_lseek(int fd, off_t offset, int whence) {
    cache_init();
    struct vtpc_file* file = lookup_file(fd);
    if (!file) {
        return -1;
    }

    off_t target = 0;
    if (whence == SEEK_SET) {
        target = offset;
    } else if (whence == SEEK_CUR) {
        target = file->offset + offset;
    } else if (whence == SEEK_END) {
        target = file->size + offset;
    } else {
        errno = EINVAL;
        return -1;
    }

    if (target < 0) {
        errno = EINVAL;
        return -1;
    }

    file->offset = target;
    return target;
}

int vtpc_fsync(int fd) {
    cache_init();
    struct vtpc_file* file = lookup_file(fd);
    if (!file) {
        return -1;
    }

    int result = 0;
    int saved_errno = 0;
    if (flush_file_blocks(file) != 0) {
        result = -1;
        saved_errno = errno;
    }
    if (fsync(file->os_fd) != 0) {
        result = -1;
        if (saved_errno == 0) {
            saved_errno = errno;
        }
    }
    if (result != 0 && saved_errno != 0) {
        errno = saved_errno;
    }
    return result;
}
