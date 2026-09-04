#include <stdio.h>
#include "qr.h"
int main(int argc, char **argv) {
  Qr q; int x, y;
  if (argc < 2 || !qr_generate(&q, argv[1])) { printf("did not fit\n"); return 1; }
  printf("%d\n", q.side);
  for (y = 0; y < q.side; y++) {
    for (x = 0; x < q.side; x++) putchar(qr_modulo(&q, x, y) ? '1' : '0');
    putchar('\n');
  }
  return 0;
}
