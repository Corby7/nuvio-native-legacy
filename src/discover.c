#include "discover.h"
#include "mark.h"
#include <SDL2/SDL.h>
#include "catalog.h"
#include "addons.h"
#include "net.h"
#include "js.h"
#include "trakt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define CINEMETA "https://v3-cinemeta.strem.io"
#define TMDB     "https://api.themoviedb.org/3"

// "2026-07-29" -> "29 de julho de 2026". Formato do web, que usa
// `toLocaleDateString(undefined, {month:"long", day:"numeric", year:"numeric"})`
// (metaDetailsScreen.js:1387); o formato numerico que estava aqui antes era
// invencao do port. Entrada que nao casa o padrao ISO sai como veio, e nao
// vazia: melhor mostrar a data crua que engolir o dado.
void disc_date_long(const char *iso, char *dst, size_t size) {
  static const char *MONTH[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
  };
  if (!iso || !dst || size == 0) { if (dst && size) dst[0] = 0; return; }
  if (strlen(iso) >= 10 && iso[4] == '-') {
    int month = (iso[5] - '0') * 10 + (iso[6] - '0');
    int day = (iso[8] - '0') * 10 + (iso[9] - '0');
    if (month >= 1 && month <= 12) {
      snprintf(dst, size, "%d %s %c%c%c%c",
               day, MONTH[month - 1], iso[0], iso[1], iso[2], iso[3]);
      return;
    }
    snprintf(dst, size, "%c%c%c%c", iso[0], iso[1], iso[2], iso[3]);
    return;
  }
  snprintf(dst, size, "%s", iso);
}


// Chave do TMDB, em art/tmdb.txt. SEGREDO do dono (saiu do dist/nuvio.env.js do
// app web) — nao versionar. Sem ela o elenco continua so com nomes.
static char tmdbKey[64];
static char dirArtDisc[512];

void disc_tmdb_set(const char *key) {
  if (!key || !*key) return;
  snprintf(tmdbKey, sizeof tmdbKey, "%s", key);
  printf("[disc] tmdb: key from the account\n");
  fflush(stdout);
}

void disc_tmdb(const char *dirArt) {
  char path[600];
  FILE *f;
  snprintf(dirArtDisc, sizeof dirArtDisc, "%s", dirArt ? dirArt : ".");
  snprintf(path, sizeof path, "%s/tmdb.txt", dirArt ? dirArt : ".");
  f = fopen(path, "r");
  if (!f) return;
  if (fgets(tmdbKey, sizeof tmdbKey, f)) {
    char *end = tmdbKey + strlen(tmdbKey);
    while (end > tmdbKey && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
  }
  fclose(f);
  printf("[disc] tmdb %s\n", tmdbKey[0] ? "ok" : "missing");
}

const char *disc_key_tmdb(void) { return tmdbKey; }

// strstr que NAO passa de `fim`. O objeto da regiao BR termina antes das
// outras regioes na resposta do TMDB; procurar rent/buy no corpo inteiro
// pegaria o provedor de outra regiao quando a BR nao tivesse.
// Canonical genre label. Cinemeta and the addons return genres in English, but
// not consistently: the same genre arrives as "Sci-Fi" from one source and
// "Science Fiction" from another, and the hyphenated forms ("Film-Noir",
// "Talk-Show") read as identifiers rather than labels. Both spellings landing
// in the same row is visible on every card and every detail screen.
//
// A table and not a lookup: the Stremio genre set is closed and small, and a
// network round trip per title to tidy two words would be absurd. A genre
// outside the table goes out as it came in.
const char *disc_genre_label(const char *g) {
  static const struct { const char *from, *to; } T[] = {
    { "Sci-Fi",     "Science Fiction" }, { "Film-Noir", "Film Noir" },
    { "Game-Show",  "Game Show" },       { "Talk-Show", "Talk Show" },
    { "Reality-TV", "Reality" },
  };
  size_t i;
  if (!g || !*g) return "";
  for (i = 0; i < sizeof T / sizeof *T; i++)
    if (!strcasecmp(g, T[i].from)) return T[i].to;
  return g;
}

static const char *ate(const char *start, const char *end, const char *needle) {
  size_t n = strlen(needle);
  for (; start && start + n <= end; start++)
    if (*start == needle[0] && memcmp(start, needle, n) == 0) return start;
  return NULL;
}

// Primeiro provedor do array `chave` dentro de [ini,fim): nome e logo no
// formato w92 do TMDB. flatrate/rent/buy sao arrays de provedores; o primeiro
// e o principal na pratica (o TMDB ordena por relevancia local).
static int providerBetween(const char *start, const char *end, const char *key,
                         char *name, size_t nName, char *logo, size_t nLogo) {
  const char *k = ate(start, end, key);
  const char *item = k ? strchr(k, '{') : NULL;
  if (!item || item >= end) return 0;
  const char *fi = js_end(item);
  if (fi > end) fi = end;
  char path[128] = "";
  if (!js_text(item, fi, "provider_name", name, nName)) return 0;
  if (js_text(item, fi, "logo_path", path, sizeof path) && path[0] == '/')
    snprintf(logo, nLogo, "https://image.tmdb.org/t/p/w92%s", path);
  return 1;
}

// Preenche foto e personagem do elenco. O Cinemeta da so o NOME; o personagem
// e o retrato vem do TMDB, que precisa de duas viagens: achar o id dele pelo
// id do IMDb e so entao pedir os creditos.
static void photosOfCast(CatItem *d, const char *imdbSeries, int series) {
  char url[400], *body;
  long idTmdb = 0;
  if (!tmdbKey[0] || d->nCast < 1) return;
  snprintf(url, sizeof url, "%s/find/%s?api_key=%s&external_source=imdb_id",
           TMDB, imdbSeries, tmdbKey);
  body = net_download(url, 20);
  if (!body) return;
  { const char *vet = series ? "tv_results" : "movie_results";
    const char *p = js_array(body, NULL, vet);
    if (p) idTmdb = (long)js_num(p, js_end(p), "id", 0); }
  free(body);
  if (!idTmdb) return;
  d->tmdb = idTmdb;

  snprintf(url, sizeof url, "%s/%s/%ld/credits?api_key=%s",
           TMDB, series ? "tv" : "movie", idTmdb, tmdbKey);
  body = net_download(url, 20);
  if (!body) return;
  { const char *p = js_array(body, NULL, "cast");
    int k = 0;
    (void)0;
    while (p && k < d->nCast) {
      const char *f = js_end(p);
      char pathPhoto[128] = "";
      js_text(p, f, "character", d->cast[k].role, sizeof d->cast[k].role);
      d->cast[k].tmdb = (long)js_num(p, f, "id", 0.0);
      if (js_text(p, f, "profile_path", pathPhoto, sizeof pathPhoto) &&
          pathPhoto[0] == '/')
        snprintf(d->cast[k].photo, sizeof d->cast[k].photo,
                 "https://image.tmdb.org/t/p/w185%s", pathPhoto);
      // O TMDB devolve o elenco na mesma ordem de importancia que o Cinemeta,
      // entao casar por posicao acerta na pratica; casar por nome falharia nos
      // acentos e nos nomes escritos de forma diferente entre as duas bases.
      k++;
      p = js_next(f);
    } }
  free(body);

  // Onde assistir. Os campos provLogo/provNome existiam no CatItem e NUNCA
  // eram preenchidos no caminho dinamico — o selo do streaming ficava vazio em
  // todo titulo. O TMDB responde por regiao; BR e a do dono.
  snprintf(url, sizeof url, "%s/%s/%ld/watch/providers?api_key=%s",
           TMDB, series ? "tv" : "movie", idTmdb, tmdbKey);
  body = net_download(url, 20);
  if (body) {
    const char *br = strstr(body, "\"BR\"");
    if (br) {
      const char *brObj = strchr(br, '{');
      const char *brEnd = brObj ? js_end(brObj) : NULL;
      if (brObj && brEnd && brEnd > brObj) {
        // flatrate = incluido na assinatura; rent = aluguel; buy = compra.
        // Se o titulo nao esta em streaming aqui, o selo fica vazio DE
        // PROPOSITO, em vez de anunciar aluguel como se fosse catalogo.
        providerBetween(brObj, brEnd, "\"flatrate\"",
                      d->providerName, sizeof d->providerName,
                      d->providerLogo, sizeof d->providerLogo);
        providerBetween(brObj, brEnd, "\"rent\"",
                      d->rentName, sizeof d->rentName,
                      d->rentLogo, sizeof d->rentLogo);
        providerBetween(brObj, brEnd, "\"buy\"",
                      d->compName, sizeof d->compName,
                      d->compLogo, sizeof d->compLogo);
      }
    }
    free(body);
  }
}

// Definida adiante, junto do resto do parse de meta do Stremio; declarada aqui
// porque a busca, logo abaixo, monta CatItem a partir da mesma resposta.
static int ofMeta(const char *start, const char *end, const char *kind, CatItem *d);

// --- BUSCA POR TITULO --------------------------------------------------------
//
// A tela de busca so filtrava o que ja estava em memoria (um strstr sobre as
// fileiras da home), entao procurar por algo fora das ~12 primeiras linhas de
// cada catalogo nao achava nada — e o dono viu isso como "nao ta procurando em
// tudo". Era verdade: nao havia consulta de rede nenhuma.
//
// O protocolo Stremio expoe busca no mesmo endpoint de catalogo, com o filtro
// no caminho: <base>/catalog/<tipo>/<id>/search=<termo>.json. O Cinemeta, que e
// o catalogo oficial e nao depende dos addons do dono, responde nos dois tipos
// — e por isso e a fonte usada aqui: uma busca que so funcionasse com os addons
// instalados falharia de formas diferentes em cada maquina.
//
// Roda em FIO PROPRIO porque bloqueia (duas viagens), e a tela de busca nao
// pode congelar entre uma tecla e outra.
static char     searchTerm[96];     // termo JA CONSULTADO
static char     searchRequest[96];    // termo que os fios devem consultar
static pthread_mutex_t searchLock = PTHREAD_MUTEX_INITIALIZER;

// Escapa o termo para caber num caminho de URL. Sem isto um espaco ou acento
// quebra o pedido, e "the invite" — duas palavras, o caso normal — nunca
// chegaria ao servidor.
static void urlEscape(const char *s, char *dst, size_t size) {
  static const char *HEX = "0123456789ABCDEF";
  size_t o = 0;
  for (; *s && o + 4 < size; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      dst[o++] = (char)c;
    } else {
      dst[o++] = '%'; dst[o++] = HEX[c >> 4]; dst[o++] = HEX[c & 15];
    }
  }
  dst[o] = 0;
}

static int readSearch(const char *kind, const char *term, CatItem *output,
                    int max) {
  char url[500], esc[300];
  char *body;
  const char *p;
  int n = 0;
  urlEscape(term, esc, sizeof esc);
  snprintf(url, sizeof url, "%s/catalog/%s/top/search=%s.json",
           CINEMETA, kind, esc);
  body = net_download(url, 20);
  if (!body) return 0;
  p = js_array(body, NULL, "metas");
  while (p && n < max) {
    const char *f = js_end(p);
    if (ofMeta(p, f, kind, &output[n])) n++;
    p = js_next(f);
  }
  free(body);
  return n;
}

// --- ALVOS DE BUSCA ---------------------------------------------------------
//
// Um "alvo" e um catalogo que aceita busca. Sao os 2 do Cinemeta (que existem
// sempre, independem dos addons do dono) mais os que os manifestos declararem.
// Nos addons do dono sao 8: Xperience (filme/serie), AIOStreams TMDB e TVDB
// (filme/serie cada) e Akashi TV (filme/serie).
//
// Antes so o Cinemeta era consultado, e era isso que o dono via como "nao ta
// procurando em todos os catalogos" — porque de fato nao estava.
#define SEARCH_TARGETS  16
#define SEARCH_PER_TARGET 12          // uma fileira por alvo, 12 cabem na tela
#define SEARCH_THREADS    3            // quantos alvos em voo ao mesmo tempo

typedef struct {
  char base[300];
  char kind[8];
  char id[96];
  char title[96];
  char addon[64];
} TargetSearch;

static TargetSearch targets[SEARCH_TARGETS];
static int       nTargets;

// Resultado POR ALVO, com a geracao em que foi obtido. Guardar por alvo (e nao
// numa lista unica) e o que permite uma fileira por catalogo, com a origem, e o
// que deixa a tela mostrar o primeiro que responder sem esperar o mais lento.
static struct {
  CatItem items[SEARCH_PER_TARGET];
  int     n;
  int     generation;
} resTarget[SEARCH_TARGETS];

static int  generation;            // sobe a cada termo novo
static int  nextTarget;        // fila de trabalho: proximo indice a consultar
static int  threadsAlive;

void disc_targets_search_reset(void) {
  pthread_mutex_lock(&searchLock);
  // O Cinemeta entra SEMPRE e primeiro: e a unica fonte que nao depende de
  // addon nenhum, entao a busca continua funcionando numa instalacao limpa.
  nTargets = 0;
  { int t; const char *tt[2] = { "movie", "series" };
    const char *rot[2] = { "Films", "Series" };
    for (t = 0; t < 2; t++) {
      TargetSearch *a = &targets[nTargets++];
      snprintf(a->base,  sizeof a->base,  "%s", CINEMETA);
      snprintf(a->kind,  sizeof a->kind,  "%s", tt[t]);
      snprintf(a->id,    sizeof a->id,    "%s", "top");
      snprintf(a->title,sizeof a->title,"%s", rot[t]);
      snprintf(a->addon, sizeof a->addon, "%s", "Cinemeta");
    } }
  memset(resTarget, 0, sizeof resTarget);
  pthread_mutex_unlock(&searchLock);
}

void disc_target_search(const char *base, const char *kind, const char *id,
                     const char *title, const char *addon) {
  pthread_mutex_lock(&searchLock);
  if (nTargets < SEARCH_TARGETS) {
    TargetSearch *a = &targets[nTargets++];
    snprintf(a->base,   sizeof a->base,   "%s", base ? base : "");
    snprintf(a->kind,   sizeof a->kind,   "%s", kind ? kind : "");
    snprintf(a->id,     sizeof a->id,     "%s", id ? id : "");
    snprintf(a->title, sizeof a->title, "%s", title ? title : "");
    snprintf(a->addon,  sizeof a->addon,  "%s", addon ? addon : "");
  }
  pthread_mutex_unlock(&searchLock);
}

// Consulta UM alvo. Devolve quantos itens leu.
static int queryTarget(const TargetSearch *a, const char *term,
                         CatItem *output, int max) {
  char url[600], esc[300];
  char *body;
  const char *p;
  int n = 0;
  urlEscape(term, esc, sizeof esc);
  snprintf(url, sizeof url, "%s/catalog/%s/%s/search=%s.json",
           a->base, a->kind, a->id, esc);
  // 6 s por alvo, como o web (SEARCH_CATALOG_TIMEOUT 6500). Addon lento nao
  // trava a tela: a fileira dele so aparece quando chegar, e as outras ja
  // estao la.
  body = net_download(url, 6);
  if (!body) return 0;
  p = js_array(body, NULL, "metas");
  while (p && n < max) {
    const char *f = js_end(p);
    if (ofMeta(p, f, a->kind, &output[n])) n++;
    p = js_next(f);
  }
  free(body);
  return n;
}

static void *threadSearch(void *arg) {
  (void)arg;
  for (;;) {
    TargetSearch a;
    char term[96];
    int mine, g;
    CatItem found[SEARCH_PER_TARGET];
    int n;

    pthread_mutex_lock(&searchLock);
    if (nextTarget >= nTargets || !searchRequest[0]) {
      threadsAlive--;
      pthread_mutex_unlock(&searchLock);
      return NULL;
    }
    mine = nextTarget++;
    a = targets[mine];
    g = generation;
    snprintf(term, sizeof term, "%s", searchRequest);
    pthread_mutex_unlock(&searchLock);

    n = queryTarget(&a, term, found, SEARCH_PER_TARGET);

    pthread_mutex_lock(&searchLock);
    // Geracao velha = o dono digitou outra coisa enquanto isto voltava. O
    // resultado nasceu obsoleto; descartar e mais barato que mostrar e trocar.
    if (g == generation) {
      memcpy(resTarget[mine].items, found, sizeof(CatItem) * (size_t)n);
      resTarget[mine].n = n;
      resTarget[mine].generation = g;
      snprintf(searchTerm, sizeof searchTerm, "%s", term);
    }
    pthread_mutex_unlock(&searchLock);
  }
}

void disc_fetch(const char *term) {
  int k, missing;
  if (!term) return;
  pthread_mutex_lock(&searchLock);
  if (!strcmp(term, searchRequest)) { pthread_mutex_unlock(&searchLock); return; }
  snprintf(searchRequest, sizeof searchRequest, "%s", term);
  generation++;
  nextTarget = 0;
  // Zera a contagem, nao os itens: a tela pode estar desenhando o quadro
  // corrente e ler item pela metade seria pior que uma fileira a menos.
  for (k = 0; k < SEARCH_TARGETS; k++) resTarget[k].n = 0;
  missing = SEARCH_THREADS - threadsAlive;
  pthread_mutex_unlock(&searchLock);

  // Fios sob demanda: os que ja estao vivos pegam os alvos novos sozinhos,
  // porque leem `proximoAlvo` sob a trava a cada volta.
  for (k = 0; k < missing; k++) {
    pthread_t t;
    pthread_mutex_lock(&searchLock); threadsAlive++; pthread_mutex_unlock(&searchLock);
    if (pthread_create(&t, NULL, threadSearch, NULL) != 0) {
      pthread_mutex_lock(&searchLock); threadsAlive--; pthread_mutex_unlock(&searchLock);
    } else {
      pthread_detach(t);
    }
  }
}

int disc_search_generation(void) {
  int g;
  pthread_mutex_lock(&searchLock);
  g = generation;
  pthread_mutex_unlock(&searchLock);
  return g;
}

int disc_search_n_targets(void) { return nTargets; }

int disc_search_target_n(int target, const char *term) {
  int n = 0;
  pthread_mutex_lock(&searchLock);
  if (target >= 0 && target < nTargets && term && !strcmp(term, searchTerm) &&
      resTarget[target].generation == generation)
    n = resTarget[target].n;
  pthread_mutex_unlock(&searchLock);
  return n;
}

const char *disc_search_target_title(int target) {
  return (target >= 0 && target < nTargets) ? targets[target].title : "";
}
const char *disc_search_target_addon(int target) {
  return (target >= 0 && target < nTargets) ? targets[target].addon : "";
}

int disc_search_target_item(int target, int i, CatItem *dst) {
  int ok = 0;
  pthread_mutex_lock(&searchLock);
  if (dst && target >= 0 && target < nTargets && i >= 0 && i < resTarget[target].n) {
    memcpy(dst, &resTarget[target].items[i], sizeof *dst);
    ok = 1;
  }
  pthread_mutex_unlock(&searchLock);
  return ok;
}

// Compatibilidade com quem ainda pergunta "quantos no total".
int disc_search_n(const char *term) {
  int k, t = 0;
  for (k = 0; k < nTargets; k++) t += disc_search_target_n(k, term);
  return t;
}


static int searching;
static pthread_t thread, threadEp;
static int epItem = -1, epTemp, threadEpAlive;

int disc_searching(void) {
  int v;
  pthread_mutex_lock(&searchLock);
  v = (threadsAlive > 0);
  pthread_mutex_unlock(&searchLock);
  return v;
}

// Um item do catalogo montado a partir de um meta do Stremio. Devolve 1 se
// deu para aproveitar (precisa de nome e de alguma arte).
static int ofMeta(const char *start, const char *end, const char *kind, CatItem *d) {
  char v[900];
  memset(d, 0, sizeof *d);
  if (!js_text(start, end, "name", d->title, sizeof d->title)) return 0;
  // O poster e o unico obrigatorio: sem ele o card fica um retangulo cinza.
  if (!js_text(start, end, "poster", d->poster, sizeof d->poster)) return 0;
  js_text(start, end, "background", d->backdrop, sizeof d->backdrop);
  js_text(start, end, "logo", d->logo, sizeof d->logo);
  // O TMDB serve o backdrop em /original/, que e 3840x2160. O download nem e o
  // problema (268 KB contra 201 KB do w1280) — o problema e o DECODIFICADO:
  // 8,3 MP viram 33 MB em RAM, mais outros 33 MB na conversao de formato, antes
  // de o SDL_BlitScaled reduzir para o teto de 1920. Num nucleo fraco isso e
  // ~0,5 s por arte, e a cada troca de heroi. Em w1280 sao 3,7 MB e ~9x menos
  // trabalho; o heroi e desenhado a 1920, entao amplia 1,5x — com o degrade e o
  // texto por cima, a diferenca nao aparece, e o tranco aparecia.
  //
  // Feito por reescrita de URL e nao pedindo outro campo porque o Cinemeta so
  // devolve este; a escada do TMDB e w300/w780/w1280/original.
  { char *o = strstr(d->backdrop, "/t/p/original/");
    if (o) {
      char new[sizeof d->backdrop];
      snprintf(new, sizeof new, "%.*s/t/p/w1280/%s",
               (int)(o - d->backdrop), d->backdrop, o + 14);
      snprintf(d->backdrop, sizeof d->backdrop, "%s", new);
    } }
  if (!d->backdrop[0]) snprintf(d->backdrop, sizeof d->backdrop, "%s", d->poster);

  if (!js_text(start, end, "imdb_id", d->imdb, sizeof d->imdb))
    js_text(start, end, "id", d->imdb, sizeof d->imdb);
  snprintf(d->kind, sizeof d->kind, "%s", kind);

  { // genero: "Filme · Acao · Drama"
    const char *g = js_array(start, end, "genres");
    char g1[48] = "", g2[48] = "";
    if (g) {
      const char *f1 = js_end(g);
      (void)f1;
      // elementos de texto: copiar direto do array
      { const char *p = g; int k = 0;
        while (p && k < 2) {
          char tmp[48]; size_t n = 0;
          if (*p != '"') break;
          p++;
          while (*p && *p != '"' && n + 1 < sizeof tmp) tmp[n++] = *p++;
          tmp[n] = 0;
          // Traduz AQUI, na entrada: o campo `genero` do CatItem e usado por
          // varias telas e todas mostrariam o ingles se a traducao ficasse no
          // desenho.
          if (k == 0) snprintf(g1, sizeof g1, "%s", disc_genre_label(tmp));
          else        snprintf(g2, sizeof g2, "%s", disc_genre_label(tmp));
          k++;
          p++;
          while (*p == ' ') p++;
          if (*p != ',') break;
          p++;
          while (*p == ' ') p++;
        } }
    }
    snprintf(d->genre, sizeof d->genre, "%s%s%s%s%s",
             strcmp(kind, "series") ? "Film" : "TV Show",
             g1[0] ? "  \xc2\xb7  " : "", g1,
             g2[0] ? "  \xc2\xb7  " : "", g2);
  }
  v[0] = 0;
  js_text(start, end, "releaseInfo", v, sizeof v);
  { char duration[24] = "";
    js_text(start, end, "runtime", duration, sizeof duration);
    // "2024–" vira "2024": o travessao de serie em andamento polui a linha.
    { char *tr = strstr(v, "\xe2\x80\x93"); if (tr) *tr = 0; }
    snprintf(d->meta, sizeof d->meta, "%.20s%s%.20s", v,
             (v[0] && duration[0]) ? "  \xc2\xb7  " : "", duration); }
  js_text(start, end, "description", d->synopsis, sizeof d->synopsis);
  // NAO INVENTAR CLASSIFICACAO. Aqui havia um `"14"` cravado, e o efeito era
  // que TODO titulo vindo da rede exibia o selo "14" — o Cinemeta nao manda
  // classificacao etaria, e o valor de reserva virou uma constante disfarcada
  // de dado, desenhada com a mesma confianca de um campo real.
  //
  // Vazio e a resposta honesta: desenhaSeloMeta ja e guardado por
  // `classificacao[0]` no chamador (detail.c), entao o selo simplesmente nao
  // aparece enquanto nao houver valor. Quem preenche de verdade e a ficha do
  // TMDB em extras.c (release_dates -> certification), que chega depois.
  d->age_rating[0] = 0;
  { double score = js_num(start, end, "imdbRating", 0.0);
    d->score = (int)(score * 10.0 + 0.5) / 1; }
  if (d->score > 99) d->score /= 10;
  return 1;
}

// Le um catalogo (movie|series) de um addon e acrescenta ao vetor.
static int readCatalog(const char *base, const char *kind, const char *id,
                       CatItem *output, int max, int count) {
  char url[900];
  char *body;
  const char *p;
  int n = 0;
  snprintf(url, sizeof url, "%s/catalog/%s/%s.json", base, kind, id);
  // 8 s e nao 25: um addon fora do ar segurava um dos tres fios por 25 s, e a
  // fileira dele atrasa TODAS as seguintes porque a montagem caminha em ordem.
  // E a mesma licao ja registrada no cache de texturas — la o timeout caiu de
  // 25 para 8 pelo mesmo motivo, com duas URLs mortas travando os dois fios de
  // decode. Um catalogo que nao responde em 8 s nao vai responder.
  body = net_download(url, 8);
  if (!body) return 0;
  p = js_array(body, NULL, "metas");
  while (p && n < max && n < count) {
    const char *f = js_end(p);
    if (ofMeta(p, f, kind, &output[n])) n++;
    p = js_next(f);
  }
  free(body);
  return n;
}

// --- fileiras da home: catalogos declarados pelos addons ---------------------
// Isto substitui a lista PREF fixa de quatro catalogos. O app web nao tem
// fileira fixa: cada fileira e um catalogo declarado no manifesto de um addon,
// e a ordem/visibilidade/nome saem de `homeCatalogPrefs`. Ver o comentario
// grande em catalogo.h, que traz o algoritmo de sortAndFilterRowsInternal.

// Teto de catalogos declarados somando TODOS os addons.
//
// Era 64, e o Xperience sozinho declara 64 — o `nDecl < DECL_MAX` do laco
// parava ali, e AIOStreams e Akashi TV nunca tinham o manifesto sequer lido.
// Nem as fileiras deles apareciam na home, nem os catalogos de busca deles
// existiam: o app se comportava como se o dono tivesse instalado um addon so.
// O sintoma que chegou primeiro foi a busca ("nao procura em todos os
// catalogos"), mas o teto cortava tudo.
#define DECL_MAX 256
// Quantos itens cada fileira mostra. A home desenha no maximo MAX_CARDS (12) e
// buscar mais e trafego que ninguem ve.
#define MAX_PER_ROW 12

static CatRow filtersBuilt[CAT_FILTER_MAX];
static int nRowsBuilt;

typedef struct {
  char key[192];      // homeCatalogKey:        <addonId>_<tipo>_<catalogoId>
  char disable[352];  // homeCatalogDisableKey: <base>_<tipo>_<catalogoId>_<nome>
  char title[96];
  char kind[8];
  char id[96];
  const char *base;
  // 1 quando o catalogo aceita BUSCA. O manifesto declara isso em
  // `extra: [{name:"search"}]` (formato novo) ou `extraSupported: ["search"]`
  // (antigo) — os addons do dono usam os dois.
  int searchable;
  char nameAddon[64];   // "Xperience", para a linha "de <addon>" no resultado
} Decl;

// Preferencias do dono, o equivalente local de `homeCatalogPrefs`. Arquivo de
// texto porque o do app web e um localStorage de outro processo — a mesma razao
// que ja valia para o progresso: aquele arquivo pertence a quem o mantem aberto,
// e escrever nele de fora corromperia o estado.
//
//   ordem     <chave>
//   desligada <chave-ou-chave-de-desativar>
//   titulo    <chave><TAB><titulo>
// 64 was not enough: an account with AIOMetadata alone declares 151 catalogues,
// and the saved order covers ALL of them. Cut at 64, the owner's order applied
// only to the start of the list and the rest fell back to the manifest's order —
// which showed up as "the home ignores what I configured".
#define PREF_MAX 256
static char prefOrder[PREF_MAX][192];   static int nPrefOrder;
static char prefOff[PREF_MAX][352];     static int nPrefOff;
static struct { char key[192], title[96]; } prefTitle[PREF_MAX];
static int nPrefTitle;

// 1 when the preferences came from the ACCOUNT. The account wins over the local
// file, as on the web, where the dedicated blob wins over the copy that travels
// inside the profile blob.
static int prefsFromAccount;

void disc_prefs_begin(void) {
  nPrefOrder = nPrefOff = nPrefTitle = 0;
  prefsFromAccount = 0;
}

void disc_prefs_add(const char *key, int enabled, const char *customTitle) {
  if (!key || !*key) return;
  // THE ORDER IS THE ARRIVAL ORDER. The server already sends the items in the
  // sequence the person chose (the `order` field is their index), so sorting
  // again here would only create a second opinion about the same thing.
  if (nPrefOrder < PREF_MAX) snprintf(prefOrder[nPrefOrder++], 192, "%s", key);
  if (!enabled && nPrefOff < PREF_MAX) snprintf(prefOff[nPrefOff++], 352, "%s", key);
  if (customTitle && *customTitle && nPrefTitle < PREF_MAX) {
    snprintf(prefTitle[nPrefTitle].key, 192, "%s", key);
    snprintf(prefTitle[nPrefTitle].title, 96, "%s", customTitle);
    nPrefTitle++;
  }
}

// Reading the preferences, for whoever assembles the rows.
//
// The account's ordered list mixes CATALOGUES and COLLECTIONS, and this file can
// only resolve catalogues — collections exist only for home.c. So the order is
// exposed raw instead of applied here: the home is what can match both kinds,
// and it needs the whole sequence, gaps and all.
int disc_prefs_n(void) { return nPrefOrder; }

const char *disc_prefs_key(int i) {
  return (i >= 0 && i < nPrefOrder) ? prefOrder[i] : "";
}

int disc_prefs_hidden(const char *key) {
  int i;
  if (!key || !*key) return 0;
  for (i = 0; i < nPrefOff; i++) if (!strcmp(prefOff[i], key)) return 1;
  return 0;
}

const char *disc_prefs_title(const char *key) {
  int i;
  if (!key || !*key) return NULL;
  for (i = 0; i < nPrefTitle; i++)
    if (!strcmp(prefTitle[i].key, key)) return prefTitle[i].title;
  return NULL;
}

void disc_prefs_end(void) {
  prefsFromAccount = nPrefOrder > 0;
  printf("[disc] account row prefs: %d ordered, %d disabled, %d renamed\n",
         nPrefOrder, nPrefOff, nPrefTitle);
  fflush(stdout);
}

static void readPrefs(void) {
  char path[600], line[600];
  FILE *f;
  // THE ACCOUNT WINS OVER THE FILE. Clearing here would wipe what the sync just
  // delivered, and the home would fall back to the manifest's order with nothing
  // to show for it.
  if (prefsFromAccount) return;
  nPrefOrder = nPrefOff = nPrefTitle = 0;
  if (!dirArtDisc[0]) return;
  snprintf(path, sizeof path, "%s/rows.txt", dirArtDisc);
  f = fopen(path, "r");
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    char *end = line + strlen(line);
    char *arg;
    while (end > line && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
    if (!line[0] || line[0] == '#') continue;
    arg = strchr(line, ' ');
    if (!arg) continue;
    *arg++ = 0;
    while (*arg == ' ') arg++;
    if (!strcmp(line, "order") && nPrefOrder < PREF_MAX) {
      snprintf(prefOrder[nPrefOrder++], 192, "%s", arg);
    } else if (!strcmp(line, "off") && nPrefOff < PREF_MAX) {
      snprintf(prefOff[nPrefOff++], 352, "%s", arg);
    } else if (!strcmp(line, "title") && nPrefTitle < PREF_MAX) {
      char *tab = strchr(arg, '\t');
      if (!tab) continue;
      *tab++ = 0;
      snprintf(prefTitle[nPrefTitle].key, 192, "%s", arg);
      snprintf(prefTitle[nPrefTitle].title, 96, "%s", tab);
      nPrefTitle++;
    }
  }
  fclose(f);
  printf("[disc] row prefs: %d ordered, %d disabled, %d renamed\n",
         nPrefOrder, nPrefOff, nPrefTitle);
}

// A conferencia e contra DUAS chaves, como no web: quem desliga pela tela de
// ajustes grava a chave de desativar (que carrega a URL base e o nome), e quem
// desliga pela ordenacao grava a chave curta.
static int off(const Decl *d) {
  int i;
  for (i = 0; i < nPrefOff; i++)
    if (!strcmp(prefOff[i], d->key) || !strcmp(prefOff[i], d->disable)) return 1;
  return 0;
}

// formatCatalogRowTitle (js/ui/screens/home/homeUtils.js:62): primeira letra
// maiuscula e, se o nome ja NAO termina com o rotulo do tipo, " - <tipo>".
// E por isso que a home mostra "For You - Filme" e nao "for you".
static void formatTitle(const char *name, const char *kind, char *dst, size_t size) {
  const char *label = strcmp(kind, "series") ? "Film" : "Series";
  const char *raw    = strcmp(kind, "series") ? "Movie" : "Series";
  size_t ln = strlen(name), lr = strlen(label), lc = strlen(raw);
  int alreadyHas = 0;
  if (!name[0]) { snprintf(dst, size, "%s", label); return; }
  if (ln >= lr && !strcasecmp(name + ln - lr, label)) alreadyHas = 1;
  if (ln >= lc && !strcasecmp(name + ln - lc, raw))    alreadyHas = 1;
  if (alreadyHas) snprintf(dst, size, "%s", name);
  else       snprintf(dst, size, "%s - %s", name, label);
  if (dst[0] >= 'a' && dst[0] <= 'z') dst[0] = (char)(dst[0] - 32);
}

// The host of a URL, so it can be LOGGED without leaking a secret. An addon's
// path carries the debrid key and the owner's token; the host does not. A log
// nobody can show to anyone ends up being a log nobody reads.
static void hostOf(const char *url, char *dst, size_t size) {
  const char *a = strstr(url ? url : "", "://");
  const char *b;
  size_t n;
  a = a ? a + 3 : (url ? url : "");
  b = strchr(a, '/');
  n = b ? (size_t)(b - a) : strlen(a);
  if (n >= size) n = size - 1;
  memcpy(dst, a, n);
  dst[n] = 0;
}

// Le <base>/manifest.json e acrescenta os catalogos declarados.
static int readManifest(const char *base, Decl *output, int max) {
  char url[900], addonId[96] = "", name[96], kind[8], id[96];
  char host[128];
  char *body;
  const char *p, *end;
  int n = 0;
  snprintf(url, sizeof url, "%s/manifest.json", base);
  hostOf(url, host, sizeof host);
  body = net_download(url, 20);
  // WHY THIS IS LOGGED. An addon that does not answer, or answers something with
  // no "catalogs", produced exactly the same visible result as an addon that
  // simply has no catalogues: the home fell back to the packaged catalogue and
  // nobody could tell which of the two had happened. "0 catalogues declared" is
  // the total; these lines say WHO contributed zero, and why.
  if (!body) {
    printf("[disc] manifest %s: NO RESPONSE (network, TLS or 404)\n", host);
    fflush(stdout);
    return 0;
  }
  end = body + strlen(body);
  js_text(body, end, "id", addonId, sizeof addonId);
  // The id is REMEMBERED on the addon. This is the only place in the app where a
  // base and an id meet, and it is how the owner's collection sources — which
  // reference catalogues by addonId — find the address to fetch from.
  addons_note_id(base, addonId);
  p = js_array(body, end, "catalogs");
  // Sem `n < max` na condicao: o vetor de fileiras pode encher, mas a varredura
  // continua ate o fim do manifesto porque os catalogos de BUSCA costumam estar
  // no fim dele (o Xperience poe os dele em 603/604 de 605). Quem para de
  // gravar e o `if (n < max)` la dentro.
  while (p) {
    const char *f = js_end(p);
    kind[0] = id[0] = name[0] = 0;
    js_text(p, f, "type", kind, sizeof kind);
    js_text(p, f, "id",   id,   sizeof id);
    js_text(p, f, "name", name, sizeof name);
    // Sem tipo ou sem id nao da para montar a URL do catalogo; e um catalogo
    // que nao responde e pior que uma fileira a menos.
    if (kind[0] && id[0]) {
      Decl local, *d;
      // Vetor cheio: usa um Decl de rascunho so para decidir/registrar a busca.
      d = (n < max) ? &output[n] : &local;
      memset(d, 0, sizeof *d);
      d->base = base;
      // BUSCA: procura "search" dentro do bloco `extra`/`extraSupported` DESTE
      // catalogo (a faixa [p,f) e o objeto dele, entao nao vaza para o vizinho).
      //
      // So filme e serie. O Akashi declara busca em `event` e `channel`
      // tambem, e o AIOStreams em `collections` — tipos que este app nao tem
      // tela para mostrar. Consultar seria trafego que nao vira nada.
      { const char *ex = strstr(p, "\"extra\"");
        if (!ex || ex >= f) ex = strstr(p, "\"extraSupported\"");
        if (ex && ex < f) {
          const char *sc = strstr(ex, "\"search\"");
          if (sc && sc < f) d->searchable = 1;
        }
        if (strcmp(kind, "movie") && strcmp(kind, "series")) d->searchable = 0;
        // Registra AQUI, e nao depois varrendo o vetor de Decl.
        //
        // O Xperience declara 605 catalogos e poe os dois de BUSCA nas duas
        // ULTIMAS posicoes (603 e 604). Qualquer teto no vetor de fileiras da
        // home — 64, 256, o numero que for — corta exatamente os catalogos que
        // interessam a busca. Os dois assuntos nao tem por que compartilhar
        // limite: sao 16 alvos de busca contra centenas de fileiras.
        if (d->searchable) {
          char label[96], nameAddon[96] = "";
          js_text(body, end, "name", nameAddon, sizeof nameAddon);
          formatTitle(name, kind, label, sizeof label);
          disc_target_search(base, kind, id, label,
                          nameAddon[0] ? nameAddon
                                       : (addonId[0] ? addonId : "addon"));
        } }
      snprintf(d->kind, sizeof d->kind, "%s", kind);
      snprintf(d->id,   sizeof d->id,   "%s", id);
      snprintf(d->key, sizeof d->key, "%s_%s_%s",
               addonId[0] ? addonId : base, kind, id);
      snprintf(d->disable, sizeof d->disable, "%s_%s_%s_%s", base, kind, id, name);
      formatTitle(name, kind, d->title, sizeof d->title);
      // Nome legivel do addon, para a linha "de <addon>" sob o titulo da
      // fileira de resultados. O manifesto tem `name`; sem ele fica o id.
      { char an[96] = "";
        js_text(body, end, "name", an, sizeof an);
        snprintf(d->nameAddon, sizeof d->nameAddon, "%s",
                 an[0] ? an : (addonId[0] ? addonId : "addon")); }
      if (n < max) n++;
    }
    p = js_next(f);
  }
  { size_t bytes = strlen(body);
    const char *has = js_array(body, end, "catalogs");
    if (!has)
      printf("[disc] manifest %s: %u bytes, NO \"catalogs\" array\n",
             host, (unsigned)bytes);
    else
      printf("[disc] manifest %s: %u bytes, %d catalogue(s) usable\n",
             host, (unsigned)bytes, n);
    fflush(stdout); }
  free(body);
  return n;
}

// --- LEITURA DOS CATALOGOS EM PARALELO ---------------------------------------
//
// Eram ate 16 GET em SERIE, 25 s de timeout cada. Medido no Mac: 8,7 s entre a
// primeira fileira aparecer (4,0 s) e o catalogo ficar completo (12,8 s), e na
// TV e pior. Sao pedidos INDEPENDENTES — nada em lerCatalogo/deMeta toca estado
// compartilhado (a tabela de generos e const), e rede_baixar ja roda em tres
// fios na busca.
//
// A ORDEM DAS FILEIRAS TEM DE SER PRESERVADA: ela sai de art/fileiras.txt e e
// preferencia do dono. Por isso os fios escrevem cada um no SEU balde e quem
// monta caminha na ordem, esperando o balde k ficar pronto. O resultado e a
// mesma ordem de antes, com o tempo do MAIOR pedido em vez da SOMA.
#define CAT_THREADS 3

typedef struct {
  const Decl *d;
  CatItem items[MAX_PER_ROW];
  int  n;
  int  ready;
} TaskCat;

static TaskCat *tasks;
static int  nTasks, nextTask;
static pthread_mutex_t catLock = PTHREAD_MUTEX_INITIALIZER;

static void *threadCatalog(void *u) {
  (void)u;
  for (;;) {
    int mine, got;
    const Decl *d;
    pthread_mutex_lock(&catLock);
    if (nextTask >= nTasks) { pthread_mutex_unlock(&catLock); return NULL; }
    mine = nextTask++;
    d = tasks[mine].d;
    pthread_mutex_unlock(&catLock);

    got = readCatalog(d->base, d->kind, d->id, tasks[mine].items,
                      MAX_PER_ROW, MAX_PER_ROW);

    pthread_mutex_lock(&catLock);
    tasks[mine].n = got;
    tasks[mine].ready = 1;
    pthread_mutex_unlock(&catLock);
  }
}

static void *build(void *u) {
  // O lote tambem cresce: era dimensionado por CAT_MAX e por isso herdava o
  // mesmo teto arbitrario.
  int cap = 128;
  CatItem *lote = malloc(sizeof(CatItem) * (size_t)cap);
  int n = 0, i;
  int nResume = 0, nSocial = 0;
  (void)u;
  if (!lote) { searching = 0; return NULL; }

  // O "continue assistindo" vem PRIMEIRO e do Trakt. A home usa as primeiras
  // posicoes do catalogo nessa fileira, entao a ordem aqui e o que define o
  // que aparece la — e o historico tem de ganhar das recomendacoes.
  mark("build: start");
  nResume = trakt_resume(lote, 8);
  n += nResume;
  mark("trakt continue watching");
  // O feed social oficial e uma fileira propria, logo depois do retorno ao
  // que estava sendo visto. Ele vem cedo para nao depender dos manifestos dos
  // addons e usa a mesma credencial Trakt ja carregada.
  nSocial = trakt_social(lote + n, 8);
  n += nSocial;
  mark("trakt friend activity");
  // O historico do Trakt e a PRIMEIRA fileira da home e chega ~1,6 s antes dos
  // manifestos. Publicar aqui poe conteudo na tela nesse instante em vez de
  // segurar tudo ate o fim.
  // Monta direto em filsMontadas: o vetor local `fil` so existe mais abaixo, e
  // criar um aqui so para copiar seria trabalho a toa.
  if (n > 0 && !cat_do_cache()) {
    int nf = 0;
    if (nResume > 0) {
      CatRow *f0 = &filtersBuilt[nf++];
      memset(f0, 0, sizeof *f0);
      snprintf(f0->key,  sizeof f0->key,  "continue_watching");
      snprintf(f0->title, sizeof f0->title, "Continue watching");
      snprintf(f0->kind,   sizeof f0->kind,   "movie");
      f0->start = 0; f0->n = nResume;
    }
    if (nSocial > 0) {
      CatRow *fs = &filtersBuilt[nf++];
      memset(fs, 0, sizeof *fs);
      snprintf(fs->key, sizeof fs->key, "social_activity");
      snprintf(fs->title, sizeof fs->title, "Friends watching");
      snprintf(fs->kind, sizeof fs->kind, "social");
      fs->start = nResume; fs->n = nSocial;
    }
    nRowsBuilt = nf;
    cat_set_all(lote, n, filtersBuilt, nf);
    mark("continue watching on screen");
  }
#define ENSURES(count) do { \
    if (n + (count) > cap) { \
      int newCap = cap; \
      CatItem *larger; \
      while (newCap < n + (count)) newCap *= 2; \
      larger = realloc(lote, sizeof(CatItem) * (size_t)newCap); \
      if (larger) { lote = larger; cap = newCap; } \
    } } while (0)

  // As fileiras vem dos CATALOGOS declarados nos manifestos dos addons, e nao
  // de uma lista fixa. A ordem, o que fica de fora e os nomes seguem o
  // algoritmo do web (sortAndFilterRowsInternal), com as preferencias lidas de
  // art/fileiras.txt.
  {
    // static: 256 entradas passam de 200 KB, e isso nao cabe com folga na
    // pilha de um fio. montar() roda uma vez e num fio so, entao nao ha
    // reentrada que isto quebre.
    static Decl decls[DECL_MAX];
    int nDecl = 0, k;
    CatRow filter[CAT_FILTER_MAX];
    int nFilter = 0;
    // A fileira 0 e "Continuar assistindo", que ja foi montada acima. Ela e
    // SINTETICA: nao esta na ordem do web e nao pode ser desligada por chave —
    // no app ela existe sempre que ha progresso.
    if (nResume > 0) {
      CatRow *f0 = &filter[nFilter++];
      memset(f0, 0, sizeof *f0);
      snprintf(f0->key,  sizeof f0->key,  "continue_watching");
      snprintf(f0->title, sizeof f0->title, "Continue watching");
      snprintf(f0->kind,   sizeof f0->kind,   "movie");
      f0->start = 0; f0->n = nResume;
    }
    if (nSocial > 0) {
      CatRow *fs = &filter[nFilter++];
      memset(fs, 0, sizeof *fs);
      snprintf(fs->key, sizeof fs->key, "social_activity");
      snprintf(fs->title, sizeof fs->title, "Friends watching");
      snprintf(fs->kind, sizeof fs->kind, "social");
      fs->start = nResume; fs->n = nSocial;
    }

    readPrefs();
    // Zera ANTES de ler os manifestos: cada catalogo com busca se registra
    // sozinho la dentro, na hora em que e lido.
    disc_targets_search_reset();
    // Sem `nDecl < DECL_MAX` no laco: com o vetor cheio o manifesto do addon
    // seguinte nem era baixado, e AIOStreams e Akashi TV ficavam invisiveis
    // para o app inteiro so porque o Xperience, lido antes, declara 605
    // catalogos. lerManifesto ja para de GRAVAR sozinho quando enche.
    for (i = 0; i < addons_n(); i++) {
      if (!addons_has_catalog(i)) continue;
      nDecl += readManifest(addons_base(i), decls + nDecl, DECL_MAX - nDecl);
    }
    printf("[disc] %d catalogues declared by the addons\n", nDecl);

    // ALVOS DE BUSCA. Independem da ordem/filtro das FILEIRAS da home: um
    // catalogo pode estar desativado na home e ainda assim ser bom para
    // procurar (o Akashi so tem busca, nao tem fileira que valha a pena).
    printf("[disc] %d search targets\n", disc_search_n_targets());
    mark("manifests read");

    // ensureOrderKeysWithPrefs: a ordem salva primeiro, e as chaves NOVAS
    // acrescentadas no fim. Catalogo que o addon passou a declarar hoje entra
    // por ultimo, nao no meio — e o que evita a home se reorganizar sozinha.
    {
      int order[DECL_MAX];
      int nOrder = 0, j;
      char watched[DECL_MAX];
      memset(watched, 0, sizeof watched);
      for (k = 0; k < nPrefOrder; k++)
        for (j = 0; j < nDecl; j++)
          if (!watched[j] && !strcmp(decls[j].key, prefOrder[k])) {
            order[nOrder++] = j; watched[j] = 1; break;
          }
      for (j = 0; j < nDecl; j++) if (!watched[j]) order[nOrder++] = j;

      int markedFirst = 0;
      // ETAPA 1 — escolher e ORDENAR as fileiras que serao lidas. Os filtros
      // (desligada, titulo personalizado) sao locais e baratos; fazer isto
      // antes deixa os fios so com a parte cara, que e a rede.
      nTasks = 0; nextTask = 0;
      tasks = calloc(CAT_FILTER_MAX, sizeof(TaskCat));
      for (k = 0; k < nOrder && nTasks < CAT_FILTER_MAX; k++) {
        Decl *d = &decls[order[k]];
        int t;
        if (off(d)) continue;
        // customTitles ganha do nome do manifesto.
        for (t = 0; t < nPrefTitle; t++)
          if (!strcmp(prefTitle[t].key, d->key)) {
            snprintf(d->title, sizeof d->title, "%s", prefTitle[t].title);
            break;
          }
        if (tasks) tasks[nTasks++].d = d;
      }

      // ETAPA 2 — CAT_FIOS trabalhando na fila. Se o calloc falhar ou nao
      // houver o que ler, nTarefas fica 0 e o laco de montagem abaixo nao roda:
      // a home segue com o que ja foi publicado, sem caminho de erro proprio.
      { pthread_t threads[CAT_THREADS];
        int created = 0, q;
        for (q = 0; q < CAT_THREADS && nTasks > 0; q++)
          if (pthread_create(&threads[created], NULL, threadCatalog, NULL) == 0) created++;
        // Sem NENHUM fio (pthread_create falhou em todos), le em serie no
        // proprio fio: pior desempenho, mesmo resultado. Melhor que home vazia.
        if (!created && nTasks > 0) threadCatalog(NULL);

        // ETAPA 3 — montar NA ORDEM, publicando cada fileira assim que o balde
        // dela fica pronto. Esperar o balde k nao desperdica tempo: os fios
        // seguem enchendo k+1, k+2 enquanto este e consumido.
        for (k = 0; k < nTasks && nFilter < CAT_FILTER_MAX; k++) {
          const Decl *d = tasks[k].d;
          int got;
          for (;;) {
            int pr;
            pthread_mutex_lock(&catLock);
            pr = tasks[k].ready;
            pthread_mutex_unlock(&catLock);
            if (pr) break;
            SDL_Delay(10);
          }
          got = tasks[k].n;
          if (!got) continue;   // fileira vazia nao vira titulo pendurado
          ENSURES(MAX_PER_ROW + 2);
          if (got > cap - n) got = cap - n;
          if (got <= 0) continue;
          memcpy(lote + n, tasks[k].items, sizeof(CatItem) * (size_t)got);
        {
          CatRow *f = &filter[nFilter++];
          memset(f, 0, sizeof *f);
          snprintf(f->key,  sizeof f->key,  "%s", d->key);
          snprintf(f->title, sizeof f->title, "%s", d->title);
          snprintf(f->kind,   sizeof f->kind,   "%s", d->kind);
          snprintf(f->base,   sizeof f->base,   "%s", d->base ? d->base : "");
          snprintf(f->catId,  sizeof f->catId,  "%s", d->id);
          f->start = n; f->n = got;
        }
        n += got;
        printf("[disc] row %d: %s (%d)\n", nFilter - 1, d->title, got);
        // PUBLICA A CADA FILEIRA, em vez de so no fim.
        //
        // Medido no Mac: o catalogo da rede so aparecia aos 12.991 ms, e ate
        // la a home mostrava apenas o catalogo estatico do pacote. Na TV e
        // pior. A referencia mostra conteudo em 1,7 s — nao porque a rede dela
        // seja mais rapida, mas porque ela mostra o que ja tem.
        //
        // cat_definir_tudo troca o bloco inteiro de uma vez (catalogo.c), entao
        // publicar N vezes e seguro para quem esta desenhando; o custo e uma
        // copia do vetor por fileira, que acontece no fio da descoberta e nao
        // no de desenho.
        nRowsBuilt = nFilter;
        memcpy(filtersBuilt, filter, sizeof(CatRow) * (size_t)nFilter);
        // So publica em partes se a tela estiver com o catalogo do PACOTE.
        // Sobre o cache seria um retrocesso visivel: 16 fileiras viram 1.
        if (!cat_do_cache())
          cat_set_all(lote, n, filtersBuilt, nRowsBuilt);
        // Bandeira propria: `nFil == 1` nunca acontece aqui porque a fileira
        // "Continuar assistindo" ja ocupou a posicao 0 antes do laco.
        if (!markedFirst) { markedFirst = 1;
                               mark("first network row on screen"); }
        }
        for (q = 0; q < created; q++) pthread_join(threads[q], NULL);
      }
      free(tasks); tasks = NULL; nTasks = 0;
      nRowsBuilt = nFilter;
      memcpy(filtersBuilt, filter, sizeof(CatRow) * (size_t)nFilter);
    }
  }

  // Watchlist e colecao entram DEPOIS das recomendacoes, e nao antes.
  // A home usa as PRIMEIRAS posicoes do catalogo nas suas fileiras; com as
  // listas do Trakt na frente (e elas passam de 60 itens cada) as fileiras
  // viravam a watchlist inteira e as recomendacoes nunca apareciam. A
  // biblioteca varre o catalogo todo procurando as marcas, entao para ela
  // tanto faz onde estao.
  ENSURES(400);
  n += trakt_list("watchlist",  lote + n, cap - n);
  ENSURES(400);
  n += trakt_list("collection", lote + n, cap - n);
#undef ENSURES

  if (n) {
    cat_set_all(lote, n, filtersBuilt, nRowsBuilt);
    cat_cache_replaced();
    mark("network catalog published");
    printf("[disc] catalog built with %d titles\n", n);
    // Grava so o resultado COMPLETO, nao as publicacoes parciais: um cache
    // com tres fileiras faria a proxima abertura nascer pela metade e so
    // completar quando a rede respondesse — exatamente o que o cache existe
    // para evitar.
    cat_write_cache(dirArtDisc);
  } else {
    printf("[disc] nothing came from the network; using the packaged catalog\n");
  }
  fflush(stdout);
  free(lote);
  searching = 0;
  return NULL;
}

void disc_start(void) {
  if (searching) return;
  searching = 1;
  if (pthread_create(&thread, NULL, build, NULL) != 0) searching = 0;
  else pthread_detach(thread);
}

// A REQUEST to rebuild, not the rebuild itself.
//
// The case this fixes: the addon list comes from the ACCOUNT and arrives with the
// sync, well after the home has already been assembled. The first build ran with
// zero addons, read zero manifests, found zero catalogues and fell back to the
// packaged catalogue — and nothing ever reconsidered it. The owner saw the
// packager's catalogue instead of their own, on every launch, because the list is
// not kept between sessions either.
//
// It stays a REQUEST because the first build may still be running when the sync
// answers: disc_start gives up silently while one is in flight, and the call
// would be lost in exactly the race this code exists to resolve.
static volatile int rebuildWanted;

void disc_rebuild(void) { rebuildWanted = 1; }

void disc_step(void) {
  if (!rebuildWanted || searching) return;
  rebuildWanted = 0;
  printf("[disc] rebuilding the rows with the account's addons\n");
  fflush(stdout);
  disc_start();
}

// --- episodios sob demanda ---------------------------------------------------

// CACHE LRU DO /meta DAS SERIES.
//
// UMA resposta do Cinemeta traz TODAS as temporadas: o `videos` vem inteiro e o
// filtro por temporada acontece aqui embaixo, de graca. Mesmo assim cada troca
// de temporada rebaixava o corpo todo — numa serie longa sao centenas de
// kilobytes de JSON por pilula apertada, e era isso que o dono sentia como
// "demora para atualizar quando troca de temporada".
//
// Guardar apenas a ultima serie fazia voltar ao titulo anterior repetir a
// transferencia inteira. Quatro respostas cobrem a navegacao normal de ida e
// volta sem deixar o uso de memoria crescer sem limite.
#define META_CACHE_N 4
static struct { char id[24]; char *body; unsigned usage; } metaCache[META_CACHE_N];
static unsigned metaClock;
static pthread_mutex_t metaLock = PTHREAD_MUTEX_INITIALIZER;

static char *metaCacheGet(const char *id) {
  char *r = NULL;
  pthread_mutex_lock(&metaLock);
  for (int i = 0; i < META_CACHE_N; i++)
    if (metaCache[i].body && !strcmp(metaCache[i].id, id)) {
      metaCache[i].usage = ++metaClock;
      r = strdup(metaCache[i].body); /* o fio trabalha em copia estavel */
      break;
    }
  pthread_mutex_unlock(&metaLock);
  return r;
}

static void metaCacheStore(const char *id, const char *body) {
  int slot = 0;
  char *copy = strdup(body);
  if (!copy) return;
  pthread_mutex_lock(&metaLock);
  for (int i = 0; i < META_CACHE_N; i++) {
    if (metaCache[i].body && !strcmp(metaCache[i].id, id)) { slot = i; break; }
    if (!metaCache[i].body || metaCache[i].usage < metaCache[slot].usage) slot = i;
  }
  free(metaCache[slot].body);
  metaCache[slot].body = copy;
  metaCache[slot].usage = ++metaClock;
  snprintf(metaCache[slot].id, sizeof metaCache[slot].id, "%s", id);
  pthread_mutex_unlock(&metaLock);
}

// Publica a parte critica antes de qualquer enriquecimento opcional. Assim a
// fileira de episodios aparece depois da primeira resposta, sem esperar pelas
// duas viagens ao TMDB usadas para foto e personagem do elenco.
static int publishEpisodes(const char *body, int targetItem, const char *title) {
#define VIDEOS_MAX 600
  CatEp *eps = malloc(sizeof(CatEp) * VIDEOS_MAX);
  int n = 0;
  if (!eps) return 0;
  const char *p = js_array(body, NULL, "videos");
  while (p && n < VIDEOS_MAX) {
    const char *f = js_end(p);
    int t = (int)js_num(p, f, "season", -1);
    if (t > 0) {
      CatEp *e = &eps[n];
      char d[24] = "";
      memset(e, 0, sizeof *e);
      e->season = t;
      e->episode = (int)js_num(p, f, "episode", 0);
      js_text(p, f, "name", e->name, sizeof e->name);
      js_text(p, f, "overview", e->synopsis, sizeof e->synopsis);
      js_text(p, f, "thumbnail", e->thumb, sizeof e->thumb);
      js_text(p, f, "released", d, sizeof d);
      disc_date_long(d, e->date, sizeof e->date);
      n++;
    }
    p = js_next(f);
  }
  for (int i = 1; i < n; i++) {
    CatEp k = eps[i];
    int j;
    for (j = i - 1; j >= 0 &&
         (eps[j].season > k.season ||
          (eps[j].season == k.season && eps[j].episode > k.episode)); j--)
      eps[j + 1] = eps[j];
    eps[j + 1] = k;
  }
  if (n) cat_set_episodes(targetItem, eps, n);
  free(eps);
  mark("episodes on screen");
  printf("[disc] %s: %d episodes published before the extras\n", title, n);
  fflush(stdout);
  return n;
}

static void *fetchEps(void *u) {
  int targetItem = epItem;
  const CatItem *orig = cat_item(targetItem);
  CatItem base;
  const CatItem *it;
  char url[600], *body = NULL;
  char series[24];
  (void)u;
  if (!orig || !orig->imdb[0]) { threadEpAlive = 0; return NULL; }
  // FILME TAMBEM PASSA AQUI. O /meta/movie traz elenco, direcao, generos e
  // nota — antes so os titulos enriquecidos no catalogo tinham elenco, e a
  // pagina do filme abria sem a fileira. O que e so de serie (episodios,
  // temporadas) e pulado abaixo.
  int isMovie = strcmp(orig->kind, "series") != 0;
  base = *orig;
  it = &base;
  { const char *dp;
    snprintf(series, sizeof series, "%s", it->imdb);
    dp = strchr(series, ':');
    if (dp) *(char *)dp = 0;
    snprintf(url, sizeof url, "%s/meta/%s/%s.json", CINEMETA,
             isMovie ? "movie" : "series", series); }

  body = metaCacheGet(series);
  mark(body ? "episodes: meta from cache" : "episodes: downloading meta");

  if (!body) {
    body = net_download(url, 25);
    if (!body) { threadEpAlive = 0; return NULL; }
    metaCacheStore(series, body);
  }
  if (!isMovie) publishEpisodes(body, targetItem, it->title);
  // A MESMA resposta traz elenco, direcao e a lista de temporadas. Buscar de
  // novo para cada uma seria tres viagens ao mesmo lugar.
  {
    CatItem edit = *it;
    const char *c = js_array(body, NULL, "cast");
    int k = 0;
    while (c && k < 6) {
      size_t n2 = 0;
      const char *p2 = c;
      if (*p2 != '"') break;
      p2++;
      while (*p2 && *p2 != '"' && n2 + 1 < sizeof edit.cast[k].name)
        edit.cast[k].name[n2++] = *p2++;
      edit.cast[k].name[n2] = 0;
      edit.cast[k].role[0] = 0;   // o Cinemeta nao diz o personagem
      edit.cast[k].photo[0] = 0;
      k++;
      p2++;
      while (*p2 == ' ') p2++;
      c = (*p2 == ',') ? p2 + 1 : NULL;
      while (c && *c == ' ') c++;
    }
    edit.nCast = k;
    { const char *dr = js_array(body, NULL, "director");
      if (dr && *dr == '"') {
        size_t n2 = 0;
        dr++;
        while (*dr && *dr != '"' && n2 + 1 < sizeof edit.directing)
          edit.directing[n2++] = *dr++;
        edit.directing[n2] = 0;
      } }
    // GENEROS, NOTA E PAIS. Vinham so do CATALOGO, e o catalogo do Cinemeta nao
    // traz nenhum dos tres: a linha de meta ficava com o TIPO ("Programa de TV")
    // no lugar dos generos, sem selo do IMDb e sem pais. O /meta traz os tres, e
    // esta funcao ja tem a resposta na mao — deixar de ler era desperdicio de uma
    // viagem que ja foi paga.
    { const char *g = js_array(body, NULL, "genres");
      char list[160]; size_t n3 = 0;
      list[0] = 0;
      while (g && *g == '"' && n3 + 1 < sizeof list) {
        const char *p2 = g + 1;
        if (n3) { // separador do web: espaco, ponto medio, espaco
          if (n3 + 4 >= sizeof list) break;
          list[n3++] = ' '; list[n3++] = '\xc2'; list[n3++] = '\xb7'; list[n3++] = ' ';
        }
        while (*p2 && *p2 != '"' && n3 + 1 < sizeof list) list[n3++] = *p2++;
        list[n3] = 0;
        if (*p2 == '"') p2++;
        while (*p2 == ' ') p2++;
        g = (*p2 == ',') ? p2 + 1 : NULL;
        while (g && *g == ' ') g++;
      }
      if (list[0]) snprintf(edit.genre, sizeof edit.genre, "%s", list); }
    { double score = js_num(body, NULL, "imdbRating", 0.0);
      // O campo vem como "8.1" (string ou numero); guardamos por 10 para caber
      // em int sem perder a casa decimal, como o resto do catalogo ja faz.
      if (score > 0.0) {
        int n10 = (int)(score * 10.0 + 0.5);
        if (n10 > 99) n10 /= 10;      // ja veio multiplicado
        edit.score = n10;
      } }
    js_text(body, NULL, "country", edit.pais, sizeof edit.pais);
    // Temporadas presentes, sem repetir e em ordem.
    { const char *v = js_array(body, NULL, "videos");
      edit.nSeasons = 0;
      while (v) {
        const char *fv = js_end(v);
        int t2 = (int)js_num(v, fv, "season", -1);
        if (t2 > 0) {
          int j, found = 0;
          for (j = 0; j < edit.nSeasons; j++)
            if (edit.seasons[j] == t2) { found = 1; break; }
          if (!found && edit.nSeasons < 12) edit.seasons[edit.nSeasons++] = t2;
        }
        v = js_next(fv);
      }
      { int i2, j2, tmp;
        for (i2 = 0; i2 < edit.nSeasons; i2++)
          for (j2 = i2 + 1; j2 < edit.nSeasons; j2++)
            if (edit.seasons[j2] < edit.seasons[i2]) {
              tmp = edit.seasons[i2];
              edit.seasons[i2] = edit.seasons[j2];
              edit.seasons[j2] = tmp;
            } } }
    // Publica texto, generos e temporadas antes do enriquecimento de imagens.
    cat_update_item(targetItem, &edit);
    mark("detail: basic meta on screen");
    { char idBase[24];
      const char *dp;
      snprintf(idBase, sizeof idBase, "%s", it->imdb);
      dp = strchr(idBase, ':');
      if (dp) *(char *)dp = 0;
      photosOfCast(&edit, idBase, !strcmp(it->kind, "series")); }
    cat_update_item(targetItem, &edit);
    printf("[disc] %s: %d actors, dir='%s', %d seasons\n",
           edit.title, edit.nCast, edit.directing, edit.nSeasons);
    fflush(stdout);
  }

  free(body);
  threadEpAlive = 0;
  return NULL;
}

// Pedido AINDA NAO ATENDIDO, quando um chega com um fio em voo. Antes isto era
// `if (fioEpVivo) return;` — o pedido era largado no chao, e trocar de
// temporada enquanto a anterior carregava deixava a lista na temporada ERRADA
// para sempre, sem nova tentativa. Guardar o ultimo (nao enfileirar todos) e o
// certo: o dono quer a temporada onde ele PAROU, nao as que ele atravessou.
static int pendingItem = -1, pendingTemp;

// --- VER TUDO ----------------------------------------------------------------
//
// Uma lista SEPARADA do catalogo da home, de proposito: a home guarda 12 por
// fileira e e ela que a biblioteca e a busca varrem. Despejar 200 itens de um
// catalogo ali dentro mudaria o que essas duas telas veem por causa de uma
// navegacao que o dono pode fechar no segundo seguinte.
#define SEEALL_STEP 100        // `skipStep` padrao do web quando o addon nao diz

static CatItem  seeallItems[SEEALL_MAX];
static int      seeallN;
static char     seeallBase[600], seeallKind[8], seeallCat[96], seeallGenre[96];
static int      seeallPagina, seeallEnd, seeallThreadAlive, seeallError;
static unsigned seeallGeneration;
static pthread_mutex_t seeallLock = PTHREAD_MUTEX_INITIALIZER;

static void *threadSeeAll(void *u) {
  (void)u;
  for (;;) {
  char url[1600], base[600], type[8], id[96], genre[96], encoded[290], *body;
  int raw=0, skip, cap;unsigned generation;
  pthread_mutex_lock(&seeallLock);
  skip=seeallPagina;generation=seeallGeneration;
  snprintf(base,sizeof base,"%s",seeallBase);snprintf(type,sizeof type,"%s",seeallKind);
  snprintf(id,sizeof id,"%s",seeallCat);snprintf(genre,sizeof genre,"%s",seeallGenre);
  pthread_mutex_unlock(&seeallLock);
  int z=0;
  for(const unsigned char *c=(const unsigned char *)genre;*c&&z<(int)sizeof encoded-4;c++) {
    if((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='-'||*c=='_')encoded[z++]=*c;
    else {snprintf(encoded+z,4,"%%%02X",*c);z+=3;}
  }encoded[z]=0;
  if(genre[0])snprintf(url,sizeof url,"%s/catalog/%s/%s/genre=%s&skip=%d.json",base,type,id,encoded,skip);
  else if(skip)snprintf(url,sizeof url,"%s/catalog/%s/%s/skip=%d.json",base,type,id,skip);
  else snprintf(url,sizeof url,"%s/catalog/%s/%s.json",base,type,id);
  body=net_download(url,10);
  cap=strstr(id,"top100")?100:strstr(id,"top250")?250:SEEALL_MAX;
  pthread_mutex_lock(&seeallLock);
  if(generation!=seeallGeneration){pthread_mutex_unlock(&seeallLock);free(body);continue;}
  pthread_mutex_unlock(&seeallLock);
  const char *first=body?js_array(body,NULL,"metas"):NULL;
  int valid=body&&strstr(body,"\"metas\"");
  int added=0;
  for(const char *p=first;p;p=js_next(js_end(p))) {
    const char *f=js_end(p);raw++;
    CatItem it;
    if (ofMeta(p, f, type, &it)) {
      pthread_mutex_lock(&seeallLock);
      int duplicate=0;
      for(int i=0;i<seeallN;i++)if(it.imdb[0]&&!strcmp(seeallItems[i].imdb,it.imdb)&&!strcmp(seeallItems[i].kind,it.kind)){duplicate=1;break;}
      if(generation==seeallGeneration&&seeallN<cap&&!duplicate){seeallItems[seeallN++]=it;added++;}
      pthread_mutex_unlock(&seeallLock);
    }
  }
  free(body);
  pthread_mutex_lock(&seeallLock);
  if(generation!=seeallGeneration){pthread_mutex_unlock(&seeallLock);continue;}
  seeallError=!valid;
  // Skip usa quantidade recebida, não 100 presumidos. Muitos addons entregam
  // 20/50 por página. Repetição sem novos ids também termina a paginação.
  if(valid){seeallPagina+=raw;if(!raw||!added||seeallN>=cap)seeallEnd=1;}
  seeallThreadAlive=0;
  pthread_mutex_unlock(&seeallLock);
  return NULL;
  }
}

static void seeallFire(void) {
  pthread_t t;
  pthread_mutex_lock(&seeallLock);
  if (seeallThreadAlive || seeallEnd || !seeallBase[0]) {pthread_mutex_unlock(&seeallLock);return;}
  seeallThreadAlive = 1;
  seeallError=0;
  if (pthread_create(&t, NULL, threadSeeAll, NULL) != 0) seeallThreadAlive = 0;
  else pthread_detach(t);
  pthread_mutex_unlock(&seeallLock);
}

void disc_seeall_open(const char *base, const char *kind, const char *catId) {
  disc_seeall_filter(base,kind,catId,"");
}
void disc_seeall_filter(const char *base, const char *kind, const char *catId,const char *genre) {
  if (!base || !kind || !catId) return;
  pthread_mutex_lock(&seeallLock);
  // Mesmo catalogo que ja esta aberto: mantem o que ja foi lido em vez de
  // recomecar do zero (o dono pode ter voltado e entrado de novo).
  if (!strcmp(seeallBase, base) && !strcmp(seeallKind, kind) && !strcmp(seeallCat, catId)
      && !strcmp(seeallGenre,genre?genre:"") && seeallN > 0) {
    pthread_mutex_unlock(&seeallLock);
    return;
  }
  snprintf(seeallBase, sizeof seeallBase, "%s", base);
  snprintf(seeallKind, sizeof seeallKind, "%s", kind);
  snprintf(seeallCat,  sizeof seeallCat,  "%s", catId);
  snprintf(seeallGenre,sizeof seeallGenre,"%s",genre?genre:"");
  seeallN = 0; seeallPagina = 0; seeallEnd = 0;seeallError=0;seeallGeneration++;
  pthread_mutex_unlock(&seeallLock);
  seeallFire();
}

void disc_seeall_more(void) { seeallFire(); }
int  disc_seeall_n(void) { pthread_mutex_lock(&seeallLock);int n=seeallN;pthread_mutex_unlock(&seeallLock);return n; }
int  disc_seeall_loading(void) { pthread_mutex_lock(&seeallLock);int n=seeallThreadAlive;pthread_mutex_unlock(&seeallLock);return n; }
int  disc_seeall_end(void) { pthread_mutex_lock(&seeallLock);int n=seeallEnd;pthread_mutex_unlock(&seeallLock);return n; }
int  disc_seeall_error(void) { pthread_mutex_lock(&seeallLock);int n=seeallError;pthread_mutex_unlock(&seeallLock);return n; }
void disc_seeall_close(void) { /* guarda o que leu; ver desc_vertudo_abrir */ }

int disc_seeall_item(int i, CatItem *dst) {
  int ok = 0;
  pthread_mutex_lock(&seeallLock);
  if (dst && i >= 0 && i < seeallN) { memcpy(dst, &seeallItems[i], sizeof *dst); ok = 1; }
  pthread_mutex_unlock(&seeallLock);
  return ok;
}

void disc_episodes(int indexItem, int season) {
  if (threadEpAlive) { pendingItem = indexItem; pendingTemp = season; return; }
  // A lista agora e UNICA e cobre todas as temporadas, entao ter qualquer
  // episodio deste titulo ja basta — trocar de aba nao pede nada.
  (void)season;
  if (cat_n_episodes(indexItem) > 0) return;
  epItem = indexItem; epTemp = season;
  threadEpAlive = 1;
  if (pthread_create(&threadEp, NULL, fetchEps, NULL) != 0) threadEpAlive = 0;
  else pthread_detach(threadEp);
}

int disc_episodes_loading(int indexItem) {
  return (threadEpAlive && epItem == indexItem) || pendingItem == indexItem;
}

// Chamada por quadro por quem desenha, para o pedido guardado sair assim que o
// fio anterior desocupar.
void disc_episodes_pending(void) {
  int i, t;
  if (threadEpAlive || pendingItem < 0) return;
  i = pendingItem; t = pendingTemp;
  pendingItem = -1;
  disc_episodes(i, t);
}

// --- TITULO SOB DEMANDA -------------------------------------------------------
//
// Abrir um credito da filmografia de um ator, ou um item de "Mais como este",
// exige meta de um titulo que o catalogo do dono NAO tem. Antes esses itens
// ficavam apagados e nao abriam, o que deixava a filmografia decorativa.
//
// O meta vem do Cinemeta, a mesma fonte do resto do catalogo, e o item entra no
// FIM do vetor (cat_acrescentar). O tipo nao e conhecido de antemao — o TMDB
// diz "movie"/"tv" no credito, mas o relacionado do Trakt nao —, entao tenta-se
// filme e, se nao houver, serie. Duas chamadas no pior caso, uma no comum.
static char sobId[24];
static long sobTmdb;          // quando > 0, o id do IMDb ainda precisa ser resolvido
static char sobKind[8];
static int  sobIndex = -1;   // resultado, consumido por desc_titulo_pronto
static int  sobThreadAlive;
static pthread_t sobThread;

static void *fetchTitle(void *arg) {
  char url[200], id[24], *body;
  int found = -1, step;
  (void)arg;
  snprintf(id, sizeof id, "%s", sobId);

  // O credito de um ator chega com o id do TMDB, nao com o do IMDb — o
  // combined_credits nao traz imdb_id. `external_ids` faz a traducao, e e uma
  // chamada so, feita apenas quando o dono abre o credito.
  if (sobTmdb > 0) {
    const char *key = disc_key_tmdb();
    id[0] = 0;
    if (key && key[0]) {
      snprintf(url, sizeof url, "%s/%s/%ld/external_ids?api_key=%s", TMDB,
               strcmp(sobKind, "tv") ? "movie" : "tv", sobTmdb, key);
      body = net_download(url, 15);
      if (body) { js_text(body, NULL, "imdb_id", id, sizeof id); free(body); }
    }
    if (!id[0] || id[0] != 't') {
      printf("[disc] on demand tmdb %ld -> no imdb\n", sobTmdb); fflush(stdout);
      sobIndex = -1; sobThreadAlive = 0; return NULL;
    }
    // Ja temos? Entao e so abrir.
    { int j = cat_index_by_imdb(id);
      if (j >= 0) { sobIndex = j; sobThreadAlive = 0; return NULL; } }
  }

  for (step = 0; step < 2 && found < 0; step++) {
    const char *kind = step ? "series" : "movie";
    snprintf(url, sizeof url, "%s/meta/%s/%s.json", CINEMETA, kind, id);
    body = net_download(url, 20);
    if (!body) continue;
    { const char *m = strstr(body, "\"meta\"");
      CatItem it;
      if (m && ofMeta(m, NULL, kind, &it)) {
        // O id do proprio pedido manda: o Cinemeta as vezes devolve o campo
        // vazio, e sem ele o titulo entraria no catalogo sem chave e nao
        // poderia ser reaberto nem casar com progresso.
        if (!it.imdb[0]) snprintf(it.imdb, sizeof it.imdb, "%s", id);
        found = cat_append(&it);
      } }
    free(body);
  }
  printf("[disc] on demand %s -> index %d\n", id, found); fflush(stdout);
  sobIndex = found;
  sobThreadAlive = 0;
  return NULL;
}

void disc_request_title_tmdb(long tmdbId, const char *kind) {
  if (tmdbId <= 0 || sobThreadAlive) return;
  sobTmdb = tmdbId;
  snprintf(sobKind, sizeof sobKind, "%s", kind ? kind : "movie");
  sobId[0] = 0;
  sobIndex = -1;
  sobThreadAlive = 1;
  if (pthread_create(&sobThread, NULL, fetchTitle, NULL) != 0) sobThreadAlive = 0;
  else pthread_detach(sobThread);
}

void disc_request_title(const char *imdb) {
  char id[24];
  const char *dp;
  if (!imdb || imdb[0] != 't' || sobThreadAlive) return;
  // Corta o sufixo de episodio, se vier: o meta e do TITULO.
  dp = strchr(imdb, ':');
  if (dp) { size_t k = (size_t)(dp - imdb);
            if (k >= sizeof id) k = sizeof id - 1;
            memcpy(id, imdb, k); id[k] = 0; }
  else snprintf(id, sizeof id, "%s", imdb);
  if (cat_index_by_imdb(id) >= 0) return;   // ja temos
  snprintf(sobId, sizeof sobId, "%s", id);
  sobTmdb = 0;
  sobIndex = -1;
  sobThreadAlive = 1;
  if (pthread_create(&sobThread, NULL, fetchTitle, NULL) != 0) sobThreadAlive = 0;
  else pthread_detach(sobThread);
}

int disc_title_ready(void) { int v = sobIndex; sobIndex = -1; return v; }
int disc_title_searching(void) { return sobThreadAlive; }
