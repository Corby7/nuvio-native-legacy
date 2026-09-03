// Escrita de JSON. O js.c le; este escreve.
//
// Nasceu com o sync: todo push do Supabase manda um array de objetos, e montar
// isso com snprintf e como o app faria ate agora significa errar o escape na
// primeira vez que um titulo tiver aspas ou uma barra invertida — e o servidor
// recusa o corpo inteiro, nao a linha ruim. Um escritor de 100 linhas custa
// menos que depurar um push que falha so para alguns usuarios.
//
// A virgula e automatica: quem escreve so diz "abre objeto, chave, valor". O
// erro classico de montar JSON a mao e a virgula sobrando no ultimo elemento,
// e ele desaparece quando ninguem escreve virgula.
#ifndef NV_JSW_H
#define NV_JSW_H
#include <stddef.h>

#define JSW_PROF 16   // profundidade maxima de aninhamento

typedef struct {
  char  *p;
  size_t n, cap;
  int    erro;                 // 1 depois de qualquer falha; o resto vira no-op
  int    prof;
  char   primeiro[JSW_PROF];   // 1 enquanto o nivel atual nao tem elemento
} Jsw;

void jsw_iniciar(Jsw *w);
void jsw_livre(Jsw *w);

// Texto pronto (0 em falha). O buffer continua sendo do escritor: copie ou use
// antes de jsw_livre.
const char *jsw_texto_final(const Jsw *w);

void jsw_obj_ini(Jsw *w);
void jsw_obj_fim(Jsw *w);
void jsw_arr_ini(Jsw *w);
void jsw_arr_fim(Jsw *w);

// Chave dentro de um objeto. O proximo jsw_* escreve o valor dela.
void jsw_chave(Jsw *w, const char *nome);

void jsw_str(Jsw *w, const char *s);     // string com escape; NULL vira null
void jsw_num(Jsw *w, double v);
void jsw_int(Jsw *w, long long v);
void jsw_bool(Jsw *w, int v);
void jsw_nulo(Jsw *w);

// Valor JSON JA PRONTO (um objeto vindo do servidor, por exemplo). Existe para
// o credential_json das credenciais, que o app repassa sem interpretar.
void jsw_bruto(Jsw *w, const char *json);

// Atalhos de par chave/valor, que e o que quase todo chamador quer.
void jsw_cs(Jsw *w, const char *chave, const char *valor);
void jsw_ci(Jsw *w, const char *chave, long long valor);
void jsw_cb(Jsw *w, const char *chave, int valor);

#endif
