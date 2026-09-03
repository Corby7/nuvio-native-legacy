// Gerenciador de foco espacial com MEMORIA DE COLUNA por fileira.
//
// Memoria de coluna e o detalhe que separa uma navegacao boa de uma irritante:
// ao descer da fileira 1 (coluna 5) para a fileira 2 e voltar, o foco tem que
// retornar a coluna 5, nao a coluna 0. O tvOS faz isso; sem isso o usuario
// perde o lugar toda vez que troca de fileira.
#ifndef NV_FOCUS_H
#define NV_FOCUS_H

#define FOCUS_MAX_FILEIRAS 32

typedef struct {
  int fileira;
  int coluna;
  int colunaLembrada[FOCUS_MAX_FILEIRAS];
  int nFileiras;
  int nColunas[FOCUS_MAX_FILEIRAS];
} Foco;

void focus_iniciar(Foco *f, int nFileiras, const int *nColunas);
int  focus_mover(Foco *f, int dx, int dy);   // 1 se moveu
int  focus_indice(const Foco *f, int fileira, int coluna);

#endif
