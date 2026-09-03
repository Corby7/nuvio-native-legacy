#ifndef NV_INTRO_H
#define NV_INTRO_H
typedef struct { double inicio,fim; int tipo; } IntroTrecho;
enum { INTRO_ABERTURA=1, INTRO_RESUMO=2, INTRO_CREDITOS=3 };
void intro_pedir(const char *imdb,int temporada,int episodio);
void intro_desligar(void);
int  intro_ativo(double posSeg,double *fim,int *tipo);
int  intro_extrair(const char *json,IntroTrecho *saida,int max);
#endif
