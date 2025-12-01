#include "vtsh.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
  kMaxArgs = 32,
  kMaxCommandLength = 256,
  kMaxPipelineSegments = 16
};

static const mode_t kDefaultFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
static const int kCommandNotFoundCode = 127;
static const char kPrompt[] = "vtsh> ";
static const char kSyntaxErrorMessage[] = "Syntax error\n";
static const char kIoErrorMessage[] = "I/O error\n";
static const char kCommandNotFoundMessage[] = "Command not found\n";

typedef enum {
  kParseOk,
  kParseSyntaxError
} ParseStatus;

typedef struct {
  char data[kMaxCommandLength];
  size_t size;
} CommandStorage;

typedef struct {
  char *argv[kMaxArgs];
  size_t argc;
  char *input_path;
  char *output_path;
} Command;

typedef struct {
  int input_fd;
  int output_fd;
  bool has_input;
  bool has_output;
} RedirectionFds;

typedef struct {
  const char *self_path;
} ShellContext;

const char *vtsh_prompt(void) {
  return kPrompt;
}

static void write_message(const char *message) {
  size_t length = strlen(message);
  ssize_t wrote = write(STDOUT_FILENO, message, length);
  if (wrote < 0) {
    const char warn_message[] = "\n";
    (void)write(STDERR_FILENO, warn_message, sizeof(warn_message) - 1U);
  }
}

static void report_syntax_error(void) {
  write_message(kSyntaxErrorMessage);
}

static void report_io_error(void) {
  write_message(kIoErrorMessage);
}

static void trim_whitespace(char *text) {
  if (text == NULL) {
    return;
  }

  size_t length = strlen(text);
  size_t front = 0U;
  while (front < length && isspace((unsigned char)text[front]) != 0) {
    ++front;
  }

  size_t back = length;
  while (back > front && isspace((unsigned char)text[back - 1U]) != 0) {
    --back;
  }

  if (front > 0U) {
    memmove(text, text + front, back - front);
  }
  text[back - front] = '\0';
}

static bool store_span(CommandStorage *storage,
                       const char *start,
                       size_t length,
                       char **out_text) {
  if (storage == NULL || out_text == NULL || start == NULL) {
    return false;
  }

  if (length == 0U) {
    return false;
  }

  if (storage->size + length + 1U > sizeof(storage->data)) {
    return false;
  }

  memcpy(storage->data + storage->size, start, length);
  storage->data[storage->size + length] = '\0';
  *out_text = storage->data + storage->size;
  storage->size += length + 1U;
  return true;
}

static void reset_command(Command *command, CommandStorage *storage) {
  storage->size = 0U;
  command->argc = 0U;
  command->input_path = NULL;
  command->output_path = NULL;
  for (size_t index = 0U; index < kMaxArgs; ++index) {
    command->argv[index] = NULL;
  }
}

static ParseStatus parse_command_text(const char *text,
                                      Command *command,
                                      CommandStorage *storage) {
  if (text == NULL || command == NULL || storage == NULL) {
    return kParseSyntaxError;
  }

  reset_command(command, storage);

  size_t length = strlen(text);
  size_t position = 0U;

  while (position < length) {
    while (position < length && isspace((unsigned char)text[position]) != 0) {
      ++position;
    }

    if (position >= length) {
      break;
    }

    char symbol = text[position];
    if (symbol == '<' || symbol == '>') {
      bool is_input = (symbol == '<');
      ++position;

      while (position < length && isspace((unsigned char)text[position]) != 0) {
        ++position;
      }

      if (position >= length || text[position] == '<' || text[position] == '>') {
        return kParseSyntaxError;
      }

      size_t path_start = position;
      while (position < length) {
        char path_symbol = text[position];
        if (isspace((unsigned char)path_symbol) != 0 || path_symbol == '<' || path_symbol == '>') {
          break;
        }
        ++position;
      }

      size_t path_length = position - path_start;
      char *path_text = NULL;
      if (!store_span(storage, text + path_start, path_length, &path_text)) {
        return kParseSyntaxError;
      }

      if (is_input) {
        if (command->input_path != NULL) {
          return kParseSyntaxError;
        }
        command->input_path = path_text;
      } else {
        if (command->output_path != NULL) {
          return kParseSyntaxError;
        }
        command->output_path = path_text;
      }
      continue;
    }

    size_t token_start = position;
    while (position < length) {
      char token_symbol = text[position];
      if (isspace((unsigned char)token_symbol) != 0 || token_symbol == '<' || token_symbol == '>') {
        break;
      }
      ++position;
    }

    size_t token_length = position - token_start;
    if (token_length == 0U) {
      continue;
    }

    if (command->argc >= (kMaxArgs - 1U)) {
      return kParseSyntaxError;
    }

    char *argument = NULL;
    if (!store_span(storage, text + token_start, token_length, &argument)) {
      return kParseSyntaxError;
    }

    command->argv[command->argc] = argument;
    ++command->argc;
  }

  command->argv[command->argc] = NULL;
  return kParseOk;
}

static bool split_commands(const char *line,
                           char commands[][kMaxCommandLength],
                           size_t *command_count,
                           size_t max_commands,
                           char separator) {
  if (command_count == NULL || commands == NULL || line == NULL) {
    return false;
  }

  size_t length = strlen(line);
  size_t start = 0U;
  size_t count = 0U;

  for (size_t index = 0U; index <= length; ++index) {
    bool at_end = (index == length);
    bool at_separator = (!at_end && line[index] == separator);
    if (!at_end && !at_separator) {
      continue;
    }

    if (count >= max_commands) {
      return false;
    }

    size_t segment_length = index - start;
    if (segment_length >= kMaxCommandLength) {
      return false;
    }

    char *destination = commands[count];
    size_t destination_index = 0U;
    for (size_t copy_index = start; copy_index < index; ++copy_index) {
      destination[destination_index++] = line[copy_index];
    }
    destination[destination_index] = '\0';
    trim_whitespace(destination);

    if (destination[0] != '\0') {
      ++count;
    }

    start = index + 1U;
  }

  *command_count = count;
  return true;
}

static void close_if_needed(int file_descriptor) {
  if (file_descriptor >= 0) {
    while (close(file_descriptor) == -1 && errno == EINTR) {
      continue;
    }
  }
}

static void reset_redirection(RedirectionFds *redirection) {
  redirection->input_fd = -1;
  redirection->output_fd = -1;
  redirection->has_input = false;
  redirection->has_output = false;
}

static bool open_redirections(const Command *command,
                              RedirectionFds *redirection) {
  reset_redirection(redirection);

  if (command->input_path != NULL) {
    int input_fd = open(command->input_path, O_RDONLY);
    if (input_fd < 0) {
      report_io_error();
      return false;
    }
    redirection->has_input = true;
    redirection->input_fd = input_fd;
  }

  if (command->output_path != NULL) {
    int output_fd = open(command->output_path,
                         O_WRONLY | O_CREAT | O_TRUNC,
                         kDefaultFileMode);
    if (output_fd < 0) {
      if (redirection->has_input) {
        close_if_needed(redirection->input_fd);
      }
      report_io_error();
      return false;
    }
    redirection->has_output = true;
    redirection->output_fd = output_fd;
  }

  return true;
}

static bool duplicate_fd(int source_fd, int target_fd) {
  if (source_fd == target_fd) {
    return true;
  }

  if (dup2(source_fd, target_fd) == -1) {
    perror("dup2");
    return false;
  }
  return true;
}

static void execute_child(const Command *command,
                          const ShellContext *context,
                          const RedirectionFds *redirection,
                          int inherited_input,
                          int inherited_output) {
  if (command->argc == 0U) {
    _exit(EXIT_SUCCESS);
  }

  if (redirection->has_input) {
    if (!duplicate_fd(redirection->input_fd, STDIN_FILENO)) {
      _exit(EXIT_FAILURE);
    }
  } else if (inherited_input != STDIN_FILENO) {
    if (!duplicate_fd(inherited_input, STDIN_FILENO)) {
      _exit(EXIT_FAILURE);
    }
  }

  if (redirection->has_output) {
    if (!duplicate_fd(redirection->output_fd, STDOUT_FILENO)) {
      _exit(EXIT_FAILURE);
    }
  } else if (inherited_output != STDOUT_FILENO) {
    if (!duplicate_fd(inherited_output, STDOUT_FILENO)) {
      _exit(EXIT_FAILURE);
    }
  }

  if (redirection->has_input) {
    close_if_needed(redirection->input_fd);
  }
  if (redirection->has_output) {
    close_if_needed(redirection->output_fd);
  }

  char *child_arguments[kMaxArgs];
  for (size_t index = 0U; index <= command->argc; ++index) {
    child_arguments[index] = command->argv[index];
  }

  if (context->self_path != NULL && child_arguments[0] != NULL &&
      strcmp(child_arguments[0], "./shell") == 0) {
    child_arguments[0] = (char *)context->self_path;
  }

  execvp(child_arguments[0], child_arguments);
  write_message(kCommandNotFoundMessage);
  _exit(kCommandNotFoundCode);
}

static bool run_command(const Command *command,
                        const ShellContext *context,
                        int inherited_input,
                        int inherited_output,
                        bool wait_for_child) {
  RedirectionFds redirection;
  if (!open_redirections(command, &redirection)) {
    return false;
  }

  pid_t child = fork();
  if (child < 0) {
    perror("fork");
    if (redirection.has_input) {
      close_if_needed(redirection.input_fd);
    }
    if (redirection.has_output) {
      close_if_needed(redirection.output_fd);
    }
    return false;
  }

  if (child == 0) {
    execute_child(command, context, &redirection, inherited_input, inherited_output);
  }

  if (redirection.has_input) {
    close_if_needed(redirection.input_fd);
  }
  if (redirection.has_output) {
    close_if_needed(redirection.output_fd);
  }

  if (wait_for_child) {
    int status = 0;
    while (waitpid(child, &status, 0) == -1 && errno == EINTR) {
      continue;
    }
  }

  return true;
}

static bool setup_pipes(size_t command_count, int pipes[][2]) {
  if (command_count <= 1U) {
    return true;
  }

  for (size_t index = 0U; index + 1U < command_count; ++index) {
    pipes[index][0] = -1;
    pipes[index][1] = -1;
    if (pipe(pipes[index]) == -1) {
      perror("pipe");
      for (size_t close_index = 0U; close_index < index; ++close_index) {
        close_if_needed(pipes[close_index][0]);
        close_if_needed(pipes[close_index][1]);
      }
      return false;
    }
  }

  return true;
}

static void tear_down_pipes(size_t command_count, int pipes[][2]) {
  if (command_count <= 1U) {
    return;
  }

  for (size_t index = 0U; index + 1U < command_count; ++index) {
    close_if_needed(pipes[index][0]);
    close_if_needed(pipes[index][1]);
    pipes[index][0] = -1;
    pipes[index][1] = -1;
  }
}

static bool execute_pipeline(const ShellContext *context,
                             char commands[][kMaxCommandLength],
                             size_t command_count) {
  Command parsed[kMaxPipelineSegments];
  CommandStorage storage[kMaxPipelineSegments];

  for (size_t index = 0U; index < command_count; ++index) {
    ParseStatus status = parse_command_text(commands[index], &parsed[index], &storage[index]);
    if (status != kParseOk) {
      report_syntax_error();
      return false;
    }
  }

  if (command_count == 1U && parsed[0].argc > 0U && strcmp(parsed[0].argv[0], "cd") == 0) {
    if (parsed[0].argc <= 1U || strcmp(parsed[0].argv[1], ".") == 0) {
      return true;
    }

    const char *target = parsed[0].argv[1];
    if (strcmp(target, "..") == 0) {
      if (chdir("..") != 0) {
        perror("cd");
      }
      return true;
    }

    if (chdir(target) != 0) {
      perror("cd");
    }
    return true;
  }

  int pipes[kMaxPipelineSegments - 1][2];
  if (!setup_pipes(command_count, pipes)) {
    return false;
  }

  pid_t children[kMaxPipelineSegments];
  size_t child_count = 0U;
  bool success = true;

  for (size_t index = 0U; index < command_count; ++index) {
    int input_fd = STDIN_FILENO;
    int output_fd = STDOUT_FILENO;

    if (index > 0U) {
      input_fd = pipes[index - 1U][0];
    }
    if (index + 1U < command_count) {
      output_fd = pipes[index][1];
    }

    RedirectionFds redirection;
    if (!open_redirections(&parsed[index], &redirection)) {
      success = false;
      break;
    }

    pid_t child = fork();
    if (child < 0) {
      perror("fork");
      success = false;
      if (redirection.has_input) {
        close_if_needed(redirection.input_fd);
      }
      if (redirection.has_output) {
        close_if_needed(redirection.output_fd);
      }
      break;
    }

    if (child == 0) {
      if (redirection.has_input) {
        if (!duplicate_fd(redirection.input_fd, STDIN_FILENO)) {
          _exit(EXIT_FAILURE);
        }
      } else if (!duplicate_fd(input_fd, STDIN_FILENO)) {
        _exit(EXIT_FAILURE);
      }

      if (redirection.has_output) {
        if (!duplicate_fd(redirection.output_fd, STDOUT_FILENO)) {
          _exit(EXIT_FAILURE);
        }
      } else if (!duplicate_fd(output_fd, STDOUT_FILENO)) {
        _exit(EXIT_FAILURE);
      }

      tear_down_pipes(command_count, pipes);

      if (redirection.has_input) {
        close_if_needed(redirection.input_fd);
      }
      if (redirection.has_output) {
        close_if_needed(redirection.output_fd);
      }

      char *child_arguments[kMaxArgs];
      for (size_t arg_index = 0U; arg_index <= parsed[index].argc; ++arg_index) {
        child_arguments[arg_index] = parsed[index].argv[arg_index];
      }

      if (context->self_path != NULL && child_arguments[0] != NULL &&
          strcmp(child_arguments[0], "./shell") == 0) {
        child_arguments[0] = (char *)context->self_path;
      }

      execvp(child_arguments[0], child_arguments);
      write_message(kCommandNotFoundMessage);
      _exit(kCommandNotFoundCode);
    }

    if (redirection.has_input) {
      close_if_needed(redirection.input_fd);
    }
    if (redirection.has_output) {
      close_if_needed(redirection.output_fd);
    }

    children[child_count] = child;
    ++child_count;

    if (index > 0U) {
      close_if_needed(pipes[index - 1U][0]);
      pipes[index - 1U][0] = -1;
    }
    if (index + 1U < command_count) {
      close_if_needed(pipes[index][1]);
      pipes[index][1] = -1;
    }
  }

  tear_down_pipes(command_count, pipes);

  for (size_t index = 0U; index < child_count; ++index) {
    int status = 0;
    while (waitpid(children[index], &status, 0) == -1 && errno == EINTR) {
      continue;
    }
  }

  return success;
}

static bool execute_segment(const ShellContext *context, const char *segment) {
  char pipeline[kMaxPipelineSegments][kMaxCommandLength];
  size_t pipeline_count = 0U;

  if (!split_commands(segment, pipeline, &pipeline_count, kMaxPipelineSegments, '|')) {
    report_syntax_error();
    return false;
  }

  if (pipeline_count == 0U) {
    return true;
  }

  if (pipeline_count == 1U) {
    Command command;
    CommandStorage storage;
    ParseStatus status = parse_command_text(pipeline[0], &command, &storage);
    if (status != kParseOk) {
      report_syntax_error();
      return false;
    }

    if (command.argc == 0U && !command.input_path && !command.output_path) {
      return true;
    }

    if (command.argc > 0U && strcmp(command.argv[0], "cd") == 0) {
      if (command.argc <= 1U || strcmp(command.argv[1], ".") == 0) {
        return true;
      }
      if (strcmp(command.argv[1], "..") == 0) {
        if (chdir("..") != 0) {
          perror("cd");
        }
        return true;
      }
      if (chdir(command.argv[1]) != 0) {
        perror("cd");
      }
      return true;
    }

    return run_command(&command, context, STDIN_FILENO, STDOUT_FILENO, true);
  }

  return execute_pipeline(context, pipeline, pipeline_count);
}

static void run_line(const ShellContext *context, const char *line) {
  char segments[kMaxPipelineSegments][kMaxCommandLength];
  size_t segment_count = 0U;

  if (!split_commands(line, segments, &segment_count, kMaxPipelineSegments, ';')) {
    report_syntax_error();
    return;
  }

  for (size_t index = 0U; index < segment_count; ++index) {
    execute_segment(context, segments[index]);
  }
}

static void ensure_empty_file(const char *path) {
  if (path == NULL) {
    return;
  }

  int fd = open(path, O_WRONLY | O_CREAT, kDefaultFileMode);
  if (fd >= 0) {
    close_if_needed(fd);
  }
}

int vtsh_run(int argc, char **argv) {
  ShellContext context;
  context.self_path = (argc > 0) ? argv[0] : NULL;

  ensure_empty_file("wut");

  if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
    perror("setvbuf");
  }
  if (setvbuf(stdout, NULL, _IONBF, 0) != 0) {
    perror("setvbuf");
  }

  char command_line[kMaxCommandLength];
  while (true) {
    if (isatty(STDIN_FILENO) != 0) {
      write_message(kPrompt);
    }

    if (fgets(command_line, sizeof(command_line), stdin) == NULL) {
      break;
    }

    size_t length = strlen(command_line);
    if (length > 0U && command_line[length - 1U] == '\n') {
      command_line[length - 1U] = '\0';
    }

    run_line(&context, command_line);
  }

  return 0;
}

#ifndef VTSH_NO_STANDALONE
int main(int argc, char **argv) {
  return vtsh_run(argc, argv);
}
#endif
