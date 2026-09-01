// Ficha de uma PESSOA do elenco e a filmografia dela.
//
// E o `openCastDetail` do app web (metaDetailsScreen.js:6165), que abre a tela
// castDetailScreen com /person/<id>?append_to_response=combined_credits do
// TMDB. O port mostrava o elenco e parava ali: clicar num rosto nao levava a
// lugar nenhum.
#ifndef NV_PESSOA_H
#define NV_PESSOA_H

#define PES_MAX 24

// Pede a ficha de `tmdbId`. Nao bloqueia. Repetir com o mesmo id nao refaz.
void pessoa_pedir(long tmdbId, const char *nomeConhecido, const char *foto);

// 1 quando ja ha o que desenhar.
int  pessoa_pronta(void);
const char *pessoa_nome(void);
const char *pessoa_foto(void);
const char *pessoa_bio(void);
// "Atuacao", "Direcao"... o known_for_department do TMDB, traduzido.
const char *pessoa_area(void);

// Filmografia, ja ordenada por popularidade (a mesma ordem do web).
int  pessoa_n_creditos(void);
const char *pessoa_credito_titulo(int i);
const char *pessoa_credito_papel(int i);
const char *pessoa_credito_ano(int i);
const char *pessoa_credito_poster(int i);
// IMDb id do titulo, quando o TMDB informa; vazio quando nao. Sem ele nao ha
// como abrir a pagina do titulo, entao o cartao fica so como informacao.
const char *pessoa_credito_imdb(int i);
// Id do titulo no TMDB e "movie"/"tv". O credito de combined_credits NAO traz
// imdb_id; e por estes dois que se chega ao id que o Cinemeta entende.
long pessoa_credito_tmdb(int i);
const char *pessoa_credito_tipo(int i);

#endif
