/* A string literal has type char[N], not char*. Typing it as a pointer made
   sizeof("") come out as 8, which broke cJSON's ensure() arithmetic
   (output_length + sizeof("\"\"") asked for five bytes too many and the
   exact-fit PrintPreallocated buffer was refused). */
#include <stdio.h>

int main(void) {
  char a[7];
  printf("%d %d %d %d\n", (int)sizeof(""), (int)sizeof("\"\""),
         (int)sizeof("hello"), (int)sizeof("x" "yz"));
  printf("%d %d\n", (int)sizeof(a), (int)sizeof("hello" + 0));
  const char *p = "still a pointer at use sites";
  printf("%c\n", p[0]);
  return 0;
}
