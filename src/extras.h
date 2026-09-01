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

// FONTES DE NOTA, na ordem em que o web as lista (renderExternalRatingsRow,
// metaDetailsScreen.js:3410). Todas menos IMDb e Trakt vem do mdbList, que
// precisa da chave do dono em art/mdblist.txt; sem o arquivo elas ficam em 0 e
// a fileira mostra so as duas que temos por conta propria.
typedef enum {
  EX_TRAKT, EX_IMDB, EX_TMDB, EX_TOMATOES, EX_AUDIENCE, EX_METACRITIC,
  EX_LETTERBOXD, EX_NFONTES
} ExFonte;

// Le art/mdblist.txt. Sem ele o modulo funciona com Trakt e IMDb apenas.
void extras_carregar(const char *dirArte);

// Nota da fonte, CRUA multiplicada por 10 (o imdb vem com uma casa decimal e
// precisa caber em inteiro). 0 = nao ha. Divida por 10 e use
// extras_fonte_percentual() para saber se o resultado e "6.2" ou "66%".
int  extras_nota(int fonte);
int  extras_fonte_percentual(int fonte);
const char *extras_fonte_marca(int fonte);
// Caminho ABSOLUTO do arquivo de marca. Ver a nota em extras.c: relativo nao
// funciona porque o diretorio de trabalho do app nao e a pasta da arte.
const char *extras_caminho_marca(int fonte);

// Comentarios: os mais curtidos primeiro, que e a ordem de `comments/likes`.
int  extras_n_comentarios(void);
const char *extras_comentario_usuario(int i);
const char *extras_comentario_texto(int i);
int  extras_comentario_curtidas(int i);

// NOTAS POR EPISODIO, para o painel que o web mostra em SERIE no lugar dos
// cartoes (renderSeriesRatingsPanel, metaDetailsScreen.js:3843): uma fileira de
// temporadas e uma grade de pastilhas "E<n> / nota", coloridas por faixa.
//
// Vem de UMA chamada: /shows/<id>/seasons?extended=episodes,full devolve todas
// as temporadas com a nota de cada episodio junto. Pedir episodio a episodio
// seriam dezenas de chamadas para desenhar uma aba.
#define EX_TEMP_MAX 12
#define EX_EP_MAX   30
int  extras_n_temporadas(void);
int  extras_temporada_numero(int t);
int  extras_n_eps(int t);
int  extras_ep_numero(int t, int i);
// Nota do episodio em DECIMOS (72 = 7.2); 0 = sem nota.
int  extras_ep_nota(int t, int i);

// Titulos relacionados, para a aba "Mais como este".
int  extras_n_relacionados(void);
const char *extras_relacionado_titulo(int i);
const char *extras_relacionado_ano(int i);
const char *extras_relacionado_imdb(int i);

#endif
