// O que as ABAS da tela de titulo mostram alem do elenco: nota do Trakt,
// comentarios do Trakt e titulos relacionados.
//
// Tudo isto ja existe no app web (renderExternalRatingsRow, a secao de
// comentarios e a aba "Mais como este"), e nenhum dos tres tinha fonte no port
// — por isso as abas caiam em "Sem informacao". Todos os pedidos falam com a
// api.trakt.tv por IMDb id, sem traducao de identificador no meio: o Trakt
// resolve "tt1234567" direto, e o TMDB nao.
#ifndef NV_EXTRAS_H
#define NV_EXTRAS_H

#define EX_COMENT_MAX 8
#define EX_REL_MAX   12

// Pede tudo de um titulo. Nao bloqueia: dispara um fio. Repetir com o mesmo
// `imdb` nao refaz o pedido. `serie` decide entre /shows e /movies.
void extras_pedir(const char *imdb, int serie);

// Nota do Trakt em 0..100 (0 = ainda nao chegou ou nao existe) e quantos
// votaram. O web mostra a mesma nota que o mdbList devolve para "trakt".
int  extras_nota_trakt(void);
int  extras_votos_trakt(void);

// Comentarios: os mais curtidos primeiro, que e a ordem de `comments/likes`.
int  extras_n_comentarios(void);
const char *extras_comentario_usuario(int i);
const char *extras_comentario_texto(int i);
int  extras_comentario_curtidas(int i);

// Titulos relacionados, para a aba "Mais como este".
int  extras_n_relacionados(void);
const char *extras_relacionado_titulo(int i);
const char *extras_relacionado_ano(int i);
const char *extras_relacionado_imdb(int i);

#endif
