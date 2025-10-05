#include "vtsh.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS 32
#define MAX_CMD_LENGTH 100
#define MAX_TOKENS 64
#define FORK_ERROR "fork error"
#define PIPE_ERROR "Pipe creation failed"
#define DUP2_STDIN_ERROR "dup2(stdin) failed"
#define DUP2_STDOUT_ERROR "dup2(stdout) failed"

static const char *SELF_PATH = NULL;

const char* vtsh_prompt(void) {
    return "vtsh> ";
}

static void trim_spaces(char *s) {
    if (!s) {
        return;
    }

    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') {
        ++p;
    }

    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }

    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static int count_pipes(const char *s) {
    int c = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '|') {
            ++c;
        }
    }
    return c;
}

static void report_syntax_error(void) {
    fprintf(stdout, "Syntax error\n");
    fflush(stdout);
}

static void report_io_error(void) {
    fprintf(stdout, "I/O error\n");
    fflush(stdout);
}

static int tokenize_command(const char *command, char *tokens[], int max_tokens) {
    static char storage[MAX_TOKENS][MAX_CMD_LENGTH];

    int count = 0;
    size_t idx = 0;

    while (command[idx] != '\0') {
        while (command[idx] == ' ' || command[idx] == '\t' || command[idx] == '\n') {
            ++idx;
        }

        if (command[idx] == '\0') {
            break;
        }

        if (count >= max_tokens) {
            return -1;
        }

        size_t len = 0;
        if (command[idx] == '<' || command[idx] == '>') {
            char op = command[idx];
            storage[count][len++] = command[idx++];

            while (command[idx] != '\0') {
                char ch = command[idx];
                if (ch == ' ' || ch == '\t' || ch == '\n') {
                    break;
                }
                if ((ch == '<' || ch == '>') && op == storage[count][0]) {
                    break;
                }
                if (len < MAX_CMD_LENGTH - 1) {
                    storage[count][len++] = ch;
                }
                ++idx;
            }
        } else {
            while (command[idx] != '\0' && command[idx] != ' ' && command[idx] != '\t' && command[idx] != '\n') {
                if (command[idx] == '<') {
                    size_t lookahead = idx + 1;
                    int has_gt = 0;
                    while (command[lookahead] != '\0' && command[lookahead] != ' ' && command[lookahead] != '\t' && command[lookahead] != '\n') {
                        if (command[lookahead] == '>') {
                            has_gt = 1;
                            break;
                        }
                        ++lookahead;
                    }
                    if (!has_gt) {
                        break;
                    }
                }
                if (len < MAX_CMD_LENGTH - 1) {
                    storage[count][len++] = command[idx];
                }
                ++idx;
            }
        }

        storage[count][len] = '\0';
        tokens[count] = storage[count];
        ++count;
    }

    tokens[count] = NULL;
    return count;
}

static int prepare_arguments(const char *command,
                             char *args[],
                             int *argc,
                             int *in_fd,
                             int *out_fd) {
    char *tokens[MAX_TOKENS + 1];
    int token_count = tokenize_command(command, tokens, MAX_TOKENS);

    *argc = 0;
    *in_fd = -1;
    *out_fd = -1;

    if (token_count < 0) {
        report_syntax_error();
        return -1;
    }

    int consumed[MAX_TOKENS] = {0};
    char *redir_paths[2] = {NULL, NULL};

    for (int i = 0; i < token_count; ++i) {
        if (i < MAX_TOKENS && consumed[i]) {
            continue;
        }

        char *tok = tokens[i];
        if (!tok || tok[0] == '\0') {
            continue;
        }

        if (tok[0] == '<' || tok[0] == '>') {
            int is_input = (tok[0] == '<');

            if (tok[1] == '<' || tok[1] == '>') {
                report_syntax_error();
                return -1;
            }

            char *filename = NULL;
            if (tok[1] != '\0') {
                filename = tok + 1;
            } else {
                if (i + 1 >= token_count) {
                    report_syntax_error();
                    return -1;
                }
                if (!tokens[i + 1] || tokens[i + 1][0] == '\0' || tokens[i + 1][0] == '<' || tokens[i + 1][0] == '>') {
                    report_syntax_error();
                    return -1;
                }
                filename = tokens[i + 1];
                consumed[i + 1] = 1;
            }

            if (!filename || filename[0] == '\0') {
                report_syntax_error();
                return -1;
            }

            int slot = is_input ? 0 : 1;
            if (redir_paths[slot] != NULL) {
                report_syntax_error();
                return -1;
            }

            consumed[i] = 1;
            redir_paths[slot] = filename;
        }
    }

    int local_in = -1;
    int local_out = -1;

    if (redir_paths[0]) {
        local_in = open(redir_paths[0], O_RDONLY);
        if (local_in < 0) {
            report_io_error();
            return -2;
        }
    }

    if (redir_paths[1]) {
        local_out = open(redir_paths[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (local_out < 0) {
            report_io_error();
            if (local_in != -1) {
                close(local_in);
            }
            return -2;
        }
    }

    int local_argc = 0;
    for (int i = 0; i < token_count; ++i) {
        if (consumed[i]) {
            continue;
        }
        char *tok = tokens[i];
        if (!tok || tok[0] == '\0') {
            continue;
        }
        if (tok[0] == '<' || tok[0] == '>') {
            continue;
        }
        if (local_argc < MAX_ARGS - 1) {
            args[local_argc++] = tok;
        }
    }

    args[local_argc] = NULL;
    *argc = local_argc;
    *in_fd = local_in;
    *out_fd = local_out;
    return 0;
}

static void execute(char *args[], int in_fd, int out_fd, int wait_child) {
    if (args[0] && strcmp(args[0], "./shell") == 0 && SELF_PATH) {
        args[0] = (char *)SELF_PATH;
    }

    pid_t pid = fork();
    if (pid > 0) {
        if (wait_child) {
            wait(NULL);
        }
        return;
    } else if (pid == 0) {
        if (in_fd != -1 && in_fd != STDIN_FILENO) {
            if (dup2(in_fd, STDIN_FILENO) == -1) {
                perror(DUP2_STDIN_ERROR);
                _exit(1);
            }
        }
        if (out_fd != -1 && out_fd != STDOUT_FILENO) {
            if (dup2(out_fd, STDOUT_FILENO) == -1) {
                perror(DUP2_STDOUT_ERROR);
                _exit(1);
            }
        }
        execvp(args[0], args);
        fprintf(stdout, "Command not found\n");
        fflush(stdout);
        _exit(127);
    } else {
        perror(FORK_ERROR);
        exit(1);
    }
}

static void execute_piped(char *buf, int num_commands) {
    if (!buf || num_commands <= 0) {
        return;
    }

    char *cmds[num_commands];
    int n = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "|", &saveptr);
    while (tok && n < num_commands) {
        while (*tok == ' ' || *tok == '\t' || *tok == '\n') {
            ++tok;
        }
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) {
            --end;
        }
        *end = '\0';
        if (*tok) {
            cmds[n++] = tok;
        }
        tok = strtok_r(NULL, "|", &saveptr);
    }
    num_commands = n;
    if (num_commands == 0) {
        return;
    }

    int pipes_count = (num_commands > 1) ? (num_commands - 1) : 0;
    int (*fd)[2] = NULL;
    if (pipes_count > 0) {
        fd = calloc((size_t)pipes_count, sizeof(int[2]));
        if (!fd) {
            perror("calloc");
            return;
        }
        for (int i = 0; i < pipes_count; ++i) {
            if (pipe(fd[i]) == -1) {
                perror(PIPE_ERROR);
                for (int k = 0; k < i; ++k) {
                    close(fd[k][0]);
                    close(fd[k][1]);
                }
                free(fd);
                return;
            }
            fcntl(fd[i][0], F_SETFD, fcntl(fd[i][0], F_GETFD) | FD_CLOEXEC);
            fcntl(fd[i][1], F_SETFD, fcntl(fd[i][1], F_GETFD) | FD_CLOEXEC);
        }
    }

    int started = 0;
    int error = 0;

    for (int i = 0; i < num_commands; ++i) {
        char tmp[MAX_CMD_LENGTH];
        strncpy(tmp, cmds[i], sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *argvv[MAX_ARGS];
        int argc = 0;
        int redir_in = -1;
        int redir_out = -1;

        int prep = prepare_arguments(tmp, argvv, &argc, &redir_in, &redir_out);
        if (prep != 0) {
            error = 1;
            break;
        }

        if (argc == 0) {
            if (redir_in != -1) {
                close(redir_in);
            }
            if (redir_out != -1) {
                close(redir_out);
            }
            continue;
        }

        int base_in = (i == 0) ? STDIN_FILENO : fd[i - 1][0];
        int base_out = (i == num_commands - 1) ? STDOUT_FILENO : fd[i][1];

        int in_fd = (redir_in != -1) ? redir_in : base_in;
        int out_fd = (redir_out != -1) ? redir_out : base_out;

        execute(argvv, in_fd, out_fd, 0);
        ++started;

        if (redir_in != -1 && redir_in != base_in) {
            close(redir_in);
        }
        if (redir_out != -1 && redir_out != base_out) {
            close(redir_out);
        }
    }

    if (pipes_count > 0) {
        for (int i = 0; i < pipes_count; ++i) {
            close(fd[i][0]);
            close(fd[i][1]);
        }
    }

    for (int i = 0; i < started; ++i) {
        int status;
        (void)wait(&status);
    }

    free(fd);

    if (error) {
        return;
    }
}

static void run_chain(char *chain) {
    trim_spaces(chain);
    if (*chain == '\0') {
        return;
    }

    int pipes_num = count_pipes(chain);

    if (pipes_num == 0) {
        char line[MAX_CMD_LENGTH];
        strncpy(line, chain, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';

        char *args[MAX_ARGS];
        int argc = 0;
        int redir_in = -1;
        int redir_out = -1;

        int prep = prepare_arguments(line, args, &argc, &redir_in, &redir_out);
        if (prep != 0) {
            return;
        }

        if (argc == 0) {
            if (redir_in != -1) {
                close(redir_in);
            }
            if (redir_out != -1) {
                close(redir_out);
            }
            return;
        }

        if (strcmp(args[0], "cd") == 0) {
            if (redir_in != -1) {
                close(redir_in);
            }
            if (redir_out != -1) {
                close(redir_out);
            }

            if (!args[1] || strcmp(args[1], ".") == 0) {
                return;
            }
            if (strcmp(args[1], "..") == 0) {
                if (chdir("..") != 0) {
                    perror("cd");
                }
            } else {
                if (chdir(args[1]) != 0) {
                    perror("cd");
                }
            }
            return;
        }

        int in_fd = (redir_in != -1) ? redir_in : STDIN_FILENO;
        int out_fd = (redir_out != -1) ? redir_out : STDOUT_FILENO;

        execute(args, in_fd, out_fd, 1);

        if (redir_in != -1) {
            close(redir_in);
        }
        if (redir_out != -1) {
            close(redir_out);
        }
    } else {
        char line[MAX_CMD_LENGTH];
        strncpy(line, chain, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        execute_piped(line, pipes_num + 1);
    }
}

int vtsh_run(int argc, char **argv) {
    SELF_PATH = (argc > 0) ? argv[0] : NULL;

    char command[MAX_CMD_LENGTH];
    const char *path = getenv("PATH");
    if (path == NULL || *path == '\0') {
        setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
    }

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    while (1) {
        if (isatty(STDIN_FILENO)) {
            fprintf(stdout, "%s", vtsh_prompt());
            fflush(stdout);
        }

        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }

        char whole[MAX_CMD_LENGTH];
        strncpy(whole, command, sizeof(whole) - 1);
        whole[sizeof(whole) - 1] = '\0';

        char *saveptr = NULL;
        char *chain = strtok_r(whole, ";", &saveptr);
        while (chain) {
            run_chain(chain);
            chain = strtok_r(NULL, ";", &saveptr);
        }
    }

    return 0;
}

#ifndef VTSH_NO_STANDALONE
int main(int argc, char **argv) {
    return vtsh_run(argc, argv);
}
#endif
