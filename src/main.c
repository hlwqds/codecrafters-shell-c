#include <asm-generic/errno-base.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

typedef enum {
  BuiltinCmdExit,
  BuiltinCmdEcho,
  BuiltinCmdType,
  BuiltinCmdPwd,
  BuiltinCmdCd,
  BuiltinCmdMax,
} BuiltinCmd;

typedef struct {
  int n;
  int *start;
  char *buf;
  int buf_len;
  union {
    struct {
      int common_end_idx;
      int out_redir_idx;
      int err_redir_idx;     
    };
  };
} ParsedArgs;

static const char *builtins[BuiltinCmdMax] = {"exit", "echo", "type", "pwd", "cd"};

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

static int generate_out_redir_fd(ParsedArgs *args) {
  if (args->out_redir_idx == -1) {
    return -1;  
  }

  char *path = args->buf + args->out_redir_idx;
  return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

static int reparse_args_redir(ParsedArgs *args) {
  for (int i = 0; i < args->n; i++) {
    char *arg = args->buf + args->start[i];
    if (strcmp(arg, ">") == 0 || strcmp(arg, "1>") == 0) {
      if (i + 1 >= args->n) {
        perror("> has no arg");
        return -1;
      }
      args->common_end_idx = i - 1;
      args->out_redir_idx = i + 1;
      break;
    }
  }
  return 0;
}

static ParsedArgs *parse_args(char *cmd, check_seq cb) {
  int len = strlen(cmd);

  // First pass: count arguments (quote-aware)
  int count = 0;
  int i = 0;
  while (i < len) {
    while (i < len && cb((unsigned char)cmd[i]))
      i++;
    if (i >= len)
      break;
    count++;
    while (i < len && !cb((unsigned char)cmd[i])) {
      if (cmd[i] == '\\') {
        i += 2;
        continue;
      }
      if (cmd[i] == '\'' || cmd[i] == '"') {
        char q = cmd[i++];
        while (i < len && cmd[i] != q) {
          if (cmd[i] == '\\' && q == '"') {
            i += 2;
            continue;
          }
          i++;
        }
        if (i < len)
          i++;
      } else {
        i++;
      }
    }
  }

  if (count == 0)
    return NULL;

  ParsedArgs *p = calloc(1, sizeof(*p));
  if (p == NULL) {
    perror("calloc");
    exit(1);
  }

  p->buf = calloc(len + 1, sizeof(char));
  if (p->buf == NULL) {
    perror("calloc");
    free(p);
    exit(1);
  }

  p->start = calloc(count, sizeof(*p->start));
  if (p->start == NULL) {
    perror("calloc");
    free(p->buf);
    free(p);
    exit(1);
  }

  p->n = count;
  p->common_end_idx = count - 1;
  p->out_redir_idx = -1;
  p->err_redir_idx = -1;

  // Second pass: extract arguments, stripping quotes
  int buf_pos = 0;
  i = 0;
  count = 0;
  while (i < len) {
    while (i < len && cb((unsigned char)cmd[i]))
      i++;
    if (i >= len)
      break;

    p->start[count++] = buf_pos;
    while (i < len && !cb((unsigned char)cmd[i])) {
      if (cmd[i] == '\\') {
        i++;
        p->buf[buf_pos++] = cmd[i];
        i++;
        continue;
      }
      if (cmd[i] == '\'' || cmd[i] == '"') {
        char q = cmd[i++];
        while (i < len && cmd[i] != q) {
          if (cmd[i] == '\\' && q == '"') {
            i++;
            p->buf[buf_pos++] = cmd[i];
            i++;
            continue;
          }
          p->buf[buf_pos++] = cmd[i++];
        }
        if (i < len)
          i++;
      } else {
        p->buf[buf_pos++] = cmd[i++];
      }
    }
    p->buf[buf_pos++] = '\0';
  }
  p->buf_len = buf_pos;
  return p;
}

static void free_parseargs(ParsedArgs *p) {
  if (p != NULL) {
    if (p->buf)
      free(p->buf);
    if (p->start)
      free(p->start);
    free(p);
  }
}

typedef void (*handle_cmd)(ParsedArgs *, ParsedArgs *);

static void handle_cd(ParsedArgs *p, ParsedArgs *_env) {
  if (p->n != 2) {
    fprintf(stderr, "invalid num of args\n");
    return;
  }
  char buf[PATH_MAX];
  char *arg1 = p->buf + p->start[1];
  if (*arg1 == '~') {
    char *home = getenv("HOME");
    if (home == NULL) {
      perror("getenv HOME");
      return;
    }
    snprintf(buf, sizeof(buf), "%s%s", home, arg1 + 1);
  } else {
    snprintf(buf, sizeof(buf), "%s", arg1);
  }

  if (chdir(buf) == -1) {
    fprintf(stderr, "cd: %s: No such file or directory\n", buf);
    return;
  }
  return;
}

static void handle_pwd(ParsedArgs *p, ParsedArgs *_env) {
  if (p->n != 1) {
    fprintf(stderr, "invalid num of args\n");
    return;
  }
  char buf[PATH_MAX];
  if (getcwd(buf, sizeof(buf)) == NULL) {
    perror("getcwd");
    return;
  }
  printf("%s\n", buf);
  return;
}

static void handle_cmd_cb(ParsedArgs *p, ParsedArgs *env, handle_cmd cb) {
  if (p->out_redir_idx == -1) {
    cb(p, env);
    return;
  }
  int fd = open(p->buf + p->start[p->out_redir_idx],
                O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror("open redirect");
    return;
  }
  int saved = dup(STDOUT_FILENO);
  if (saved < 0) {
    perror("dup");
    close(fd);
    return;
  }
  if (dup2(fd, STDOUT_FILENO) < 0) {
    perror("dup2");
    close(fd);
    close(saved);
    return;
  }
  close(fd);

  cb(p, env);

  if (dup2(saved, STDOUT_FILENO) < 0) {
    perror("dup2 restore");
  }
  close(saved);
  return;
}

static void handle_echo(ParsedArgs *p, ParsedArgs *_env) {
  
  for (int i = 1; i <= p->common_end_idx; i++) {
    printf("%s", p->buf + p->start[i]);
    if (i != p->common_end_idx) {
      printf(" ");
    }
  }
  printf("\n");
  return;
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
  for (int i = 0; i <= p->common_end_idx; i++) {
    argv[i] = p->buf + p->start[i];
  }
  argv[p->common_end_idx + 1] = NULL;
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

  char input[4096];
  while (1) {  
    printf("$ ");

    if (fgets(input, sizeof(input), stdin) == NULL)
      break;
    input[strcspn(input, "\n")] = '\0';
    ParsedArgs *p = parse_args(input, is_space);
    if (p == NULL) {
      continue;
    }
    if (reparse_args_redir(p) == -1) {
      continue;
    }
    char *cmd = p->buf + p->start[0];
    if (strcmp(cmd, builtins[BuiltinCmdExit]) == 0) {
      exit(0);
    } else if (strcmp(cmd, builtins[BuiltinCmdEcho]) == 0) {
      handle_cmd_cb(p, env_p, handle_echo);
    } else if (strcmp(cmd, builtins[BuiltinCmdType]) == 0) {
      handle_cmd_cb(p, env_p, handle_type);
    } else if (strcmp(cmd, builtins[BuiltinCmdPwd]) == 0) {
      handle_cmd_cb(p, env_p, handle_pwd);
    } else if (strcmp(cmd, builtins[BuiltinCmdCd]) == 0) {
      handle_cmd_cb(p, env_p, handle_cd);
    } else if (is_externel(cmd, env_p)) {
      handle_cmd_cb(p, env_p, handle_external);
    } else {
      fprintf(stderr, "%s: command not found\n", input);
    }
    free_parseargs(p);
  }

  return 0;
}
