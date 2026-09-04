#include "addons.h"
#include "streams.h"
#include "net.h"
#include "js.h"
#include "mark.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>

#define ADD_MAX 12

// `fonte` marca quem realmente entrega stream. Descoberto pelo manifesto: o
// Xperience declara resources catalog/meta/subtitles e NENHUM stream, entao
// respondia {"streams":[]} para tudo. Consultar quem nao fornece e um
// round-trip jogado fora em CADA abertura de titulo.
static struct { char name[64]; char base[600]; int source, catalog, subtitle; } addon[ADD_MAX];
static int nAddon;
static _Atomic AddState state = ADD_STOPPED;
static pthread_t thread;
static char targetId[64], targetKind[16];
static int threadAlive;
static Stream *result;
static int nResult;
static char pendingId[64], pendingKind[16];

// --- leitura do arquivo de configuracao -------------------------------------

int addons_load(const char *dirArt) {
  char path[600], line[900];
  FILE *f;
  snprintf(path, sizeof path, "%s/addons.txt", dirArt ? dirArt : ".");
  f = fopen(path, "r");
  if (!f) { printf("[addons] no %s\n", path); return 0; }
  nAddon = 0;
  while (nAddon < ADD_MAX && fgets(line, sizeof line, f)) {
    char *tab = strchr(line, '\t');
    char *end;
    size_t n;
    // TAB e nao "|" como separador: nome de addon contem "|" de verdade
    // ("AIOStreams | ElfHosted") e partir no primeiro pipe corrompia a URL.
    if (!tab) continue;
    *tab = 0;
    end = tab + 1 + strlen(tab + 1);
    while (end > tab + 1 && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
    if (line[0] == '#' || !tab[1]) continue;
    // Terceira coluna (opcional): 1 = fornece stream. Ausente vale 1, para
    // arquivo antigo continuar funcionando.
    addon[nAddon].source = 1;
    addon[nAddon].catalog = 1;
    addon[nAddon].subtitle = 0;
    { char *tab2 = strchr(tab + 1, '\t');
      if (tab2) {
        char *tab3;
        *tab2 = 0;
        tab3 = strchr(tab2 + 1, '\t');
        if (tab3) {
          char *tab4 = strchr(tab3 + 1, '\t');
          *tab3 = 0;
          if (tab4) { *tab4 = 0; addon[nAddon].subtitle = atoi(tab4 + 1); }
          addon[nAddon].catalog = atoi(tab3 + 1);
        }
        addon[nAddon].source = atoi(tab2 + 1);
      } }
    snprintf(addon[nAddon].name, sizeof addon[nAddon].name, "%s", line);
    snprintf(addon[nAddon].base, sizeof addon[nAddon].base, "%s", tab + 1);
    // A URL guardada aponta para o manifesto; a base e ela sem esse sufixo.
    n = strlen(addon[nAddon].base);
    if (n > 14 && !strcmp(addon[nAddon].base + n - 14, "/manifest.json"))
      addon[nAddon].base[n - 14] = 0;
    else while (n && addon[nAddon].base[n - 1] == '/') addon[nAddon].base[--n] = 0;
    nAddon++;
  }
  fclose(f);
  { int f = 0, k;
    for (k = 0; k < nAddon; k++) f += addon[k].source;
    printf("[addons] %d configured, %d provide streams\n", nAddon, f); }
  return nAddon;
}

int addons_set_list(const AddonRemote *new, int n) {
  int i, accepted = 0;
  if (!new || n <= 0) {
    // Vazio nao substitui. Ver o comentario no cabecalho: uma resposta vazia
    // nao se distingue de uma delecao, e a diferenca entre as duas e a pessoa
    // ficar ou nao sem nenhuma fonte.
    printf("[addons] account list came back empty; keeping the local one (%d)\n", nAddon);
    return 0;
  }
  for (i = 0; i < n && accepted < ADD_MAX; i++) {
    size_t k;
    if (!new[i].url[0] || !new[i].active) continue;
    snprintf(addon[accepted].name, sizeof addon[accepted].name, "%s",
             new[i].name[0] ? new[i].name : "Addon");
    snprintf(addon[accepted].base, sizeof addon[accepted].base, "%s", new[i].url);
    k = strlen(addon[accepted].base);
    if (k > 14 && !strcmp(addon[accepted].base + k - 14, "/manifest.json"))
      addon[accepted].base[k - 14] = 0;
    else while (k && addon[accepted].base[k - 1] == '/') addon[accepted].base[--k] = 0;
    // A conta nao diz o que cada addon fornece; o manifesto e que diria, e
    // consultar todos no arranque custaria uma viagem por addon. Assumir que
    // fornece tudo faz no maximo uma consulta vazia a mais por titulo — o
    // contrario (assumir que nao fornece) esconderia fontes de verdade.
    addon[accepted].source = 1;
    addon[accepted].catalog = 1;
    addon[accepted].subtitle = 0;
    accepted++;
  }
  if (accepted == 0) {
    printf("[addons] account had only disabled addons; keeping the local list\n");
    return 0;
  }
  nAddon = accepted;
  printf("[addons] %d from the account\n", nAddon);
  return nAddon;
}

int addons_export(AddonRemote *output, int max) {
  int i, k = 0;
  for (i = 0; i < nAddon && k < max; i++) {
    snprintf(output[k].name, sizeof output[k].name, "%s", addon[i].name);
    snprintf(output[k].url, sizeof output[k].url, "%s", addon[i].base);
    output[k].active = 1;
    k++;
  }
  return k;
}

void addons_forget(void) {
  memset(addon, 0, sizeof addon);
  nAddon = 0;
  printf("[addons] list forgotten (signed out)\n");
}

int addons_n(void) { return nAddon; }

const char *addons_base(int i) {
  return (i >= 0 && i < nAddon) ? addon[i].base : "";
}

// Quarta coluna de addons.txt. Como a de stream, ausente vale 1 — arquivo
// antigo continua funcionando, so faz uma consulta a mais que pode dar vazio.
int addons_has_catalog(int i) {
  return (i >= 0 && i < nAddon) ? addon[i].catalog : 0;
}
AddState addons_state(void) {
  AddState e = atomic_load(&state);
  // Publica no fio da UI: nenhum desenho observa uma lista parcialmente escrita.
  if (threadAlive && e != ADD_SEARCHING) {
    pthread_join(thread, NULL);
    threadAlive = 0;
    if (!pendingId[0]) stream_set_list(result, nResult);
    free(result); result = NULL; nResult = 0;
    if (pendingId[0]) {
      char id[64], kind[16];
      snprintf(id, sizeof id, "%s", pendingId);
      snprintf(kind, sizeof kind, "%s", pendingKind);
      pendingId[0] = 0;
      addons_fetch(id, kind);
      return ADD_SEARCHING;
    }
  }
  return e;
}

// --- leitura tolerante de JSON ----------------------------------------------
// Um analisador completo nao se paga aqui: o formato e conhecido e raso, e o
// que importa e nunca travar com campo faltando. Cada funcao devolve o que
// achou ou nada, e quem chama decide.

static const char *skipSpace(const char *p) {
  while (*p && (unsigned char)*p <= ' ') p++;
  return p;
}

// Copia o valor textual de "chave" dentro do objeto que comeca em `obj`,
// respeitando escapes. Devolve 1 se achou.
static int fieldText(const char *obj, const char *endObj, const char *key,
                      char *dst, size_t size) {
  char search[48];
  const char *p;
  size_t k = 0;
  snprintf(search, sizeof search, "\"%s\"", key);
  p = strstr(obj, search);
  if (!p || p >= endObj) return 0;
  p = skipSpace(p + strlen(search));
  if (*p != ':') return 0;
  p = skipSpace(p + 1);
  if (*p != '"') return 0;
  p++;
  while (*p && *p != '"' && k + 1 < size) {
    if (*p == '\\' && p[1]) {
      p++;
      // \u..... vira "?" de proposito: os nomes vem cheios de emoji e o texto
      // e so para exibicao. Decodificar UTF-16 aqui seria trabalho sem retorno.
      if (*p == 'u') { p += 5; dst[k++] = ' '; continue; }
      if (*p == 'n' || *p == 't' || *p == 'r') { p++; dst[k++] = ' '; continue; }
    }
    dst[k++] = *p++;
  }
  dst[k] = 0;
  return k > 0;
}

// Acha o fim do objeto JSON que comeca em `p` (que aponta para '{').
static const char *endObject(const char *p) {
  int depth = 0, text = 0;
  for (; *p; p++) {
    if (text) { if (*p == '\\') p++; else if (*p == '"') text = 0; continue; }
    if (*p == '"') text = 1;
    else if (*p == '{') depth++;
    else if (*p == '}' && --depth == 0) return p + 1;
  }
  return p;
}

// --- legendas ---------------------------------------------------------------

static Subtitle subs[SUB_MAX];
static int nSubs;
static pthread_t threadSub;
static int threadSubAlive, threadSubCreated, subStop;
static char subId[64], subKind[16];
static unsigned subGeneration;
static pthread_mutex_t subLock = PTHREAD_MUTEX_INITIALIZER;

int addons_n_subtitles(void) {
  int n;
  pthread_mutex_lock(&subLock); n = nSubs; pthread_mutex_unlock(&subLock);
  return n;
}
const Subtitle *addons_subtitle(int i) {
  const Subtitle *r = NULL;
  pthread_mutex_lock(&subLock);
  if (i >= 0 && i < nSubs) r = &subs[i];
  pthread_mutex_unlock(&subLock);
  return r;
}

// Idiomas que interessam a esta casa, na ordem em que devem aparecer. Trazer as
// 70 que o OpenSubtitles devolve seria uma lista impossivel de percorrer com
// controle remoto.
static const char *LANGUAGES_PT[] = {
  "pob", "pt-br", "pt_br", "ptb", "br", "por", "pt"
};
static const char *LANGUAGES_EN[] = {
  "eng", "en", "en-us", "en_us", "en-gb", "en_gb"
};

// 0 = portugues, 1 = ingles. O usuario pediu explicitamente estes dois grupos;
// espanhol nao entra mais como fallback silencioso. Variantes regionais sao
// normalizadas aqui, antes de ocupar uma das doze linhas da TV.
static int groupLanguage(const char *l) {
  size_t i;
  for (i = 0; i < sizeof LANGUAGES_PT / sizeof *LANGUAGES_PT; i++)
    if (!strcasecmp(l, LANGUAGES_PT[i])) return 0;
  for (i = 0; i < sizeof LANGUAGES_EN / sizeof *LANGUAGES_EN; i++)
    if (!strcasecmp(l, LANGUAGES_EN[i])) return 1;
  return -1;
}

static const char *nameLanguage(const char *c) {
  if (!strcasecmp(c, "pob") || !strcasecmp(c, "pt-br") ||
      !strcasecmp(c, "pt_br") || !strcasecmp(c, "ptb") || !strcasecmp(c, "br"))
    return "Portuguese (BR)";
  if (!strcasecmp(c, "por") || !strcasecmp(c, "pt")) return "Portuguese";
  if (groupLanguage(c) == 1) return "English";
  return c;
}

static int requestChanged(unsigned generation) {
  int changed;
  pthread_mutex_lock(&subLock);
  changed = subStop || generation != subGeneration;
  pthread_mutex_unlock(&subLock);
  return changed;
}

static void episodeRequest(const char *id, int *season, int *episode) {
  const char *p = strchr(id, ':');
  *season = *episode = 0;
  if (p) sscanf(p + 1, "%d:%d", season, episode);
}

static int episodeCorrect(const char *obj, const char *end, int season, int episode) {
  int t, e;
  if (season <= 0 || episode <= 0) return 1;
  t = (int)js_num(obj, end, "season", -1);
  e = (int)js_num(obj, end, "episode", -1);
  // Alguns addons antigos nao devolvem os campos. Quando devolvem, eles sao
  // uma garantia: nunca mostre T2E3 numa busca por T2E4.
  if ((t >= 0 && t != season) || (e >= 0 && e != episode)) return 0;
  if (t >= 0 || e >= 0) return 1;
  // Alguns addons omitem season/episode mas devolvem o episodio no nome do
  // arquivo. Antes aceitavamos S02E03 numa busca por T2E4 e depois fabricavamos
  // o rotulo T2E4 com base no pedido, escondendo o erro. Se o nome traz uma
  // identidade verificavel, ela precisa casar; nome sem marcador segue aceito.
  { char name[160] = "", bottom[160]; size_t i;
    if (!js_text(obj, end, "subtitleFileName", name, sizeof name))
      js_text(obj, end, "movieReleaseName", name, sizeof name);
    for (i = 0; name[i] && i + 1 < sizeof bottom; i++)
      bottom[i] = (char)tolower((unsigned char)name[i]);
    bottom[i] = 0;
    for (i = 0; bottom[i]; i++) {
      int nt = -1, ne = -1;
      if (sscanf(bottom + i, "s%2de%2d", &nt, &ne) == 2 ||
          sscanf(bottom + i, "%2dx%2d", &nt, &ne) == 2)
        return nt == season && ne == episode;
    }
  }
  return 1;
}

static void *fetchSubtitles(void *u) {
  (void)u;
  for (;;) {
    Subtitle found[SUB_MAX] = {{0}};
    char id[64], kind[16];
    unsigned generation;
    int nFound = 0, season, episode, i;

    pthread_mutex_lock(&subLock);
    if (subStop) { threadSubAlive = 0; pthread_mutex_unlock(&subLock); return NULL; }
    snprintf(id, sizeof id, "%s", subId);
    snprintf(kind, sizeof kind, "%s", subKind);
    generation = subGeneration;
    pthread_mutex_unlock(&subLock);
    episodeRequest(id, &season, &episode);

    for (i = 0; i < nAddon && nFound < SUB_MAX; i++) {
      char url[900], *body;
      const char *p;
    // Addon que nao declara legenda nao e consultado: o AIOStreams responderia
    // vazio e o Xperience tambem, dois round-trips sem retorno.
      if (!addon[i].subtitle) continue;
      snprintf(url, sizeof url, "%s/subtitles/%s/%s.json",
               addon[i].base, kind, id);
      body = net_download(url, 25);
      if (requestChanged(generation)) { free(body); break; }
      if (!body) continue;
      p = js_array(body, NULL, "subtitles");
      {
        int group;
        // Uma passada por grupo garante ordem PT -> EN e evita que doze
        // resultados portugueses consumam a lista inteira antes do ingles.
        // Seis por idioma e um limite deliberado para navegacao por D-pad.
        for (group = 0; group < 2; group++) {
          const char *q = p;
          int inGroup = 0, j;
          for (j = 0; j < nFound; j++)
            if (groupLanguage(found[j].language) == group) inGroup++;
          while (q && nFound < SUB_MAX && inGroup < SUB_MAX / 2) {
            const char *f = js_end(q);
            char l[16] = "", name[120] = "";
            Subtitle *d = &found[nFound];
            if (episodeCorrect(q, f, season, episode) &&
                js_text(q, f, "lang", l, sizeof l) && groupLanguage(l) == group &&
                js_text(q, f, "url", d->url, sizeof d->url)) {
              js_text(q, f, "subtitleFileName", name, sizeof name);
              if (!name[0]) js_text(q, f, "movieReleaseName", name, sizeof name);
              snprintf(d->language, sizeof d->language, "%s", l);
              if (season > 0 && episode > 0)
                snprintf(d->label, sizeof d->label, "T%dE%d  \xc2\xb7  %s%s%.22s",
                         season, episode, nameLanguage(l), name[0] ? "  \xc2\xb7  " : "", name);
              else
                snprintf(d->label, sizeof d->label, "%s%s%.36s",
                         nameLanguage(l), name[0] ? "  \xc2\xb7  " : "", name);
              nFound++; inGroup++;
            }
            q = js_next(f);
          }
        }
      }
      free(body);
    }

    pthread_mutex_lock(&subLock);
    if (subStop) { threadSubAlive = 0; pthread_mutex_unlock(&subLock); return NULL; }
    if (generation != subGeneration) { pthread_mutex_unlock(&subLock); continue; }
    memcpy(subs, found, sizeof found);
    nSubs = nFound;
    threadSubAlive = 0;
    pthread_mutex_unlock(&subLock);
    printf("[subtitles] %s: %d\n", id, nFound);
    fflush(stdout);
    return NULL;
  }
}

void addons_fetch_subtitles(const char *imdb, const char *kind) {
  int series, merge = 0;
  char id[64], tp[16];
  if (!nAddon || !imdb || !*imdb) return;
  series = kind && !strcmp(kind, "series");
  if (series && !strchr(imdb, ':'))
    snprintf(id, sizeof id, "%s:1:1", imdb);
  else
    snprintf(id, sizeof id, "%s", imdb);
  snprintf(tp, sizeof tp, "%s", series ? "series" : "movie");

  pthread_mutex_lock(&subLock);
  if (!strcmp(id, subId) && !strcmp(tp, subKind) && (threadSubAlive || nSubs > 0)) {
    pthread_mutex_unlock(&subLock);
    return;
  }
  snprintf(subId, sizeof subId, "%s", id);
  snprintf(subKind, sizeof subKind, "%s", tp);
  subGeneration++;
  nSubs = 0;
  if (threadSubAlive) { pthread_mutex_unlock(&subLock); return; }
  merge = threadSubCreated;
  pthread_mutex_unlock(&subLock);

  if (merge) pthread_join(threadSub, NULL);
  pthread_mutex_lock(&subLock);
  threadSubCreated = 0;
  subStop = 0;
  threadSubAlive = 1;
  if (pthread_create(&threadSub, NULL, fetchSubtitles, NULL) != 0) threadSubAlive = 0;
  else threadSubCreated = 1;
  pthread_mutex_unlock(&subLock);
}

// UM FIO POR ADDON DE FONTE.
//
// MEDIDO NA TV, na sessao do dono: 16,5 s entre abrir o titulo e ter uma fonte
// escolhida (detail_abrir 51098 -> fonte escolhida 67659). Eram consultas em
// SERIE com 25 s de timeout cada; um addon lento atrasa todos os outros, e a
// tela fica com "buscando" o tempo todo.
//
// Os addons sao independentes e `extrair` so escreve no balde que recebe, entao
// cada um le no proprio. A ORDEM e preservada na juncao: ela decide qual fonte
// o automatico ve primeiro, e trocar a ordem trocaria a fonte escolhida.
#define ADD_THREADS 4

typedef struct {
  int    idx;                 // qual addon
  Stream *found;
  int    n;
} BucketSource;

static BucketSource *buckets;
static int nBuckets, nextBucket;
static pthread_mutex_t addLock = PTHREAD_MUTEX_INITIALIZER;

static void *threadSources(void *u) {
  (void)u;
  for (;;) {
    int mine, i;
    char url[900], *body;
    pthread_mutex_lock(&addLock);
    if (nextBucket >= nBuckets) { pthread_mutex_unlock(&addLock); return NULL; }
    mine = nextBucket++;
    pthread_mutex_unlock(&addLock);
    i = buckets[mine].idx;
    snprintf(url, sizeof url, "%s/stream/%s/%s.json",
             addon[i].base, targetKind, targetId);
    // 12 s e nao 25: com os addons em paralelo o timeout deixa de ser somado,
    // mas continua sendo o tempo que o dono espera pelo mais lento.
    body = net_download(url, 12);
    if (!body) { printf("[addons] %s: no response\n", addon[i].name); continue; }
    buckets[mine].n = stream_parse(body, addon[i].name, &buckets[mine].found);
    printf("[addons] %s: %d sources (%u bytes)\n",
           addon[i].name, buckets[mine].n, (unsigned)strlen(body));
    free(body);
  }
}

static void *fetch(void *u) {
  Stream *found = NULL;
  int n = 0, i;
  (void)u;
  mark("addons: query start");

  nBuckets = 0; nextBucket = 0;
  buckets = calloc((size_t)(nAddon > 0 ? nAddon : 1), sizeof(BucketSource));
  if (buckets)
    for (i = 0; i < nAddon; i++)
      if (addon[i].source) buckets[nBuckets++].idx = i;

  if (buckets && nBuckets > 0) {
    pthread_t threads[ADD_THREADS];
    int created = 0, q;
    for (q = 0; q < ADD_THREADS && q < nBuckets; q++)
      if (pthread_create(&threads[created], NULL, threadSources, NULL) == 0) created++;
    if (!created) threadSources(NULL);        // sem fios: em serie, mesmo resultado
    for (q = 0; q < created; q++) pthread_join(threads[q], NULL);
    // Junta NA ORDEM DOS ADDONS, que e a ordem em que o dono os instalou.
    for (q = 0; q < nBuckets; q++) {
      int k = buckets[q].n;
      if (k > 0) {
        Stream *tmp = realloc(found, sizeof(Stream) * (size_t)(n + k));
        if (tmp) { found = tmp;
          memcpy(found + n, buckets[q].found, sizeof(Stream) * (size_t)k);
          n += k;
        } else printf("[addons] not enough memory for %d sources\n", k);
      }
      free(buckets[q].found);
    }
  }
  free(buckets); buckets = NULL; nBuckets = 0;

  mark(n ? "addons: sources received" : "addons: no sources");
  result = found; nResult = n;
  printf("[addons] total %d\n", n);
  fflush(stdout);
  atomic_store(&state, n ? ADD_READY : ADD_EMPTY);
  return NULL;
}

void addons_fetch(const char *imdb, const char *kind) {
  int series;
  if (!imdb || !*imdb) return;
  if (!nAddon) { stream_set_list(NULL, 0); state = ADD_EMPTY; return; }
  if (threadAlive) {
    if (strcmp(imdb, targetId) || strcmp(kind ? kind : "movie", targetKind)) {
      snprintf(pendingId, sizeof pendingId, "%s", imdb);
      snprintf(pendingKind, sizeof pendingKind, "%s", kind ? kind : "movie");
    }
    return;
  }
  stream_set_list(NULL, 0);
  series = kind && !strcmp(kind, "series");
  // Serie SEM episodio devolve lista vazia, com HTTP 200 e sem erro nenhum
  // (medido: 14 bytes de resposta). O identificador tem de ser
  // "tt1234567:temporada:episodio". Como o catalogo ainda nao traz lista de
  // episodios, assume T1E1 — e o mesmo lugar onde o episodio real entra quando
  // houver.
  if (series && !strchr(imdb, ':'))
    snprintf(targetId, sizeof targetId, "%s:1:1", imdb);
  else
    snprintf(targetId, sizeof targetId, "%s", imdb);
  snprintf(targetKind, sizeof targetKind, "%s", kind && *kind ? kind : "movie");
  state = ADD_SEARCHING;
  threadAlive = 1;
  if (pthread_create(&thread, NULL, fetch, NULL) != 0) { threadAlive = 0; state = ADD_STOPPED; }
}

void addons_shutdown(void) {
  int mergeSub;
  if (threadAlive) pthread_join(thread, NULL);
  threadAlive = 0;
  pthread_mutex_lock(&subLock);
  subStop = 1; subGeneration++; mergeSub = threadSubCreated;
  pthread_mutex_unlock(&subLock);
  if (mergeSub) pthread_join(threadSub, NULL);
  pthread_mutex_lock(&subLock);
  threadSubCreated = threadSubAlive = 0; nSubs = 0;
  pthread_mutex_unlock(&subLock);
  free(result); result = NULL; nResult = 0;
  state = ADD_STOPPED;
}
