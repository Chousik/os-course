#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAX_ARGS 10
#define MAX_CMD_LENGTH 100
#define TOKENS_DELIMITERS " \t\n"
#define INVITED " $ "
#define EOF_ERROR "\x53\x65\x76\x61\x20\x35\x32\x20"
#define INPUT_ERROR "invalid input"
#define FORK_ERROR "fork error"
#define EXECUTE_TIME "Execute time: %.3f ms\n"
#define PIPE_ERROR "Pipe creation failed"
#define DUP2_STDIN_ERROR "dup2(stdin) failed"
#define DUP2_STDOUT_ERROR "dup2(stdout) failed"

static void trim_spaces(char *s) {
  if (!s) return;
  char *p = s;
  while (*p==' '||*p=='\t'||*p=='\n') p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t len = strlen(s);
  while (len && (s[len-1]==' '||s[len-1]=='\t'||s[len-1]=='\n')) s[--len] = '\0';
}

static int count_pipes(const char *s) {
  int c = 0;
  for (const char *p = s; *p; ++p) if (*p == '|') c++;
  return c;
}

static void parse_command(char *command, char *args[], int *count) {
  char *token; int i = 0;
  token = strtok(command, TOKENS_DELIMITERS);
  while (token != NULL && i < MAX_ARGS - 1) {
    args[i++] = token;
    token = strtok(NULL, TOKENS_DELIMITERS);
  }
  args[i] = NULL;
  *count = i;
}

/* Удаляет в argv пары "< file" и "> file", открывает файлы, возвращает fd через *in_fd/*out_fd (или -1 если не задано) */
static int apply_redirs(char *argv[], int *in_fd, int *out_fd) {
  *in_fd = -1; *out_fd = -1;
  for (int i = 0; argv[i]; ++i) {
    if (strcmp(argv[i], "<") == 0) {
      if (!argv[i+1]) { fprintf(stderr, "syntax error: expected filename after '<'\n"); return -1; }
      int fd = open(argv[i+1], O_RDONLY);
      if (fd < 0) { perror("open <"); return -1; }
      *in_fd = fd;
      for (int j = i; argv[j]; ++j) argv[j] = argv[j+2];
      i--;
    } else if (strcmp(argv[i], ">") == 0) {
      if (!argv[i+1]) { fprintf(stderr, "syntax error: expected filename after '>'\n"); return -1; }
      int fd = open(argv[i+1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0) { perror("open >"); return -1; }
      *out_fd = fd;
      for (int j = i; argv[j]; ++j) argv[j] = argv[j+2];
      i--;
    }
  }
  return 0;
}

static void execute(char *args[], int in_fd, int out_fd, int wait_child) {
  pid_t pid = fork();
  if (pid > 0) {
    if (wait_child) wait(NULL);
    return;
  } else if (pid == 0) {
    if (in_fd != STDIN_FILENO) {
      if (dup2(in_fd, STDIN_FILENO) == -1) { perror(DUP2_STDIN_ERROR); _exit(1); }
      if (in_fd != -1) close(in_fd);
    }
    if (out_fd != STDOUT_FILENO) {
      if (dup2(out_fd, STDOUT_FILENO) == -1) { perror(DUP2_STDOUT_ERROR); _exit(1); }
      if (out_fd != -1) close(out_fd);
    }
    execvp(args[0], args);
    perror(INPUT_ERROR);
    _exit(127);
  } else {
    perror(FORK_ERROR);
    exit(1);
  }
}

static void executePiped(char *buf, int num_commands) {
  if (!buf || num_commands <= 0) return;

  char *cmds[num_commands];
  int n = 0;
  char *saveptr = NULL;
  char *tok = strtok_r(buf, "|", &saveptr);
  while (tok && n < num_commands) {
    while (*tok==' '||*tok=='\t'||*tok=='\n') tok++;
    char *end = tok + strlen(tok);
    while (end>tok && (end[-1]==' '||end[-1]=='\t'||end[-1]=='\n')) end--;
    *end = '\0';
    if (*tok) cmds[n++] = tok;
    tok = strtok_r(NULL, "|", &saveptr);
  }
  num_commands = n;
  if (num_commands == 0) return;

  int pipes_count = (num_commands > 1) ? (num_commands - 1) : 0;
  int fd[pipes_count][2];
  for (int i = 0; i < pipes_count; i++) {
    if (pipe(fd[i]) == -1) {
      perror(PIPE_ERROR);
      for (int k = 0; k < i; k++) { close(fd[k][0]); close(fd[k][1]); }
      return;
    }
    fcntl(fd[i][0], F_SETFD, fcntl(fd[i][0], F_GETFD) | FD_CLOEXEC);
    fcntl(fd[i][1], F_SETFD, fcntl(fd[i][1], F_GETFD) | FD_CLOEXEC);
  }

  for (int i = 0; i < num_commands; i++) {
    char tmp[MAX_CMD_LENGTH];
    strncpy(tmp, cmds[i], sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';

    char *argvv[MAX_ARGS];
    int argc = 0;
    parse_command(tmp, argvv, &argc);
    if (!argvv[0]) continue;

    int redir_in = -1, redir_out = -1;
    if (apply_redirs(argvv, &redir_in, &redir_out) < 0) {
      continue;
    }

    int base_in  = (i == 0) ? STDIN_FILENO : fd[i-1][0];
    int base_out = (i == num_commands-1) ? STDOUT_FILENO : fd[i][1];

    int in_fd  = (redir_in  != -1) ? redir_in  : base_in;
    int out_fd = (redir_out != -1) ? redir_out : base_out;

    execute(argvv, in_fd, out_fd, 0);
  }

  for (int i = 0; i < pipes_count; i++) {
    close(fd[i][0]);
    close(fd[i][1]);
  }

  for (int i = 0; i < num_commands; i++) {
    int st; (void)wait(&st);
  }
}

static void run_chain(char *chain) {
  trim_spaces(chain);
  if (*chain == '\0') return;

  int pipes_num = count_pipes(chain);

  if (pipes_num == 0) {
    char line[MAX_CMD_LENGTH];
    strncpy(line, chain, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    char *args[MAX_ARGS];
    int argc = 0;
    parse_command(line, args, &argc);
    if (!args[0]) return;

    if (strcmp(args[0], "cd") == 0) {
      if (!args[1] || strcmp(args[1], ".") == 0) return;
      if (strcmp(args[1], "..") == 0) {
        if (chdir("..") != 0) perror("cd");
      } else {
        if (chdir(args[1]) != 0) perror("cd");
      }
      return;
    }

    int redir_in = -1, redir_out = -1;
    if (apply_redirs(args, &redir_in, &redir_out) < 0) return;

    int in_fd  = (redir_in  != -1) ? redir_in  : STDIN_FILENO;
    int out_fd = (redir_out != -1) ? redir_out : STDOUT_FILENO;

    execute(args, in_fd, out_fd, 1);
  } else {
    char line[MAX_CMD_LENGTH];
    strncpy(line, chain, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    executePiped(line, pipes_num + 1);
  }
}

int main(void) {
  char command[MAX_CMD_LENGTH];
  const char *username = getenv("USER");
  struct timeval start_time, end_time;

  while (1) {
    fprintf(stdout, "%s%s", username ? username : "user", INVITED);
    fflush(stdout);

    if (fgets(command, sizeof(command), stdin) == NULL) {
      fprintf(stdout, "%s", EOF_ERROR);
      break;
    }

    gettimeofday(&start_time, NULL);

    char whole[MAX_CMD_LENGTH];
    strncpy(whole, command, sizeof(whole) - 1);
    whole[sizeof(whole) - 1] = '\0';

    char *saveptr = NULL;
    char *chain = strtok_r(whole, ";", &saveptr);
    while (chain) {
      run_chain(chain);
      chain = strtok_r(NULL, ";", &saveptr);
    }

    gettimeofday(&end_time, NULL);

    long seconds = end_time.tv_sec - start_time.tv_sec;
    long microseconds = end_time.tv_usec - start_time.tv_usec;
    double execute_time = seconds * 1000.0 + microseconds / 1000.0;
    fprintf(stdout, EXECUTE_TIME, execute_time);
    fflush(stdout);
  }

  return 0;
}
