#ifndef NV_LEGENDA_H
#define NV_LEGENDA_H
#include <stddef.h>

typedef struct {
  double inicio, fim;
  char texto[768];
} LegendaCue;

/* OpenSubtitles e desenhado pela UI, acima do plano de video. */
void legenda_carregar(const char *url);
void legenda_desligar(void);
int  legenda_texto(double posSeg, int atrasoMs, char *dst, size_t tam);

/* Parser puro, tambem usado pela regressao. O chamador libera *saida. */
int legenda_extrair(const char *corpo, LegendaCue **saida);

#endif
