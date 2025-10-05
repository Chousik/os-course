#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_DIRECT
#define O_DIRECT 0
#endif

struct options {
    const char *file_path;
    bool write_mode;
    size_t block_size;
    size_t block_count;
    off_t range_start;
    off_t range_end;
    bool range_full;
    bool use_direct;
    bool random_access;
    uint64_t seed;
};

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --rw=<read|write> --block-size=<bytes> --block-count=<count> --file=<path>\n"
            "           [--range=<start>-<end>] [--direct=<on|off>] [--type=<sequence|random>] [--seed=<number>]\n",
            prog);
}

static bool parse_size_t(const char *arg, size_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long val = strtoull(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0') {
        return false;
    }
    *out = (size_t)val;
    return true;
}

static bool parse_off_t(const char *arg, off_t *out) {
    char *end = NULL;
    errno = 0;
    long long val = strtoll(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || val < 0) {
        return false;
    }
    *out = (off_t)val;
    return true;
}

static bool parse_range(const char *value, off_t *start, off_t *end, bool *range_full) {
    char *dash = strchr(value, '-');
    if (!dash) {
        return false;
    }
    *dash = '\0';
    off_t s = 0;
    off_t e = 0;
    if (!parse_off_t(value, &s)) {
        *dash = '-';
        return false;
    }
    if (!parse_off_t(dash + 1, &e)) {
        *dash = '-';
        return false;
    }
    *dash = '-';
    *start = s;
    *end = e;
    *range_full = (s == 0 && e == 0);
    return true;
}

static uint64_t mix_seed(uint64_t seed) {
    uint64_t z = seed + UINT64_C(0x9E3779B97F4A7C15);
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static uint32_t rng_next(uint64_t *state) {
    *state += UINT64_C(0x9E3779B97F4A7C15);
    uint64_t z = *state;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    z ^= z >> 31;
    return (uint32_t)z;
}

static double now_seconds(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    }
#endif
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static bool fill_buffer(uint8_t *buffer, size_t size, bool write_mode, uint64_t *state) {
    if (!write_mode) {
        memset(buffer, 0, size);
        return true;
    }
    if (!state) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        buffer[i] = (uint8_t)(rng_next(state) & 0xFF);
    }
    return true;
}

int main(int argc, char **argv) {
    struct options opts = {
        .file_path = NULL,
        .write_mode = false,
        .block_size = 0,
        .block_count = 0,
        .range_start = 0,
        .range_end = 0,
        .range_full = true,
        .use_direct = false,
        .random_access = false,
        .seed = (uint64_t)time(NULL),
    };

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = strchr(arg, '=');
        char key_buf[64];
        if (value) {
            size_t key_len = (size_t)(value - arg);
            if (key_len >= sizeof(key_buf)) {
                fprintf(stderr, "Option name too long: %s\n", arg);
                return EXIT_FAILURE;
            }
            memcpy(key_buf, arg, key_len);
            key_buf[key_len] = '\0';
            value += 1;
            arg = key_buf;
        }

        if (strcmp(arg, "--rw") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value) {
                fprintf(stderr, "Missing rw value.\n");
                return EXIT_FAILURE;
            }
            if (strcmp(value, "read") == 0) {
                opts.write_mode = false;
            } else if (strcmp(value, "write") == 0) {
                opts.write_mode = true;
            } else {
                fprintf(stderr, "Unsupported rw mode: %s\n", value);
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "--block-size") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value || !parse_size_t(value, &opts.block_size) || opts.block_size == 0) {
                fprintf(stderr, "Invalid block-size value.\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "--block-count") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value || !parse_size_t(value, &opts.block_count) || opts.block_count == 0) {
                fprintf(stderr, "Invalid block-count value.\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "--file") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value) {
                fprintf(stderr, "Missing file path value.\n");
                return EXIT_FAILURE;
            }
            opts.file_path = value;
        } else if (strcmp(arg, "--range") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value) {
                fprintf(stderr, "Missing range value.\n");
                return EXIT_FAILURE;
            }
            off_t start = 0;
            off_t end = 0;
            bool full = false;
            char *dup = strdup(value);
            if (!dup) {
                fprintf(stderr, "Memory allocation failure.\n");
                return EXIT_FAILURE;
            }
            bool ok = parse_range(dup, &start, &end, &full);
            free(dup);
            if (!ok) {
                fprintf(stderr, "Invalid range format. Expected start-end.\n");
                return EXIT_FAILURE;
            }
            opts.range_start = start;
            opts.range_end = end;
            opts.range_full = full;
        } else if (strcmp(arg, "--direct") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value) {
                fprintf(stderr, "Missing direct value.\n");
                return EXIT_FAILURE;
            }
            if (strcmp(value, "on") == 0) {
                opts.use_direct = true;
            } else if (strcmp(value, "off") == 0) {
                opts.use_direct = false;
            } else {
                fprintf(stderr, "Unsupported direct value: %s\n", value);
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "--type") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value) {
                fprintf(stderr, "Missing type value.\n");
                return EXIT_FAILURE;
            }
            if (strcmp(value, "sequence") == 0) {
                opts.random_access = false;
            } else if (strcmp(value, "random") == 0) {
                opts.random_access = true;
            } else {
                fprintf(stderr, "Unsupported type value: %s\n", value);
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "--seed") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value) {
                fprintf(stderr, "Missing seed value.\n");
                return EXIT_FAILURE;
            }
            char *endptr = NULL;
            errno = 0;
            unsigned long long parsed = strtoull(value, &endptr, 10);
            if (errno != 0 || endptr == value || *endptr != '\0') {
                fprintf(stderr, "Invalid seed value.\n");
                return EXIT_FAILURE;
            }
            opts.seed = parsed;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!opts.file_path || opts.block_size == 0 || opts.block_count == 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (opts.use_direct && O_DIRECT == 0) {
        fprintf(stderr, "direct=on is not supported on this platform.\n");
        return EXIT_FAILURE;
    }

    uint64_t rng_state = mix_seed(opts.seed);
    int open_flags = opts.write_mode ? (O_RDWR | O_CREAT) : O_RDONLY;
    if (opts.use_direct) {
        open_flags |= O_DIRECT;
    }

    int fd = open(opts.file_path, open_flags, 0644);
    if (fd < 0) {
        perror("open");
        return EXIT_FAILURE;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("fstat");
        close(fd);
        return EXIT_FAILURE;
    }

    off_t file_size = st.st_size;
    off_t effective_end = 0;

    if (opts.range_full) {
        effective_end = file_size;
        if (opts.write_mode) {
            off_t required = (off_t)opts.range_start + (off_t)(opts.block_size * opts.block_count);
            if (required > effective_end) {
                if (ftruncate(fd, required) != 0) {
                    perror("ftruncate");
                    close(fd);
                    return EXIT_FAILURE;
                }
                effective_end = required;
            }
        }
    } else {
        effective_end = opts.range_end;
        if (effective_end <= opts.range_start) {
            fprintf(stderr, "range end must be greater than start.\n");
            close(fd);
            return EXIT_FAILURE;
        }
        if (opts.write_mode && effective_end > file_size) {
            if (ftruncate(fd, effective_end) != 0) {
                perror("ftruncate");
                close(fd);
                return EXIT_FAILURE;
            }
        }
        if (!opts.write_mode && effective_end > file_size) {
            effective_end = file_size;
        }
    }

    if (!opts.write_mode && effective_end <= opts.range_start) {
        fprintf(stderr, "Effective range is empty for reading.\n");
        close(fd);
        return EXIT_FAILURE;
    }

    off_t span = effective_end - opts.range_start;
    if (span < (off_t)opts.block_size) {
        fprintf(stderr, "Range is smaller than block size.\n");
        close(fd);
        return EXIT_FAILURE;
    }

    size_t alignment = opts.use_direct ? (size_t)sysconf(_SC_PAGESIZE) : sizeof(void *);
    if (alignment == 0) {
        alignment = 4096;
    }

    if (opts.use_direct && (opts.block_size % alignment) != 0) {
        fprintf(stderr, "Block size must be multiple of %zu when using direct IO.\n", alignment);
        close(fd);
        return EXIT_FAILURE;
    }

    uint8_t *buffer = NULL;
    int rc = posix_memalign((void **)&buffer, alignment, opts.block_size);
    if (rc != 0 || !buffer) {
        fprintf(stderr, "posix_memalign failed (%d).\n", rc);
        close(fd);
        return EXIT_FAILURE;
    }

    if (!fill_buffer(buffer, opts.block_size, opts.write_mode, &rng_state)) {
        fprintf(stderr, "Failed to initialize buffer.\n");
        free(buffer);
        close(fd);
        return EXIT_FAILURE;
    }

    double start_time = now_seconds();
    size_t completed = 0;
    size_t short_io = 0;
    size_t errors = 0;

    off_t current = opts.range_start;
    size_t blocks_in_span = (size_t)((span - (off_t)opts.block_size) / (off_t)opts.block_size + 1);

    for (size_t iter = 0; iter < opts.block_count; ++iter) {
        off_t offset = current;
        if (opts.random_access) {
            if (blocks_in_span == 0) {
                fprintf(stderr, "No blocks available for random access.\n");
                break;
            }
            size_t block_index = blocks_in_span > 1 ? (rng_next(&rng_state) % blocks_in_span) : 0;
            offset = opts.range_start + (off_t)block_index * (off_t)opts.block_size;
        } else {
            current += (off_t)opts.block_size;
            if (current >= effective_end) {
                current = opts.range_start;
            }
        }

        ssize_t io_res;
        if (opts.write_mode) {
            io_res = pwrite(fd, buffer, opts.block_size, offset);
        } else {
            io_res = pread(fd, buffer, opts.block_size, offset);
        }

        if (io_res < 0) {
            perror(opts.write_mode ? "pwrite" : "pread");
            errors++;
            break;
        }
        if ((size_t)io_res != opts.block_size) {
            short_io++;
        }
        if (opts.write_mode && opts.random_access) {
            // Refill buffer with new pseudo-random data to avoid cache reuse.
            fill_buffer(buffer, opts.block_size, true, &rng_state);
        }
        completed++;
    }

    double end_time = now_seconds();

    free(buffer);
    close(fd);

    double elapsed = end_time - start_time;
    double throughput_mb = 0.0;
    if (elapsed > 0.0) {
        throughput_mb = (double)(completed * opts.block_size) / (1024.0 * 1024.0) / elapsed;
    }

    printf("Mode: %s\n", opts.write_mode ? "write" : "read");
    printf("Access: %s\n", opts.random_access ? "random" : "sequence");
    printf("Blocks processed: %zu\n", completed);
    printf("Bytes processed: %zu\n", completed * opts.block_size);
    printf("Short operations: %zu\n", short_io);
    printf("Errors: %zu\n", errors);
    printf("Elapsed seconds: %.6f\n", elapsed);
    printf("Throughput MiB/s: %.2f\n", throughput_mb);

    return errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
