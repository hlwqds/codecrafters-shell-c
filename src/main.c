#include <asm-generic/errno-base.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
  BuiltinCmdExit,
  BuiltinCmdEcho,
  BuiltinCmdType,
  BuiltinCmdMax,
} BuiltinCmd;

typedef struct {
  int n;
  int *start;
  char *buf;
  int buf_len;
} ParsedArgs;

static const char *builtins[BuiltinCmdMax] = {"exit", "echo", "type"};

typedef bool (*check_seq)(unsigned char);

static bool is_space(unsigned char c) {
  return isspace(c);
}

static bool is_path_seq(unsigned char c) {
  return c == ':';
}

static bool is_builtin_cmd(char *cmd) {
  for (int i = 0; i < BuiltinCmdMax; i++) {
    if (strcmp(cmd, builtins[i]) == 0) {
      return true;
    }
  }
  return false;
}

static ParsedArgs *parse_args(char *cmd, check_seq cb) {
  int count = 0;
  int len = strlen(cmd);
  bool in_arg = false;

  for (int i = 0 ; i < len; i++) {
    if (cb((unsigned char)cmd[i])) {
      in_arg = false;
    } else if (!in_arg) {
      count++;
      in_arg = true;
    }
  }

  if (count == 0) {
    return NULL;
  }

  ParsedArgs *p = calloc(1, sizeof(*p));
  if (p == NULL) {
    perror("calloc");
    exit(1);
  }

  p->buf = cmd;
  p->buf_len = len;
  p->n = count;
  p->start = calloc(count, sizeof(*p->start));
  if (p->start == NULL) {
    perror("calloc");
    free(p);
    exit(1);
  }

  count = 0;
  in_arg = false;
  for (int i = 0; i < len; i++) {
    if (cb((unsigned char)cmd[i])) {
      in_arg = false;
      cmd[i] = '\0';
    } else if (!in_arg) {
      p->start[count++] = i;
      in_arg = true;
    }
  }
  return p;
}

static void free_parseargs(ParsedArgs *p) {
  if (p != NULL) {
      if (p->start) {
        free(p->start);
      }
    free(p);
  }
}

static void handle_echo(ParsedArgs *p) {
  for (int i = 1; i < p->n; i++) {
    printf("%s", p->buf + p->start[i]);
    if (i != p->n - 1) {
      printf(" ");
    }
  }
  printf("\n");
}

static void handle_type(ParsedArgs *p, ParsedArgs *env) {
  if (p->n != 2) {
    fprintf(stderr, "invalid num of args\n");
    return;
  }

  char *cmd = p->buf + p->start[1];
  if (is_builtin_cmd(cmd)) {
    printf("%s is a shell builtin\n", cmd);
    return;
  }

  char path[PATH_MAX];
  for (int i = 0; i < env->n; i++) {
    sprintf(path, "%s/%s", env->buf + env->start[i], cmd);
    if (access(path, X_OK) == 0) {
      printf("%s is %s\n", cmd, path);
      return;
    }
  }

  fprintf(stderr, "%s: not found\n", cmd);
  return;
}

static bool is_externel(char *cmd, ParsedArgs *env) {
  char path[PATH_MAX];
  for (int i = 0; i < env->n; i++) {
    snprintf(path, sizeof(path), "%s/%s", env->buf + env->start[i], cmd);
    if (access(path, X_OK) == 0) {
      return true;
    }
  }
  return false;
}

static void handle_external(ParsedArgs *p, ParsedArgs *env) {
  char path[PATH_MAX];
  char *cmd = p->buf + p->start[0];
  for (int i = 0; i < env->n; i++) {
    snprintf(path, sizeof(path), "%s/%s", env->buf + env->start[i], cmd);
    if (access(path, X_OK) == 0) {
      break;
    }
  }
  char **argv = malloc((p->n + 1) * sizeof(*argv));
  if (argv == NULL) {
    perror("malloc");
    exit(1);
  }
  for (int i = 0; i < p->n; i++) {
    argv[i] = p->buf + p->start[i];
  }
  argv[p->n] = NULL;
  pid_t pid = fork();
  if (pid == 0) {
    execv(path, argv);
    perror("execv");
    exit(1);
  } else if (pid < 0) {
    perror("fork");
    exit(1);
  }
  if (waitpid(pid, NULL, 0) == -1) {
    perror("waitpid");
    exit(1);
  }
  free(argv);
  return;
}

int main(int argc, char *argv[]) {
  // Flush after every printf
  setbuf(stdout, NULL);

  const char *path = getenv("PATH");
  char *path_d = strdup(path);
  ParsedArgs *env_p = parse_args(path_d, is_path_seq);

  char input[100];
  while (1) {  
    printf("$ ");

    fgets(input, sizeof(input), stdin);
    input[sizeof(input) - 1] = '\0';
    input[strlen(input) - 1] = '\0';
    ParsedArgs *p = parse_args(input, is_space);
    if (p == NULL) {
      continue;
    }
    char *cmd = p->buf + p->start[0];
    if (strcmp(cmd, builtins[BuiltinCmdExit]) == 0) {
      exit(0);
    } else if (strcmp(cmd, builtins[BuiltinCmdEcho]) == 0) {
      handle_echo(p);
    } else if (strcmp(cmd, builtins[BuiltinCmdType]) == 0) {
      handle_type(p, env_p);
    } else if (is_externel(cmd, env_p)) {
      handle_external(p, env_p);
    } else {
      printf("%s: command not found\n", input);
    }
    free_parseargs(p);
  }

  return 0;
}
