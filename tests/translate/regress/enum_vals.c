#include <stdio.h>
enum E { A, B, C = 10, D, E_NEG = -5, F };
int main(void) { printf("%d %d %d %d %d %d\n", A, B, C, D, E_NEG, F); return 0; }
