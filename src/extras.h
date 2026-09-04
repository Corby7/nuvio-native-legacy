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

#define EX_COMMENT_MAX 8
#define EX_REL_MAX   12

// Pede tudo de um titulo. Nao bloqueia: dispara um fio. Repetir com o mesmo
// `imdb` nao refaz o pedido. `serie` decide entre /shows e /movies.
// `tmdbId` e o id do titulo no TMDB (0 quando nao se sabe). Serve so a COLECAO,
// que o TMDB expoe apenas por id proprio — nao ha caminho por IMDb.
void extras_request(const char *imdb, int series, long tmdbId);

// Nota do Trakt em 0..100 (0 = ainda nao chegou ou nao existe) e quantos
// votaram. O web mostra a mesma nota que o mdbList devolve para "trakt".
int  extras_score_trakt(void);
int  extras_votes_trakt(void);

// FONTES DE NOTA, na ordem em que o web as lista (renderExternalRatingsRow,
// metaDetailsScreen.js:3410). Todas menos IMDb e Trakt vem do mdbList, que
// precisa da chave do dono em art/mdblist.txt; sem o arquivo elas ficam em 0 e
// a fileira mostra so as duas que temos por conta propria.
typedef enum {
  EX_TRAKT, EX_IMDB, EX_TMDB, EX_TOMATOES, EX_AUDIENCE, EX_METACRITIC,
  EX_LETTERBOXD, EX_NFONTES
} ExSource;

// Le art/mdblist.txt. Sem ele o modulo funciona com Trakt e IMDb apenas.
void extras_load(const char *dirArt);

// Chave do mdblist vinda da CONTA. Mesmo motivo do TMDB: enquanto sair de
// art/mdblist.txt (que esta ate com modo 0600), o pacote distribui a chave de
// quem o montou.
void extras_set_key(const char *key);

// Nota da fonte, CRUA multiplicada por 10 (o imdb vem com uma casa decimal e
// precisa caber em inteiro). 0 = nao ha. Divida por 10 e use
// extras_fonte_percentual() para saber se o resultado e "6.2" ou "66%".
int  extras_score(int source);
int  extras_source_percentual(int source);
const char *extras_source_brand(int source);
// Caminho ABSOLUTO do arquivo de marca. Ver a nota em extras.c: relativo nao
// funciona porque o diretorio de trabalho do app nao e a pasta da arte.
const char *extras_path_brand(int source);
// Marca por NOME de arquivo, para as que nao sao fonte de nota (o wordmark do
// Trakt, "trakt_wordmark").
const char *extras_path_brand_name(const char *name);

// Comentarios: os mais curtidos primeiro, que e a ordem de `comments/likes`.
int  extras_n_comments(void);
const char *extras_comment_user(int i);
const char *extras_comment_text(int i);
int  extras_comment_likes(int i);
// Nota de QUEM COMENTOU (user_rating do Trakt), 0..10; 0 quando nao avaliou.
// A referencia mostra "10/10  17 curtidas" no rodape do cartao.
int  extras_comment_score(int i);

// COMENTARIOS DO EPISODIO, para o seletor "Série | Episódio" que a referencia
// mostra acima dos cartoes.
//
// Sao uma consulta SEPARADA no Trakt
// (/shows/<id>/seasons/<t>/episodes/<e>/comments/likes), nao um recorte da
// lista da serie — os dois conjuntos nao se cruzam. Feita SOB DEMANDA: uma
// viagem por episodio, e so quando o dono escolhe a aba "Episódio".
//
// `imdbSerie` aceita tanto "tt1234567" quanto o "tt1234567:2:4" que a lista de
// episodios usa; a parte depois do primeiro ':' e ignorada.
void extras_request_comments_ep(const char *imdbSeries, int season, int episode);
int  extras_n_comments_ep(void);
// 1 enquanto a consulta esta em voo. Quem desenha usa isto para mostrar
// "carregando" em vez de "nao ha comentarios" — os dois estados sao a lista
// vazia, e confundi-los faz o episodio parecer sem comentario nenhum.
int  extras_comments_ep_loading(void);
const char *extras_comment_ep_user(int i);
const char *extras_comment_ep_text(int i);
int  extras_comment_ep_likes(int i);
int  extras_comment_ep_score(int i);

// NOTAS POR EPISODIO, para o painel que o web mostra em SERIE no lugar dos
// cartoes (renderSeriesRatingsPanel, metaDetailsScreen.js:3843): uma fileira de
// temporadas e uma grade de pastilhas "E<n> / nota", coloridas por faixa.
//
// Vem de UMA chamada: /shows/<id>/seasons?extended=episodes,full devolve todas
// as temporadas com a nota de cada episodio junto. Pedir episodio a episodio
// seriam dezenas de chamadas para desenhar uma aba.
#define EX_TEMP_MAX 12
#define EX_EP_MAX   30
int  extras_n_seasons(void);
int  extras_season_number(int t);
int  extras_n_eps(int t);
int  extras_ep_number(int t, int i);
// Nota do episodio em DECIMOS (72 = 7.2); 0 = sem nota.
int  extras_ep_score(int t, int i);

// COLECAO (franquia) do filme, para a aba que o web chama pelo nome dela.
// Vem de /movie/<id> -> belongs_to_collection -> /collection/<id>. As partes
// trazem so o id do TMDB, entao abrir uma delas passa pelo mesmo caminho do
// credito de um ator (desc_pedir_titulo_tmdb).
#define EX_COL_MAX 12
const char *extras_collection_name(void);
int  extras_n_collection(void);
const char *extras_collection_title(int i);
const char *extras_collection_year(int i);
long extras_collection_tmdb(int i);

// EPISODIOS JA ASSISTIDOS, do Trakt (/shows/<id>/progress/watched). O card do
// episodio ganha uma mascara e um check quando o dono ja viu. Sem isto, quem
// acompanha uma serie nao tinha como saber onde parou olhando a lista.
int  extras_ep_watched(int season, int episode);
// 1 apenas depois de receber o historico desta obra. Sem resposta nao inferir
// que todos os episodios estao por assistir.
int extras_progress_ready(void);
int extras_next_episode(int *season, int *episode);

// FICHA TECNICA do filme, para a secao "Detalhes do Filme".
//
// Tudo isto sai da MESMA chamada /movie/<id> que a colecao ja fazia — o corpo
// trazia status, runtime, release_date e os paises desde sempre, e o parse lia
// so belongs_to_collection e jogava o resto fora. Com
// `append_to_response=release_dates,videos` a mesma viagem passa a trazer
// tambem a classificacao etaria e os trailers. Nenhum pedido de rede novo.
//
// Vazio ("" ou 0) quando ainda nao chegou ou o TMDB nao tem o campo. Quem
// desenha deve OMITIR a linha nesse caso, nunca escrever um valor de reserva:
// ver a nota sobre a classificacao cravada em descoberta.c.
const char *extras_profile_status(void);          // "Released", "Post Production"
int         extras_profile_duration(void);         // minutos; 0 = nao ha
const char *extras_profile_countries(void);          // "United States of America, Canada"
const char *extras_profile_age_rating(void);   // "R", "PG-13", "14"
const char *extras_profile_release(void);      // "2026-01-15"

// TRAILERS. So o que da para mostrar: id do YouTube, nome e miniatura.
//
// NAO HA COMO TOCAR. O app web abre um iframe do YouTube; este port nao tem
// reprodutor nem extrator de stream, e a decisao ja registrada em detail.c e
// gfx.c foi remover o botao de trailer em vez de deixar um controle que promete
// o que nao cumpre. A mesma regra vale aqui: o card entra na composicao, mas
// nao deve receber foco enquanto nao houver o que abrir.
#define EX_TRAILER_MAX 6
int         extras_n_trailers(void);
const char *extras_trailer_yt(int i);        // id do video ("dQw4w9WgXcQ")
const char *extras_trailer_name(int i);      // "Official Trailer"
// URL da miniatura. Previsivel a partir do id, sem chamada de API — e o mesmo
// caminho que o web usa (metaDetailsScreen.js:5728). Passe direto a tex_obter:
// o cache de texturas baixa e guarda qualquer URL sozinho.
const char *extras_trailer_thumb(int i);

// Titulos relacionados, para a aba "Mais como este".
int  extras_n_related(void);
const char *extras_related_title(int i);
const char *extras_related_year(int i);
const char *extras_related_imdb(int i);
// Poster do relacionado (URL). Vem de `extended=images` do Trakt, que devolve o
// caminho sem esquema — o https e acrescentado aqui.
const char *extras_related_poster(int i);

#endif
