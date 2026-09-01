#include "trakt.h"
#include "rede.h"
#include "js.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define CINEMETA "https://v3-cinemeta.strem.io"

static char token[128], cliente[80];
static int  ligado;

int trakt_ativo(void) { return ligado; }

int trakt_carregar(const char *dirArte) {
  char caminho[600], linha[300], *tab;
  FILE *f;
  snprintf(caminho, sizeof caminho, "%s/trakt.txt", dirArte ? dirArte : ".");
  f = fopen(caminho, "r");
  if (!f) { printf("[trakt] sem %s\n", caminho); return 0; }
  if (fgets(linha, sizeof linha, f)) {
    char *fim;
    tab = strchr(linha, '\t');
    if (tab) {
      *tab = 0;
      snprintf(cliente, sizeof cliente, "%s", tab + 1);
      fim = cliente + strlen(cliente);
      while (fim > cliente && (fim[-1] == '\n' || fim[-1] == '\r')) *--fim = 0;
    }
    snprintf(token, sizeof token, "%s", linha);
  }
  fclose(f);
  ligado = token[0] && cliente[0];
  printf("[trakt] %s\n", ligado ? "credencial carregada" : "credencial incompleta");
  return ligado;
}

// Arte e sinopse por id do IMDb. O Trakt devolve so identificadores e
// progresso; quem tem imagem e o Cinemeta, que e o mesmo indice que os addons
// usam — entao o que aparece na tela e o que da para pedir fonte.
static int enfeitar(CatItem *d, const char *tipo) {
  char url[300], *corpo;
  char serie[24];
  const char *dp;
  int ok = 0;
  snprintf(serie, sizeof serie, "%s", d->imdb);
  dp = strchr(serie, ':');
  if (dp) *(char *)dp = 0;
  snprintf(url, sizeof url, "%s/meta/%s/%s.json", CINEMETA, tipo, serie);
  corpo = rede_baixar(url, 20);
  if (!corpo) return 0;
  ok = js_texto(corpo, NULL, "poster", d->poster, sizeof d->poster);
  js_texto(corpo, NULL, "background", d->backdrop, sizeof d->backdrop);
  js_texto(corpo, NULL, "logo", d->logo, sizeof d->logo);
  if (!d->titulo[0]) js_texto(corpo, NULL, "name", d->titulo, sizeof d->titulo);
  js_texto(corpo, NULL, "description", d->sinopse, sizeof d->sinopse);
  if (!d->backdrop[0]) snprintf(d->backdrop, sizeof d->backdrop, "%s", d->poster);
  { char r[24] = "", ano[24] = "";
    js_texto(corpo, NULL, "runtime", r, sizeof r);
    js_texto(corpo, NULL, "releaseInfo", ano, sizeof ano);
    { char *tr = strstr(ano, "\xe2\x80\x93"); if (tr) *tr = 0; }
    snprintf(d->meta, sizeof d->meta, "%.20s%s%.20s", ano,
             (ano[0] && r[0]) ? "  \xc2\xb7  " : "", r);
    // Minutos que faltam, para a legenda do card. O Trakt da a porcentagem e o
    // Cinemeta a duracao; o cruzamento das duas e o unico jeito de ter isto
    // sem baixar o arquivo.
    if (d->progresso > 0 && d->progresso < 100) {
      int total = atoi(r);
      if (total > 0) d->restanteMin = total - (total * d->progresso) / 100;
    } else if (d->progresso == 0) {
      d->restanteMin = atoi(r);
    } }
  snprintf(d->genero, sizeof d->genero, "%s",
           strcmp(tipo, "series") ? "Filme" : "Programa de TV");
  snprintf(d->classificacao, sizeof d->classificacao, "14");
  free(corpo);
  return ok;
}

int trakt_continuar(CatItem *saida, int max) {
  const char *cab[4];
  char aut[200], chave[140];
  char *corpo;
  const char *p;
  int n = 0;
  if (!ligado) return 0;
  snprintf(aut, sizeof aut, "Authorization: Bearer %s", token);
  snprintf(chave, sizeof chave, "trakt-api-key: %s", cliente);
  cab[0] = aut;
  cab[1] = "trakt-api-version: 2";
  cab[2] = chave;
  cab[3] = NULL;
  corpo = rede_baixar_com("https://api.trakt.tv/sync/playback?extended=full", 25, cab);
  if (!corpo) { printf("[trakt] sem resposta\n"); return 0; }
  // O corpo e um array na raiz; js_array procura por chave, entao anda-se a mao.
  p = strchr(corpo, '[');
  p = p ? p + 1 : NULL;
  while (p && *p && n < max) {
    const char *f;
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p != '{') break;
    f = js_fim(p);
    {
      CatItem *d = &saida[n];
      const char *ep = strstr(p, "\"episode\"");
      int serie = ep && ep < f;
      char imdb[24] = "";
      memset(d, 0, sizeof *d);
      d->progresso = (int)js_num(p, f, "progress", 0.0);
      // O bloco "movie"/"show" tem o titulo e os ids; o "episode" traz
      // temporada e numero. Procurar "imdb" na faixa inteira pegaria o do
      // episodio, que os addons tambem aceitam mas nao identifica a obra.
      { const char *bloco = strstr(p, serie ? "\"show\"" : "\"movie\"");
        if (bloco && bloco < f) {
          const char *fb = js_fim(strchr(bloco, '{'));
          js_texto(bloco, fb, "title", d->titulo, sizeof d->titulo);
          js_texto(bloco, fb, "imdb", imdb, sizeof imdb);
        } }
      if (!imdb[0]) { p = js_prox(f); continue; }
      if (serie) {
        const char *fe = js_fim(strchr(ep, '{'));
        d->temporada = (int)js_num(ep, fe, "season", 0);
        d->episodio  = (int)js_num(ep, fe, "number", 0);
        snprintf(d->imdb, sizeof d->imdb, "%s:%d:%d", imdb,
                 d->temporada ? d->temporada : 1, d->episodio ? d->episodio : 1);
        snprintf(d->tipo, sizeof d->tipo, "series");
      } else {
        snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
        snprintf(d->tipo, sizeof d->tipo, "movie");
      }
      if (enfeitar(d, d->tipo)) n++;
    }
    p = js_prox(f);
  }
  free(corpo);
  printf("[trakt] %d em andamento\n", n);
  fflush(stdout);
  return n;
}

int trakt_lista(const char *qual, CatItem *saida, int max) {
  const char *cab[4];
  char aut[200], chave[140], url[160], *corpo;
  const char *p;
  int n = 0, passo;
  if (!ligado) return 0;
  snprintf(aut, sizeof aut, "Authorization: Bearer %s", token);
  snprintf(chave, sizeof chave, "trakt-api-key: %s", cliente);
  cab[0] = aut; cab[1] = "trakt-api-version: 2"; cab[2] = chave; cab[3] = NULL;

  // Filmes e series vem em endpoints separados; misturar as duas listas na
  // mesma fileira e o que o dono ve como "Minha Lista".
  for (passo = 0; passo < 2 && n < max; passo++) {
    const char *tipo = passo ? "shows" : "movies";
    snprintf(url, sizeof url, "https://api.trakt.tv/sync/%s/%s", qual, tipo);
    corpo = rede_baixar_com(url, 25, cab);
    if (!corpo) continue;
    p = strchr(corpo, '[');
    p = p ? p + 1 : NULL;
    while (p && *p && n < max) {
      const char *f;
      while (*p && (unsigned char)*p <= ' ') p++;
      if (*p != '{') break;
      f = js_fim(p);
      {
        CatItem *d = &saida[n];
        const char *bloco = strstr(p, passo ? "\"show\"" : "\"movie\"");
        char imdb[24] = "";
        memset(d, 0, sizeof *d);
        if (bloco && bloco < f) {
          const char *fb = js_fim(strchr(bloco, '{'));
          js_texto(bloco, fb, "title", d->titulo, sizeof d->titulo);
          js_texto(bloco, fb, "imdb", imdb, sizeof imdb);
        }
        if (imdb[0]) {
          snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
          snprintf(d->tipo, sizeof d->tipo, "%s", passo ? "series" : "movie");
          if (!strcmp(qual, "watchlist")) d->naLista = 1;
          else                            d->naColecao = 1;
          // Arte SEM consultar: as URLs do metahub sao deterministicas pelo id
          // do IMDb (verificado, 200 em todos os testados). Uma consulta por
          // item custava ~0,3 s e limitava a lista a dez; assim ela pode ter o
          // tamanho que o dono tem, e a imagem so e baixada quando aparece na
          // tela — o tex_cache ja faz isso.
          snprintf(d->poster, sizeof d->poster,
                   "https://images.metahub.space/poster/small/%s/img", imdb);
          snprintf(d->backdrop, sizeof d->backdrop,
                   "https://images.metahub.space/background/medium/%s/img", imdb);
          snprintf(d->logo, sizeof d->logo,
                   "https://images.metahub.space/logo/medium/%s/img", imdb);
          snprintf(d->genero, sizeof d->genero, "%s",
                   passo ? "Programa de TV" : "Filme");
          snprintf(d->classificacao, sizeof d->classificacao, "14");
          n++;
        }
      }
      p = js_prox(f);
    }
    free(corpo);
  }
  printf("[trakt] %s: %d\n", qual, n);
  fflush(stdout);
  return n;
}

// --- gravar progresso -------------------------------------------------------

static char marcaId[64];
static double marcaPos, marcaDur;
static pthread_t fioMarca;
static int fioMarcaVivo;

static void *enviarMarca(void *u) {
  const char *cab[4];
  char aut[200], chave[140], corpo[400], *r;
  char id[24];
  int t = 0, e = 0;
  const char *dp;
  double pct;
  (void)u;
  snprintf(id, sizeof id, "%s", marcaId);
  dp = strchr(id, ':');
  if (dp) { sscanf(dp + 1, "%d:%d", &t, &e); *(char *)dp = 0; }
  pct = 100.0 * marcaPos / marcaDur;
  if (pct < 0.0) pct = 0.0;
  if (pct > 100.0) pct = 100.0;

  snprintf(aut, sizeof aut, "Authorization: Bearer %s", token);
  snprintf(chave, sizeof chave, "trakt-api-key: %s", cliente);
  cab[0] = aut; cab[1] = "trakt-api-version: 2"; cab[2] = chave; cab[3] = NULL;

  // O Trakt separa "parei aqui" (scrobble/pause) de "assisti" (history). Como o
  // app so sabe a posicao, e sempre pause: marcar como assistido um filme que o
  // dono largou no meio seria pior que nao marcar nada. Acima de 90% o proprio
  // Trakt promove para assistido, que e a regra dele e nao nossa.
  if (t > 0 && e > 0)
    snprintf(corpo, sizeof corpo,
             "{\"show\":{\"ids\":{\"imdb\":\"%s\"}},"
             "\"episode\":{\"season\":%d,\"number\":%d},\"progress\":%.2f}",
             id, t, e, pct);
  else
    snprintf(corpo, sizeof corpo,
             "{\"movie\":{\"ids\":{\"imdb\":\"%s\"}},\"progress\":%.2f}",
             id, pct);

  r = rede_postar("https://api.trakt.tv/scrobble/pause", 20, cab, corpo);
  printf("[trakt] pause %s %.1f%% -> %s\n", marcaId, pct, r ? "ok" : "falhou");
  fflush(stdout);
  free(r);
  fioMarcaVivo = 0;
  return NULL;
}

void trakt_marcar(const char *imdb, double posSeg, double durSeg) {
  if (!ligado || !imdb || !*imdb || durSeg <= 1.0 || fioMarcaVivo) return;
  snprintf(marcaId, sizeof marcaId, "%s", imdb);
  marcaPos = posSeg; marcaDur = durSeg;
  fioMarcaVivo = 1;
  if (pthread_create(&fioMarca, NULL, enviarMarca, NULL) != 0) fioMarcaVivo = 0;
  else pthread_detach(fioMarca);
}
