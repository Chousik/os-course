#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct options {
    const char *file_path;
    const char *needle;
    const char *replacement;
    size_t repeat;
    size_t buffer_size;
    size_t generate_size;
    uint64_t seed;
    bool do_generate;
    bool do_fsync;
};

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --file=<path> --needle=<string> --replacement=<string> --repeat=<count> [options]\n"
            "Options:\n"
            "  --buffer-size=<bytes>    Buffer size for streaming IO (default: 1048576).\n"
            "  --generate-size=<bytes>  Generate or overwrite file with random data of given size.\n"
            "  --seed=<number>          Seed for pseudo-random generators.\n"
            "  --fsync                  Force fsync after each iteration.\n"
            "  --help                   Show this message.\n",
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

static bool write_full(int fd, const char *buffer, size_t total) {
    size_t written_total = 0;
    while (written_total < total) {
        ssize_t written = write(fd, buffer + written_total, total - written_total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("write");
            return false;
        }
        if (written == 0) {
            fprintf(stderr, "Short write: disk full or unavailable.\n");
            return false;
        }
        written_total += (size_t)written;
    }
    return true;
}

static int generate_file(const struct options *opts, size_t needle_len) {
    int fd = open(opts->file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open for generate");
        return -1;
    }

    size_t buffer_size = opts->buffer_size;
    if (buffer_size < needle_len) {
        buffer_size = needle_len;
    }
    char *buffer = malloc(buffer_size);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate buffer for generation.\n");
        close(fd);
        return -1;
    }

    uint64_t rng = mix_seed(opts->seed);
    size_t remaining = opts->generate_size;
    size_t total_written = 0;
    size_t interval = needle_len ? needle_len * 101 : 1024;

    while (remaining > 0) {
        size_t chunk = remaining < buffer_size ? remaining : buffer_size;
        for (size_t i = 0; i < chunk; ++i) {
            buffer[i] = (char)('a' + (rng_next(&rng) % 26));
        }

        if (needle_len > 0 && total_written % interval == 0 && chunk >= needle_len) {
            size_t pos = (rng_next(&rng) % (chunk - needle_len + 1));
            memcpy(buffer + pos, opts->needle, needle_len);
        }

        if (!write_full(fd, buffer, chunk)) {
            free(buffer);
            close(fd);
            return -1;
        }
        remaining -= chunk;
        total_written += chunk;
    }

    if (opts->do_fsync) {
        if (fsync(fd) != 0) {
            perror("fsync");
        }
    }

    free(buffer);
    close(fd);
    return 0;
}

static size_t replace_in_file(int fd, const struct options *opts, size_t needle_len) {
    size_t buffer_size = opts->buffer_size;
    if (buffer_size < needle_len) {
        buffer_size = needle_len;
    }

    char *buffer = malloc(buffer_size + (needle_len > 1 ? needle_len - 1 : 0));
    if (!buffer) {
        fprintf(stderr, "Failed to allocate IO buffer.\n");
        return 0;
    }

    size_t tail = 0;
    off_t offset = 0;
    size_t replacements = 0;

    for (;;) {
        ssize_t rd = 0;
        do {
            rd = pread(fd, buffer + tail, buffer_size, offset);
        } while (rd < 0 && errno == EINTR);
        if (rd < 0) {
            perror("pread");
            break;
        }
        if (rd == 0) {
            break;
        }
        size_t chunk = (size_t)rd;
        size_t total = tail + chunk;
        size_t limit = total >= needle_len ? total - needle_len + 1 : 0;

        off_t base_offset = offset - (off_t)tail;
        for (size_t pos = 0; pos < limit; ++pos) {
            if (buffer[pos] == opts->needle[0] && memcmp(buffer + pos, opts->needle, needle_len) == 0) {
                off_t target = base_offset + (off_t)pos;
                size_t total_written = 0;
                while (total_written < needle_len) {
                    ssize_t wr = pwrite(fd,
                                        opts->replacement + total_written,
                                        needle_len - total_written,
                                        target + (off_t)total_written);
                    if (wr < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        perror("pwrite");
                        free(buffer);
                        return replacements;
                    }
                    if (wr == 0) {
                        fprintf(stderr, "Short write while replacing.\n");
                        free(buffer);
                        return replacements;
                    }
                    total_written += (size_t)wr;
                }
                replacements++;
            }
        }

        if (needle_len > 1) {
            tail = needle_len - 1;
            if (total < tail) {
                tail = total;
            }
            memmove(buffer, buffer + total - tail, tail);
        } else {
            tail = 0;
        }

        offset += (off_t)chunk;
    }

    if (opts->do_fsync) {
        if (fsync(fd) != 0) {
            perror("fsync");
        }
    }

    free(buffer);
    return replacements;
}

int main(int argc, char **argv) {
    struct options opts = {
        .file_path = NULL,
        .needle = NULL,
        .replacement = NULL,
        .repeat = 0,
        .buffer_size = 1 << 20,
        .generate_size = 0,
        .seed = (uint64_t)time(NULL),
        .do_generate = false,
        .do_fsync = false,
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

        if (strcmp(arg, "--file") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value) {
                fprintf(stderr, "Missing file path value.\n");
                return EXIT_FAILURE;
            }
            opts.file_path = value;
        } else if (strcmp(arg, "--needle") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value) {
                fprintf(stderr, "Missing needle value.\n");
                return EXIT_FAILURE;
            }
            opts.needle = value;
        } else if (strcmp(arg, "--replacement") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value) {
                fprintf(stderr, "Missing replacement value.\n");
                return EXIT_FAILURE;
            }
            opts.replacement = value;
        } else if (strcmp(arg, "--repeat") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value || !parse_size_t(value, &opts.repeat) || opts.repeat == 0) {
                fprintf(stderr, "Invalid repeat value.\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "--buffer-size") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value || !parse_size_t(value, &opts.buffer_size) || opts.buffer_size == 0) {
                fprintf(stderr, "Invalid buffer-size value.\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "--generate-size") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value || !parse_size_t(value, &opts.generate_size) || opts.generate_size == 0) {
                fprintf(stderr, "Invalid generate-size value.\n");
                return EXIT_FAILURE;
            }
            opts.do_generate = true;
        } else if (strcmp(arg, "--seed") == 0) {
            if (!value && i + 1 < argc) {
                value = argv[++i];
            }
            if (!value) {
                fprintf(stderr, "Missing seed value.\n");
                return EXIT_FAILURE;
            }
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = strtoull(value, &end, 10);
            if (errno != 0 || end == value || *end != '\0') {
                fprintf(stderr, "Invalid seed value.\n");
                return EXIT_FAILURE;
            }
            opts.seed = parsed;
        } else if (strcmp(arg, "--fsync") == 0) {
            opts.do_fsync = true;
            if (value) {
                fprintf(stderr, "--fsync does not take a value.\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!opts.file_path || !opts.needle || !opts.replacement || opts.repeat == 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    size_t needle_len = strlen(opts.needle);
    if (needle_len == 0) {
        fprintf(stderr, "Needle must not be empty.\n");
        return EXIT_FAILURE;
    }

    if (strlen(opts.replacement) != needle_len) {
        fprintf(stderr, "Replacement must have the same length as needle.\n");
        return EXIT_FAILURE;
    }

    if (opts.buffer_size < needle_len) {
        opts.buffer_size = needle_len;
    }

    if (opts.do_generate) {
        if (generate_file(&opts, needle_len) != 0) {
            return EXIT_FAILURE;
        }
    }

    int fd = open(opts.file_path, O_RDWR);
    if (fd < 0) {
        perror("open");
        return EXIT_FAILURE;
    }

    size_t total_replacements = 0;
    size_t min_repl = SIZE_MAX;
    size_t max_repl = 0;

    for (size_t iter = 0; iter < opts.repeat; ++iter) {
        size_t replaced = replace_in_file(fd, &opts, needle_len);
        total_replacements += replaced;
        if (replaced < min_repl) {
            min_repl = replaced;
        }
        if (replaced > max_repl) {
            max_repl = replaced;
        }
        if (lseek(fd, 0, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            return EXIT_FAILURE;
        }
    }

    close(fd);

    if (min_repl == SIZE_MAX) {
        min_repl = 0;
    }

    double avg = (double)total_replacements / (double)opts.repeat;
    printf("Iterations: %zu\n", opts.repeat);
    printf("Average replacements: %.2f\n", avg);
    printf("Min replacements: %zu\n", min_repl);
    printf("Max replacements: %zu\n", max_repl);

    return EXIT_SUCCESS;
}
