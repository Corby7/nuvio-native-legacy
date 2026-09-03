#include <stdio.h>
#include "qr.h"
int main(int argc, char **argv) {
  Qr q; int x, y;
  if (argc < 2 || !qr_gerar(&q, argv[1])) { printf("nao coube\n"); return 1; }
  printf("%d\n", q.lado);
  for (y = 0; y < q.lado; y++) {
    for (x = 0; x < q.lado; x++) putchar(qr_modulo(&q, x, y) ? '1' : '0');
    putchar('\n');
  }
  return 0;
}
