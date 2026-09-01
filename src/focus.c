#include "focus.h"
#include <string.h>

void focus_iniciar(Foco *f, int nFileiras, const int *nColunas) {
  memset(f, 0, sizeof *f);
  if (nFileiras > FOCUS_MAX_FILEIRAS) nFileiras = FOCUS_MAX_FILEIRAS;
  f->nFileiras = nFileiras;
  for (int i = 0; i < nFileiras; i++) f->nColunas[i] = nColunas[i];
}

int focus_mover(Foco *f, int dx, int dy) {
  int fAntes = f->fileira, cAntes = f->coluna;

  if (dx) {
    int novo = f->coluna + dx;
    if (novo >= 0 && novo < f->nColunas[f->fileira]) f->coluna = novo;
  }
  if (dy) {
    int nova = f->fileira + dy;
    if (nova >= 0 && nova < f->nFileiras) {
      // guarda onde estava nesta fileira antes de sair
      f->colunaLembrada[f->fileira] = f->coluna;
      f->fileira = nova;
      int alvo = f->colunaLembrada[nova];
      if (alvo >= f->nColunas[nova]) alvo = f->nColunas[nova] - 1;
      if (alvo < 0) alvo = 0;
      f->coluna = alvo;
    }
  }
  return (f->fileira != fAntes || f->coluna != cAntes);
}

int focus_indice(const Foco *f, int fileira, int coluna) {
  return (f->fileira == fileira && f->coluna == coluna);
}
