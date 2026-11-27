/* _popen / _pclose: a pipe to a command run through the shell.
 *
 * The child must inherit its end of the pipe and must not inherit ours, or it
 * keeps the write end open and the reader never reaches end-of-file. _pclose
 * waits for the child and reports what it exited with.
 *
 * Every command here is a cmd.exe builtin, so the test needs nothing installed.
 */
#include <stdio.h>
#include <string.h>

static int drain(FILE *p) {
  char line[256];
  int n = 0;
  while (fgets(line, sizeof line, p))
    n++;
  return n;
}

int main(void) {
  char line[256];
  FILE *p;
  int n, rc;
  size_t len;

  p = _popen("echo hello from the pipe", "r");
  if (!p) {
    printf("popen failed\n");
    return 1;
  }
  n = 0;
  while (fgets(line, sizeof line, p)) {
    len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    printf("[%s]\n", line);
    n++;
  }
  rc = _pclose(p);
  printf("%d %d\n", n, rc);

  /* several lines, and an exit code that has to come back */
  p = _popen("echo one& echo two& echo three& exit /b 3", "r");
  n = drain(p);
  rc = _pclose(p);
  printf("%d %d\n", n, rc);

  /* a command that does not exist: the pipe opens, the shell fails */
  p = _popen("this_command_does_not_exist_xyz 2>nul", "r");
  n = drain(p);
  rc = _pclose(p);
  printf("%d %d\n", n, rc != 0);

  /* the unprefixed spelling */
  p = popen("echo via macro", "r");
  n = 0;
  if (fgets(line, sizeof line, p))
    n = (int)(strlen(line) > 0);
  printf("%d %d\n", n, pclose(p));

  /* an unsupported mode is a null return, not a crash */
  printf("%d\n", _popen("echo x", "z") == NULL);
  return 0;
}
