#include <asm-generic/errno-base.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <dirent.h>
#include <search.h>
#include "uthash.h"
#include <readline/history.h>

typedef enum {
  BuiltinCmdExit,
  BuiltinCmdEcho,
  BuiltinCmdType,
  BuiltinCmdPwd,
  BuiltinCmdCd,
  BuiltinCmdComplete,
  BuiltinCmdJobs,
  BuiltinCmdHistory,
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
      bool out_append;
      bool err_append;
      bool background_job;
    };
  };
} ParsedArgs;

static int job_index = 0;
typedef struct {
  int job_index;
  char cmd[256];
  pid_t pid;
  UT_hash_handle hh;
} job_entry;
static job_entry *job_table = NULL;

// BuiltinCmdMax is NULL
static const char *builtins[BuiltinCmdMax + 1] = {"exit", "echo", "type", "pwd", "cd", "complete", "jobs", "history", NULL};

typedef bool (*check_seq)(unsigned char);

static bool is_space(unsigned char c) {
  return isspace(c);
}

static bool is_path_seq(unsigned char c) {
  return c == ':';
}

static int split_pipeline(char *line, char **segments, int max) {
  int n = 0;
  char *p = line;
  segments[n++] = p;
  while (*p) {
    if (*p == '\\') { p++; if (*p) p++; continue; }
    if (*p == '\'' || *p == '"') {
      char q = *p++;
      while (*p && *p != q) {
        if (*p == '\\' && q == '"') { p++; if (*p) p++; continue; }
        p++;
      }
      if (*p) p++;
      continue;
    }
    if (*p == '|') {
      *p = '\0';
      p++;
      while (*p && isspace((unsigned char)*p)) p++;
      if (*p && n < max) segments[n++] = p;
    } else {
      p++;
    }
  }
  return n;
}

static bool is_builtin_cmd(char *cmd) {
  for (int i = 0; i < BuiltinCmdMax; i++) {
    if (strcmp(cmd, builtins[i]) == 0) {
      return true;
    }
  }
  return false;
}

static int reparse_args_redir(ParsedArgs *args) {
  for (int i = 0; i < args->n; i++) {
    char *arg = args->buf + args->start[i];
    bool is_out = strcmp(arg, ">") == 0 || strcmp(arg, "1>") == 0;
    bool out_append = strcmp(arg, ">>") == 0 || strcmp(arg, "1>>") == 0;
    bool is_err = strcmp(arg, "2>") == 0;
    bool err_append = strcmp(arg, "2>>") == 0;
    bool background_job = strcmp(arg, "&") == 0;
    if (!is_out && !is_err && !out_append && !err_append && !background_job)
      continue;
    if (background_job) {
      if (args->common_end_idx == args->n - 1)
        args->common_end_idx = i - 1;
      args->background_job = true;
      continue;
    }
    if (i + 1 >= args->n) {
      fprintf(stderr, "syntax error near unexpected token `newline'\n");
      return -1;
    }
    if (args->common_end_idx == args->n - 1)
      args->common_end_idx = i - 1;
    if (is_out || out_append) {
      args->out_redir_idx = i + 1;
      args->out_append = out_append;
    } else {
      args->err_redir_idx = i + 1;
      args->err_append = err_append;
    }
    if (background_job) {
      args->background_job = true;
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

static void handle_echo(ParsedArgs *p, ParsedArgs *_env);
static void handle_type(ParsedArgs *p, ParsedArgs *env);
static void handle_pwd(ParsedArgs *p, ParsedArgs *_env);

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

static void handle_complete(ParsedArgs *p, ParsedArgs *_env) {
  if (p->n < 2) {
    fprintf(stderr, "invalid num of args\n");
    return;
  }

  const char *arg2 = p->buf + p->start[1];
  char *arg3 = p->buf + p->start[2];

  if (strcmp(arg2, "-p") == 0) {
    if (p->n != 3) {
      fprintf(stderr, "invalid num of args\n");
      return;
    }
    ENTRY *found = hsearch((ENTRY){.key = arg3}, FIND);
    if (found && found->data != NULL) {
      printf("complete -C '%s' %s\n", (char *)found->data, arg3);
    } else {
      fprintf(stderr, "complete: %s: no completion specification\n", arg3);
    }
  } else if (strcmp(arg2, "-C") == 0) {
    const char *arg4 = p->buf + p->start[3];
    if (p->n != 4) {
      fprintf(stderr, "invalid num of args\n");
      return;
    }
    ENTRY e = {.key = strdup(arg4), .data = strdup(arg3)};
    hsearch(e, ENTER);
  } else if (strcmp(arg2, "-r") == 0) {
     if (p->n != 3) {
      fprintf(stderr, "invalid num of args\n");
      return;
    }

    ENTRY *found = hsearch((ENTRY){.key = arg3}, FIND);
    if (found && found->data != NULL) {
      free(found->data);
      found->data = NULL;
    }
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

static void handle_history() {
  HIST_ENTRY **list = history_list();
  if (!list) {
    return;
  }
  for (int i = 0; list[i]; i++) {
    printf("%5d  %s\n", i + history_base, list[i]->line);
  }
}

static void setup_redirects(ParsedArgs *p) {
  int out_flags = p->out_append ? (O_WRONLY | O_CREAT | O_APPEND)
                                : (O_WRONLY | O_CREAT | O_TRUNC);
  int err_flags = p->err_append ? (O_WRONLY | O_CREAT | O_APPEND)
                                : (O_WRONLY | O_CREAT | O_TRUNC);
  if (p->out_redir_idx != -1) {
    int fd = open(p->buf + p->start[p->out_redir_idx], out_flags, 0644);
    if (fd < 0) { perror("open redirect"); _exit(1); }
    dup2(fd, STDOUT_FILENO);
    close(fd);
  }
  if (p->err_redir_idx != -1) {
    int fd = open(p->buf + p->start[p->err_redir_idx], err_flags, 0644);
    if (fd < 0) { perror("open redirect"); _exit(1); }
    dup2(fd, STDERR_FILENO);
    close(fd);
  }
}

static void run_builtin(ParsedArgs *p, ParsedArgs *env) {
  char *cmd = p->buf + p->start[0];
  if (strcmp(cmd, "echo") == 0) handle_echo(p, env);
  else if (strcmp(cmd, "type") == 0) handle_type(p, env);
  else if (strcmp(cmd, "pwd") == 0) handle_pwd(p, env);
  else if (strcmp(cmd, "history") == 0) handle_history();
}

static char *resolve_path(char *cmd, ParsedArgs *env) {
  static char path[PATH_MAX];
  for (int i = 0; i < env->n; i++) {
    snprintf(path, sizeof(path), "%s/%s", env->buf + env->start[i], cmd);
    if (access(path, X_OK) == 0) return path;
  }
  return NULL;
}

static int next_job_number() {
  for (int i = 1; ; i++) {
    job_entry *s;
    HASH_FIND_INT(job_table, &i, s);
    if (s == NULL) {
      return i;
    }
  }
}

static void reap_jobs() {
  job_entry *done[256];
  int done_count = 0;  job_entry *s, *tmp;
  HASH_ITER(hh, job_table, s, tmp) {
    if (waitpid(s->pid, NULL, WNOHANG) <= 0) {
      continue;
    }
    char mark = ' ';
    if (s->hh.next == NULL) {
      mark = '+';
    } else if (((job_entry *)(s->hh.next))->hh.next == NULL) {
      mark = '-';
    }
    printf("[%d]%c  %-24s%s\n", s->job_index, mark, "Done", s->cmd);
    done[done_count++] = s;
  }

  for (int i = 0; i < done_count; i++) {
    s = done[i];
    HASH_DEL(job_table, s);
    free(s);   
  }
}

static void handle_jobs() {
  job_entry *done[256];
  int done_count = 0;
  job_entry *s, *tmp;
  for (s = job_table; s != NULL; s = s->hh.next) {
    char mark = ' ';
    if (s->hh.next == NULL) {
      mark = '+';
    } else if (((job_entry *)(s->hh.next))->hh.next == NULL) {
      mark = '-';
    }
    if (waitpid(s->pid, NULL, WNOHANG) > 0) {
      printf("[%d]%c  %-24s%s\n", s->job_index, mark, "Done", s->cmd);
      done[done_count++] = s;
    } else {
      printf("[%d]%c  %-24s%s &\n", s->job_index, mark, "Running", s->cmd);
    }
  }
  for (int i = 0; i < done_count; i++) {
    s = done[i];
    HASH_DEL(job_table, s);
    free(s);   
  }
}

static void run_child(ParsedArgs *p, ParsedArgs *env) {
  setup_redirects(p);
  char *cmd = p->buf + p->start[0];
  if (is_builtin_cmd(cmd)) {
    run_builtin(p, env);
    _exit(0);
  }
  char *path = resolve_path(cmd, env);
  if (!path) {
    fprintf(stderr, "%s: command not found\n", cmd);
    _exit(1);
  }
  char **argv = malloc((p->common_end_idx + 2) * sizeof(*argv));
  for (int i = 0; i <= p->common_end_idx; i++)
    argv[i] = p->buf + p->start[i];
  argv[p->common_end_idx + 1] = NULL;
  execv(path, argv);
  perror("execv");
  _exit(1);
}

static void execute_command(ParsedArgs *p, ParsedArgs *env) {
  char *cmd = p->buf + p->start[0];

  if (strcmp(cmd, "exit") == 0) exit(0);
  if (strcmp(cmd, "cd") == 0) { handle_cd(p, env); return; }
  if (strcmp(cmd, "complete") == 0) { handle_complete(p, env); return; }
  if (strcmp(cmd, "jobs") == 0) { handle_jobs(); return; }

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return; }
  if (pid == 0) run_child(p, env);

  if (p->background_job) {
    job_index = next_job_number();
    job_entry *job = malloc(sizeof(*job));
    job->job_index = job_index;
    job->pid = pid;
    int len = 0;
    for (int i = 0; i <= p->common_end_idx; i++) {
      len += snprintf(job->cmd + len, sizeof(job->cmd) - len, "%s", p->buf + p->start[i]);
      if (i != p->common_end_idx)
        len += snprintf(job->cmd + len, sizeof(job->cmd) - len, " ");
    }
    fprintf(stderr, "[%d] %d\n", job_index, pid);
    HASH_ADD_INT(job_table, job_index, job);
  } else {
    waitpid(pid, NULL, 0);
  }
}

static void execute_pipeline(ParsedArgs **cmds, int n, ParsedArgs *env) {
  int (*pfds)[2] = malloc((n - 1) * sizeof(int[2]));
  for (int i = 0; i < n - 1; i++)
    pipe(pfds[i]);

  pid_t *pids = malloc(n * sizeof(pid_t));
  for (int i = 0; i < n; i++) {
    pids[i] = fork();
    if (pids[i] < 0) { perror("fork"); return; }
    if (pids[i] == 0) {
      if (i > 0) dup2(pfds[i-1][0], STDIN_FILENO);
      if (i < n-1) dup2(pfds[i][1], STDOUT_FILENO);
      for (int j = 0; j < n-1; j++) {
        close(pfds[j][0]);
        close(pfds[j][1]);
      }
      run_child(cmds[i], env);
    }
  }
  for (int j = 0; j < n-1; j++) {
    close(pfds[j][0]);
    close(pfds[j][1]);
  }

  bool bg = cmds[n-1]->background_job;
  if (bg) {
    int jn = next_job_number();
    fprintf(stderr, "[%d] %d\n", jn, pids[0]);
  } else {
    for (int i = 0; i < n; i++)
      waitpid(pids[i], NULL, 0);
  }

  free(pfds);
  free(pids);
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

static ParsedArgs *env_p = NULL;

static char *registered_script;
static ParsedArgs *line_to_complete;

static char *script_generator(const char *text, int state) {
  static FILE *fp = NULL;
  static char line[256];
  char *arg1 = line_to_complete->n > 0
                   ? line_to_complete->buf + line_to_complete->start[0]
                   : "";
  char *arg3 = line_to_complete->n >= 2
                   ? line_to_complete->buf + line_to_complete->start[line_to_complete->n - 2]
                   : "";

  if (state == 0) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "COMP_LINE='%s' COMP_POINT=%lu %s %s %s %s",
             rl_line_buffer, strlen(rl_line_buffer), registered_script, arg1, text, arg3);
    fp = popen(cmd, "r");
  }
  while (fp && fgets(line, sizeof(line), fp)) {
    line[strcspn(line, "\n")] = '\0';
    if (line[0]) {
      return strdup(line);
    }
  }

  if (fp) {
    pclose(fp);
  }

  return NULL;
}

static char *command_generator(const char *text, int state) {
  static char **candidates;
  static int count, pos;

  if (state == 0) {
    for (int i = 0; i < count; i++)
      free(candidates[i]);
    count = 0;
    int cap = 64;
    candidates = malloc(cap * sizeof(*candidates));

    for (int i = 0; i < BuiltinCmdMax; i++) {
      if (strncmp(text, builtins[i], strlen(text)) == 0)
        candidates[count++] = strdup(builtins[i]);
    }
    for (int i = 0; i < env_p->n; i++) {
      DIR *dir = opendir(env_p->buf + env_p->start[i]);
      if (!dir) continue;
      struct dirent *entry;
      while ((entry = readdir(dir)) != NULL) {
        if (strncmp(text, entry->d_name, strlen(text)) != 0)
          continue;
        char fullpath[PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s",
                 env_p->buf + env_p->start[i], entry->d_name);
        if (access(fullpath, X_OK) != 0)
          continue;
        if (count >= cap) {
          cap *= 2;
          candidates = realloc(candidates, cap * sizeof(*candidates));
        }
        candidates[count++] = strdup(entry->d_name);
      }
      closedir(dir);
    }
    pos = 0;
  }

  if (pos < count)
    return strdup(candidates[pos++]);
  return NULL;
}

static char **attempted_completion(const char *text, int start, int end) {
  if (start == 0) {
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, command_generator);
  }
  registered_script = NULL;
  free_parseargs(line_to_complete);
  line_to_complete = parse_args(rl_line_buffer, is_space);
  char *first_word = line_to_complete->n ? line_to_complete->buf + line_to_complete->start[0] : NULL;
  ENTRY *found = hsearch((ENTRY){.key = first_word}, FIND);
  if (found && found->data != NULL) {
    rl_attempted_completion_over = 1;
    registered_script = found->data;
    return rl_completion_matches(text, script_generator);
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  rl_attempted_completion_function = attempted_completion;
  // Flush after every printf
  setbuf(stdout, NULL);

  const char *path = getenv("PATH");
  char *path_d = strdup(path);
  env_p = parse_args(path_d, is_path_seq);
  hcreate(1024);

  char *line;
  while (true) {
    reap_jobs();
    line = readline("$ ");
    if (!line) break;
    if (*line) {
      add_history(line); 
    }

    char *segments[32];
    int nseg = split_pipeline(line, segments, 32);

    if (nseg == 1) {
      ParsedArgs *p = parse_args(segments[0], is_space);
      if (p == NULL) { free(line); continue; }
      reparse_args_redir(p);
      execute_command(p, env_p);
      free_parseargs(p);
    } else {
      ParsedArgs *cmds[32];
      bool valid = true;
      for (int i = 0; i < nseg; i++) {
        cmds[i] = parse_args(segments[i], is_space);
        if (cmds[i] == NULL) { valid = false; break; }
        reparse_args_redir(cmds[i]);
      }
      if (valid) execute_pipeline(cmds, nseg, env_p);
      for (int i = 0; i < nseg; i++)
        free_parseargs(cmds[i]);
    }
    free(line);
  }
  free_parseargs(env_p);

  return 0;
}
