#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  BuiltinCmdExit,
  BuiltinCmdMax,
} BuiltinCmd;

static const char *builtins[BuiltinCmdMax] = {"exit"};

int main(int argc, char *argv[]) {
  // Flush after every printf
  setbuf(stdout, NULL);

  char input[100];
  while (1) {  
    printf("$ ");

    fgets(input, sizeof(input), stdin);
    input[sizeof(input) - 1] = '\0';
    input[strlen(input) - 1] = '\0';
    if (strcmp(input, builtins[BuiltinCmdExit]) == 0) {
      exit(0);
    }
    printf("%s: command not found\n", input);
  }

  return 0;
}
