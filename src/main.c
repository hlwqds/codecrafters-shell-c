#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

typedef enum {
  BuiltinCmdExit,
  BuiltinCmdEcho,
  BuiltinCmdMax,
} BuiltinCmd;

typedef struct {
  int n;
  int *start;
  char *buf;
  int buf_len;
} ParsedArgs;

static const char *builtins[BuiltinCmdMax] = {"exit", "echo"};

static ParsedArgs *parse_args(char *cmd) {
  int count = 0;
  int len = strlen(cmd);
  bool in_arg = false;

  for (int i = 0 ; i < len; i++) {
    if (isspace((unsigned char)cmd[i])) {
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
    if (isspace((unsigned char)cmd[i])) {
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

int main(int argc, char *argv[]) {
  // Flush after every printf
  setbuf(stdout, NULL);

  char input[100];
  while (1) {  
    printf("$ ");

    fgets(input, sizeof(input), stdin);
    input[sizeof(input) - 1] = '\0';
    input[strlen(input) - 1] = '\0';
    ParsedArgs *p = parse_args(input);
    if (p == NULL) {
      continue;
    }
    char *cmd = p->buf + p->start[0];
    if (strcmp(cmd, builtins[BuiltinCmdExit]) == 0) {
      exit(0);
    } else if (strcmp(cmd, builtins[BuiltinCmdEcho]) == 0) {
      handle_echo(p);
    } else {
      printf("%s: command not found\n", input);
    }
    free_parseargs(p);
  }

  return 0;
}
