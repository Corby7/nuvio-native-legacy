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
    // PULA fileira vazia. Num filme as fileiras de temporada e de episodio tem
    // zero colunas, e pousar nelas era foco em coisa que a tela nem desenha: o
    // D-pad parecia travado e a rolagem ainda mirava o grupo vazio. Uma fileira
    // sem item nunca deve receber foco, entao a busca segue no mesmo sentido
    // ate achar uma que tenha — ou desistir na borda, devolvendo 0.
    int nova = f->fileira + dy;
    while (nova >= 0 && nova < f->nFileiras && f->nColunas[nova] <= 0) nova += dy;
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
