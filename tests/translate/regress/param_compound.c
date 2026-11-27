/* Read-modify-write on a parameter must update its value handle. Taking the
 * parameter's address and storing through it can leave the register value stale. */
static int decimal_digits(unsigned long long value) {
  int count = 0;
  while (value) {
    value /= 10;
    count++;
    if (count > 20)
      return -1;
  }
  return count;
}

static int countdown(int value) {
  int count = 0;
  while (value--)
    count++;
  return count;
}

int main(void) {
  return decimal_digits(1333333333ull) == 10 && countdown(7) == 7 ? 0 : 1;
}
