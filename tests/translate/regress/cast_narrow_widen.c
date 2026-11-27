/* Truncate, then extend: `(int64_t)(int32_t)u` on an unsigned 32-bit value.
 *
 * The pair drops the high half and then brings bit 31 down the whole register,
 * and no single cast says that. The optimizer used to compose the two into the
 * narrow one and lose the sign extension, so a -4 read back through a uint32
 * came out as 4294967292. Mettle's own linker computes relocation addends that
 * way and rejected every REL32 as out of range.
 */
#include <stdio.h>
#include <stdint.h>

static unsigned char buf[4] = {0xFC, 0xFF, 0xFF, 0xFF};

static uint32_t rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int main(void) {
  uint32_t u = rd32(buf);
  uint64_t target = 0x140050000ull, patch = 0x140001000ull;
  int64_t addend = (int64_t)(int32_t)rd32(buf);
  int64_t value = (int64_t)target + addend - (int64_t)(patch + 4u);

  printf("%lld %lld\n", (long long)(int64_t)(int32_t)rd32(buf),
         (long long)(int64_t)(int32_t)u);
  printf("%lld %d\n", (long long)value,
         (value < INT32_MIN || value > INT32_MAX) ? 0 : 1);
  printf("%u %lld\n", u, (long long)(int64_t)u);

  /* Every narrow-then-widen pair, so the fix stays pinned to the one case
     that needed it. A signed char read back through unsigned char is the one
     that must still compose: `(int)(unsigned char)c` is 195, not -61. */
  {
    static char cbuf[2] = {0x41, (char)0xC3};
    static uint32_t ubuf[1] = {0xFFFFFFFCu};
    printf("%d %lld %lld %lld\n", (int)(unsigned char)cbuf[1],
           (long long)(int8_t)ubuf[0], (long long)(int16_t)ubuf[0],
           (long long)(int32_t)ubuf[0]);
    printf("%lld %lld %lld\n", (long long)(uint8_t)ubuf[0],
           (long long)(uint16_t)ubuf[0], (long long)(uint32_t)ubuf[0]);
  }

  /* the same shape one width down, and the unsigned counterpart */
  printf("%lld %lld\n", (long long)(int64_t)(int16_t)(uint16_t)0xFFFCu,
         (long long)(int64_t)(uint16_t)(uint32_t)0xFFFFFFFCu);
  printf("%lld\n", (long long)(int64_t)(int8_t)(uint8_t)0xFCu);
  return 0;
}
