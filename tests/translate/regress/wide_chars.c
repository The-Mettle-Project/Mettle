/* Wide characters. wchar_t is 16 bits here, as on every Windows toolchain, so
 * a wide literal is UTF-16: the source arrives as UTF-8 bytes and has to be
 * decoded and re-encoded. `L"你"` is three bytes of source and one unit.
 *
 * The prefix binds to the literal, not to an identifier that happens to be one
 * letter long, which is why the lexer looks at what follows the L.
 */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const wchar_t *g = L"global";
static wchar_t buf[32];

int main(void) {
  wchar_t c = L'x';
  wchar_t nul = L'\0';
  wchar_t esc = L'\n';
  wchar_t s[] = L"hello";
  const wchar_t *p = L"pointer";
  const wchar_t *mix = L"abé你z"; /* one, two and three byte sources */
  unsigned i;

  printf("%d %d %d\n", (int)c, (int)nul, (int)esc);
  printf("%d %d %d\n", (int)wcslen(s), (int)wcslen(p), (int)wcslen(g));

  /* sizeof counts units, and the array has room for its terminator */
  printf("%d %d %d\n", (int)sizeof(wchar_t), (int)sizeof s, (int)sizeof(L'a'));

  for (i = 0; i < wcslen(mix); i++)
    printf("%04X ", (unsigned)mix[i]);
  printf("\n");
  printf("%d\n", (int)wcslen(mix));

  printf("%d %d %d\n", wcscmp(s, L"hello"), wcscmp(s, L"hellp"),
         wcsncmp(s, L"help", 3));
  printf("%d %d\n", wcschr(s, L'l') != 0, wcschr(s, L'z') != 0);
  printf("%d\n", (int)(wcsrchr(s, L'l') - s));
  printf("%d\n", wcsstr(p, L"int") != 0);

  wcscpy(buf, L"copy");
  wcscat(buf, L"-cat");
  printf("%d %d\n", (int)wcslen(buf), wcscmp(buf, L"copy-cat"));

  wmemset(buf, L'q', 4);
  buf[4] = 0;
  printf("%d %d\n", (int)wcslen(buf), (int)buf[0]);

  {
    wchar_t dst[8];
    wmemcpy(dst, L"abc", 4);
    printf("%d %d\n", wcscmp(dst, L"abc"), wmemcmp(dst, L"abc", 3));
  }

  /* wide and narrow literals with the same text stay separate objects */
  printf("%d %d\n", (int)sizeof("hello"), (int)sizeof(L"hello"));
  printf("%d %d\n", (int)strlen("hello"), (int)wcslen(L"hello"));
  return 0;
}
