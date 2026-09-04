#include "trakt.h"
#include "net.h"
#include "js.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define CINEMETA "https://v3-cinemeta.strem.io"

static char token[128], client[80];
static int  on;

// Estado da ultima escrita iniciada pelo menu. O corpo de um POST nao e prova
// de sucesso: o Trakt tambem devolve corpo em 4xx. O consumidor usa este
// estado para so espelhar a intencao local depois de um HTTP 2xx.
enum { TK_OP_NONE, TK_OP_PENDING, TK_OP_CONFIRMED, TK_OP_FAILURE };
enum { TK_OP_LIST = 1, TK_OP_HISTORY = 2 };
static volatile int listState, historyState;

// Mantem o contrato antigo (so IMDb) sem perder o tipo quando o item ja esta
// no catalogo. O sufixo de episodio continua sendo um fallback para chamadas
// antigas feitas antes de o catalogo estar montado.
extern const char *cat_kind_by_imdb(const char *imdb);
extern void cat_history_set_id(const char *imdb, const char *kind, int watched);

static const char *kind_item(const char *kind, const char *imdb) {
  if (kind && (!strcmp(kind, "series") || !strcmp(kind, "show"))) return "series";
  if (kind && !strcmp(kind, "movie")) return "movie";
  return cat_kind_by_imdb(imdb);
}
static pthread_mutex_t lockList = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lockHistory = PTHREAD_MUTEX_INITIALIZER;

static void stateWrite(volatile int *state, int value) {
  __atomic_store_n(state, value, __ATOMIC_RELEASE);
}

static int stateRead(const volatile int *state) {
  return __atomic_load_n(state, __ATOMIC_ACQUIRE);
}

// API pequena e interna ao port: a declaracao fica no consumidor porque o
// contrato publico historico de trakt_watchlist/trakt_assistido continua void.
int trakt_operation_state(int kind) {
  if (kind == TK_OP_LIST) return stateRead(&listState);
  if (kind == TK_OP_HISTORY) return stateRead(&historyState);
  return TK_OP_NONE;
}

int trakt_active(void) { return on; }

void trakt_forget(void) {
  token[0] = 0;
  client[0] = 0;
  on = 0;
  printf("[trakt] credential forgotten (signed out)\n");
}

int trakt_set(const char *tk, const char *cli) {
  if (!tk || !*tk) return 0;
  snprintf(token, sizeof token, "%s", tk);
  if (cli && *cli) snprintf(client, sizeof client, "%s", cli);
  on = token[0] && client[0];
  printf("[trakt] credential from the account: %s\n",
         on ? "active" : "no application client id (see tools/env.sh)");
  return on;
}


int trakt_headers(const char **header, char *auth, size_t nAuth,
                     char *key, size_t nKey) {
  if (!on) return 0;
  snprintf(auth, nAuth, "Authorization: Bearer %s", token);
  snprintf(key, nKey, "trakt-api-key: %s", client);
  header[0] = auth; header[1] = "trakt-api-version: 2"; header[2] = key; header[3] = NULL;
  return 1;
}

int trakt_load(const char *dirArt) {
  char path[600], line[300], *tab;
  FILE *f;
  snprintf(path, sizeof path, "%s/trakt.txt", dirArt ? dirArt : ".");
  f = fopen(path, "r");
  if (!f) { printf("[trakt] no %s\n", path); return 0; }
  if (fgets(line, sizeof line, f)) {
    char *end;
    tab = strchr(line, '\t');
    if (tab) {
      *tab = 0;
      snprintf(client, sizeof client, "%s", tab + 1);
      end = client + strlen(client);
      while (end > client && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
    }
    snprintf(token, sizeof token, "%s", line);
  }
  fclose(f);
  on = token[0] && client[0];
  printf("[trakt] %s\n", on ? "credential loaded" : "credential incomplete");
  return on;
}

// Arte e sinopse por id do IMDb. O Trakt devolve so identificadores e
// progresso; quem tem imagem e o Cinemeta, que e o mesmo indice que os addons
// usam — entao o que aparece na tela e o que da para pedir fonte.
static int decorate(CatItem *d, const char *kind) {
  char url[300], *body;
  char series[24];
  const char *dp;
  int ok = 0;
  snprintf(series, sizeof series, "%s", d->imdb);
  dp = strchr(series, ':');
  if (dp) *(char *)dp = 0;
  snprintf(url, sizeof url, "%s/meta/%s/%s.json", CINEMETA, kind, series);
  // 8 s e nao 20: sao ate OITO destes em serie (um por item do historico) antes
  // de a primeira fileira da home existir. Medido no Mac: 2,1 s no caso bom;
  // com um item lento eram 20 s de tela sem conteudo nenhum.
  body = net_download(url, 8);
  if (!body) return 0;
  ok = js_text(body, NULL, "poster", d->poster, sizeof d->poster);
  js_text(body, NULL, "background", d->backdrop, sizeof d->backdrop);
  js_text(body, NULL, "logo", d->logo, sizeof d->logo);
  if (!d->title[0]) js_text(body, NULL, "name", d->title, sizeof d->title);
  js_text(body, NULL, "description", d->synopsis, sizeof d->synopsis);
  if (!d->backdrop[0]) snprintf(d->backdrop, sizeof d->backdrop, "%s", d->poster);
  { char r[24] = "", year[24] = "";
    js_text(body, NULL, "runtime", r, sizeof r);
    js_text(body, NULL, "releaseInfo", year, sizeof year);
    { char *tr = strstr(year, "\xe2\x80\x93"); if (tr) *tr = 0; }
    snprintf(d->meta, sizeof d->meta, "%.20s%s%.20s", year,
             (year[0] && r[0]) ? "  \xc2\xb7  " : "", r);
    // Minutos que faltam, para a legenda do card. O Trakt da a porcentagem e o
    // Cinemeta a duracao; o cruzamento das duas e o unico jeito de ter isto
    // sem baixar o arquivo.
    if (d->progress > 0 && d->progress < 100) {
      int total = atoi(r);
      if (total > 0) d->remainingMin = total - (total * d->progress) / 100;
    } else if (d->progress == 0) {
      d->remainingMin = atoi(r);
    } }
  snprintf(d->genre, sizeof d->genre, "%s",
           strcmp(kind, "series") ? "Film" : "TV Show");
  snprintf(d->age_rating, sizeof d->age_rating, "14");
  free(body);
  return ok;
}

// ENFEITAR EM PARALELO.
//
// Sao ate 8 GET ao Cinemeta, um por item do historico, e eram feitos EM SERIE
// dentro do laco de leitura. Medido no Mac: 2,1 s antes de a home ter qualquer
// conteudo de rede — e essa e a PRIMEIRA fileira, a que o dono ve primeiro.
//
// Cada `enfeitar` so escreve no seu proprio CatItem e nao toca estado
// compartilhado, entao a paralelizacao e direta. A ordem do historico e
// preservada porque cada fio escreve na posicao que ja era dele.
#define TK_THREADS 3

typedef struct { CatItem *d; char kind[8]; int ok; } TaskDecorate;
static TaskDecorate *decorateTasks;
static int decorateN, decorateNext;
static pthread_mutex_t decorateLock = PTHREAD_MUTEX_INITIALIZER;

static void *threadDecorate(void *u) {
  (void)u;
  for (;;) {
    int mine;
    pthread_mutex_lock(&decorateLock);
    if (decorateNext >= decorateN) { pthread_mutex_unlock(&decorateLock); return NULL; }
    mine = decorateNext++;
    pthread_mutex_unlock(&decorateLock);
    decorateTasks[mine].ok = decorate(decorateTasks[mine].d, decorateTasks[mine].kind);
  }
}

// A barra de retomada vem de /sync/playback e nao informa se o titulo foi
// marcado como assistido. Consultamos o historico real uma vez no mesmo ciclo
// de descoberta para que a modal nao trate progresso alto como prova de visto.
// Para series, registros com `episode` sao deliberadamente ignorados: ter
// visto um episodio nao significa ter marcado a serie inteira como assistida.
static void loadHistoryReal(const char *const *header) {
  char *body = net_download_com("https://api.trakt.tv/sync/history?limit=100&extended=full", 25, header);
  const char *p;
  if (!body) return;
  p = strchr(body, '[');
  p = p ? p + 1 : NULL;
  while (p && *p) {
    const char *f, *obj;
    char id[24] = "";
    const char *kind = NULL;
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p != '{') break;
    f = js_end(p);
    if (strstr(p, "\"episode\"") && strstr(p, "\"episode\"") < f) {
      p = js_next(f);
      continue;
    }
    obj = strstr(p, "\"movie\"");
    if (obj && obj < f) kind = "movie";
    else {
      obj = strstr(p, "\"show\"");
      if (obj && obj < f) kind = "series";
    }
    if (obj && kind) {
      const char *fo = js_end(strchr(obj, '{'));
      js_text(obj, fo, "imdb", id, sizeof id);
      if (id[0]) cat_history_set_id(id, kind, 1);
    }
    p = js_next(f);
  }
  free(body);
}

int trakt_resume(CatItem *output, int max) {
  const char *header[4];
  char auth[200], key[140];
  char *body;
  const char *p;
  int n = 0;
  if (!on) return 0;
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
  snprintf(key, sizeof key, "trakt-api-key: %s", client);
  header[0] = auth;
  header[1] = "trakt-api-version: 2";
  header[2] = key;
  header[3] = NULL;
  body = net_download_com("https://api.trakt.tv/sync/playback?extended=full", 25, header);
  if (!body) { printf("[trakt] no response\n"); return 0; }
  // O corpo e um array na raiz; js_array procura por chave, entao anda-se a mao.
  p = strchr(body, '[');
  p = p ? p + 1 : NULL;
  while (p && *p && n < max) {
    const char *f;
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p != '{') break;
    f = js_end(p);
    {
      CatItem *d = &output[n];
      const char *ep = strstr(p, "\"episode\"");
      int series = ep && ep < f;
      char imdb[24] = "";
      memset(d, 0, sizeof *d);
      d->progress = (int)js_num(p, f, "progress", 0.0);
      // O bloco "movie"/"show" tem o titulo e os ids; o "episode" traz
      // temporada e numero. Procurar "imdb" na faixa inteira pegaria o do
      // episodio, que os addons tambem aceitam mas nao identifica a obra.
      { const char *block = strstr(p, series ? "\"show\"" : "\"movie\"");
        if (block && block < f) {
          const char *fb = js_end(strchr(block, '{'));
          js_text(block, fb, "title", d->title, sizeof d->title);
          js_text(block, fb, "imdb", imdb, sizeof imdb);
        } }
      if (!imdb[0]) { p = js_next(f); continue; }
      if (series) {
        const char *fe = js_end(strchr(ep, '{'));
        d->season = (int)js_num(ep, fe, "season", 0);
        d->episode  = (int)js_num(ep, fe, "number", 0);
        js_text(ep, fe, "title", d->nameEpisode, sizeof d->nameEpisode);
        snprintf(d->imdb, sizeof d->imdb, "%s:%d:%d", imdb,
                 d->season ? d->season : 1, d->episode ? d->episode : 1);
        snprintf(d->kind, sizeof d->kind, "series");
      } else {
        snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
        snprintf(d->kind, sizeof d->kind, "movie");
      }
      // Enfeitar fica para DEPOIS do laco, em paralelo. Aqui o item ja esta
      // montado: so falta a arte e a sinopse, que vem da rede.
      n++;
    }
    p = js_next(f);
  }
  free(body);
  loadHistoryReal(header);

  // ENFEITAR os n itens em TK_FIOS fios, e so entao compactar: `enfeitar` falha
  // para item que o Cinemeta nao conhece, e antes o `if (enfeitar(...)) n++`
  // simplesmente nao contava — agora o item ja esta na posicao, entao os que
  // falharam saem por compactacao, preservando a ordem do historico.
  if (n > 0) {
    decorateTasks = calloc((size_t)n, sizeof(TaskDecorate));
    if (decorateTasks) {
      pthread_t threads[TK_THREADS];
      int created = 0, q, r, w;
      for (q = 0; q < n; q++) {
        decorateTasks[q].d = &output[q];
        snprintf(decorateTasks[q].kind, sizeof decorateTasks[q].kind, "%s", output[q].kind);
      }
      decorateN = n; decorateNext = 0;
      for (q = 0; q < TK_THREADS; q++)
        if (pthread_create(&threads[created], NULL, threadDecorate, NULL) == 0) created++;
      if (!created) threadDecorate(NULL);      // sem fios: em serie, mesmo resultado
      for (q = 0; q < created; q++) pthread_join(threads[q], NULL);

      for (r = 0, w = 0; r < n; r++)
        if (decorateTasks[r].ok) { if (w != r) output[w] = output[r]; w++; }
      n = w;
      free(decorateTasks); decorateTasks = NULL; decorateN = 0;
    } else {
      // Sem memoria para a fila: em serie, no proprio fio.
      int r, w;
      for (r = 0, w = 0; r < n; r++)
        if (decorate(&output[r], output[r].kind)) { if (w != r) output[w] = output[r]; w++; }
      n = w;
    }
  }

  printf("[trakt] %d em andamento\n", n);
  fflush(stdout);
  return n;
}

// Alguns clientes recebem 401 apenas no feed agregado. O grafo e o
// historico publico dos perfis continuam acessiveis com a mesma credencial.
static char *socialByFollowed(const char *const *header, int max) {
  char *list=net_download_com("https://api.trakt.tv/users/me/following?extended=full",10,header);
  if(!list)return NULL;
  char *out=calloc(1,262144);size_t used=1;int n=0,queried=0;
  if(!out){free(list);return NULL;}out[0]='[';
  const char *p=strchr(list,'[');p=p?p+1:NULL;
  while(p&&*p&&n<max&&queried<8) {
    while(*p&&(unsigned char)*p<=' ')p++;
    if(*p!='{')break;
    const char *f=js_end(p),*u=strstr(p,"\"user\"");
    if(!u||u>=f){p=js_next(f);continue;}
    u=strchr(u,'{');const char *uf=js_end(u);char id[128]="",url[400];
    js_text(u,uf,"slug",id,sizeof id);
    if(!id[0] || strspn(id,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=strlen(id)){p=js_next(f);continue;}
    queried++;
    snprintf(url,sizeof url,"https://api.trakt.tv/users/%s/watching?extended=full",id);
    char *body=net_download_com(url,8,header);int now=body&&strchr(body,'{');
    if(!now){free(body);snprintf(url,sizeof url,"https://api.trakt.tv/users/%s/history?limit=1&extended=full",id);body=net_download_com(url,8,header);}
    const char *b=body?strchr(body,'{'):NULL,*bf=b?js_end(b):NULL;
    if(b&&bf&&bf>b+1) {
      size_t un=(size_t)(uf-u),bn=(size_t)(bf-b-2);
      if(used+un+bn+80<262144){
        int k=snprintf(out+used,262144-used,"%s{\"user\":%.*s,%s%.*s}",n?",":"",(int)un,u,
            now?"\"action\":\"watching\",":"",(int)bn,b+1);
        used+=(size_t)k;n++;
      }
    }
    free(body);p=js_next(f);
  }
  out[used++]=']';out[used]=0;free(list);
  printf("[trakt] social: %d follows queried, %d activities\n",queried,n);
  return out;
}

int trakt_social(CatItem *output, int max) {
  const char *header[4];
  char auth[200], key[140], *body;
  const char *p;
  int n = 0;
  if (!on || max < 1) return 0;
  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) return 0;
  body = net_download_com(
    "https://api.trakt.tv/users/me/friends/activities?extended=full&page=1&limit=12",
    20, header);
  // Contas sem o escopo social novo podem receber 401 no grafo `friends`,
  // embora o token continue valido para historico. `following` e o fallback
  // honesto: ainda sao pessoas escolhidas pelo dono, nunca atividade global.
  if (!body) body = net_download_com(
    "https://api.trakt.tv/users/me/following/activities?extended=full&page=1&limit=12",
    20, header);
  if (!body) body=socialByFollowed(header,max);
  if (!body) { printf("[trakt] social feed unavailable\n"); return 0; }
  p = strchr(body, '['); p = p ? p + 1 : NULL;
  while (p && *p && n < max) {
    const char *f, *bu, *bm, *bs, *be, *fb;
    CatItem *d;
    char imdb[24] = "", person[64] = "", action[32] = "";
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p != '{') break;
    f = js_end(p);
    bu = strstr(p, "\"user\"");
    bm = strstr(p, "\"movie\"");
    bs = strstr(p, "\"show\"");
    be = strstr(p, "\"episode\"");
    if (!bu || bu >= f || ((!bm || bm >= f) && (!bs || bs >= f))) {
      p = js_next(f); continue;
    }
    d = &output[n]; memset(d, 0, sizeof *d);
    fb = js_end(strchr(bu, '{'));
    js_text(bu, fb, "name", person, sizeof person);
    if (!person[0]) js_text(bu, fb, "username", person, sizeof person);
    js_text(bu, fb, "slug", d->socialSlug, sizeof d->socialSlug);
    const char *avatar = strstr(bu, "\"avatar\"");
    if (avatar && avatar < fb) js_text(avatar, fb, "full", d->socialAvatar, sizeof d->socialAvatar);
    snprintf(d->socialName, sizeof d->socialName, "%s", person[0] ? person : "Friend");
    js_text(p, f, "action", action, sizeof action);
    snprintf(d->pais, sizeof d->pais, "%s", person[0] ? person : "Friend");
    snprintf(d->providerName, sizeof d->providerName, "%s",
             !strcmp(action,"watching") ? "watching now" :
             !strcmp(action, "watch") || !strcmp(action, "scrobble") ? "watched" :
             !strcmp(action, "checkin") ? "checked in" :
             !strcmp(action, "rating") ? "rated" : "recent activity");
    snprintf(d->socialAction, sizeof d->socialAction, "%s", d->providerName);
    if (bs && bs < f) {
      fb = js_end(strchr(bs, '{'));
      js_text(bs, fb, "title", d->title, sizeof d->title);
      js_text(bs, fb, "imdb", imdb, sizeof imdb);
      snprintf(d->kind, sizeof d->kind, "series");
      if (be && be < f) {
        const char *fe = js_end(strchr(be, '{'));
        d->season = (int)js_num(be, fe, "season", 0);
        d->episode = (int)js_num(be, fe, "number", 0);
        js_text(be, fe, "title", d->nameEpisode, sizeof d->nameEpisode);
        snprintf(d->directing, sizeof d->directing, "T%dE%d%s%s", d->season,
                 d->episode, d->nameEpisode[0] ? "  \xc2\xb7  " : "",
                 d->nameEpisode);
      }
    } else {
      fb = js_end(strchr(bm, '{'));
      js_text(bm, fb, "title", d->title, sizeof d->title);
      js_text(bm, fb, "imdb", imdb, sizeof imdb);
      snprintf(d->kind, sizeof d->kind, "movie");
      snprintf(d->directing, sizeof d->directing, "Film");
    }
    if (!imdb[0]) { p = js_next(f); continue; }
    snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
    n++;
    p = js_next(f);
  }
  free(body);
  // O feed ja vem ordenado do mais recente. A arte e resolvida em paralelo,
  // com o mesmo limite de tres conexoes usado pelo Continue Assistindo.
  if (n > 0) {
    TaskDecorate *tasksSoc = calloc((size_t)n, sizeof *tasksSoc);
    if (tasksSoc) {
      pthread_t threads[TK_THREADS]; int created = 0, q;
      decorateTasks = tasksSoc; decorateN = n; decorateNext = 0;
      for (q = 0; q < n; q++) {
        tasksSoc[q].d = &output[q];
        snprintf(tasksSoc[q].kind, sizeof tasksSoc[q].kind, "%s", output[q].kind);
      }
      for (q = 0; q < TK_THREADS; q++)
        if (pthread_create(&threads[created], NULL, threadDecorate, NULL) == 0) created++;
      if (!created) threadDecorate(NULL);
      for (q = 0; q < created; q++) pthread_join(threads[q], NULL);
      // Arte indisponivel nao pode apagar uma pessoa real do feed.
      free(tasksSoc); decorateTasks = NULL; decorateN = 0;
    }
  }
  printf("[trakt] %d friend activities\n", n); fflush(stdout);
  return n;
}

static void profileGenre(ProfileData *d, const char *name) {
  int i;
  if (!name || !*name) return;
  static const char *en[]={"drama","science-fiction","comedy","crime","thriller","action","mystery","history","fantasy","horror","adventure","romance","documentary","animation"};
  // Trakt sends lowercase slugs ("science-fiction"); these are the labels the
  // interface shows.
  static const char *label[]={"Drama","Science Fiction","Comedy","Crime","Thriller","Action","Mystery","History","Fantasy","Horror","Adventure","Romance","Documentary","Animation"};
  for (unsigned k=0;k<sizeof en/sizeof *en;k++) if(!strcmp(name,en[k])) {name=label[k];break;}
  for (i=0;i<d->nGenres;i++) if (!strcmp(d->genres[i].name,name)) {
    d->genres[i].count++; return;
  }
  if (d->nGenres < PROFILE_MAX_GENRES) {
    ProfileGenre *g=&d->genres[d->nGenres++];
    snprintf(g->name,sizeof g->name,"%s",name); g->count=1;
  }
}

static void profileGenresJson(ProfileData *d, const char *b, const char *f) {
  const char *g = strstr(b,"\"genres\"");
  if (!g || g >= f || !(g=strchr(g,'[')) || g>=f) return;
  g++;
  while (g < f) {
    char name[40]; size_t n=0;
    while (g<f && *g!='\"' && *g!=']') g++;
    if (g>=f || *g==']') break;
    g++;
    while (g<f && *g!='\"' && n+1<sizeof name) name[n++]=*g++;
    name[n]=0; profileGenre(d,name);
    if (g<f) g++;
  }
}

int trakt_profile(ProfileData *d) {
  const char *header[4]; char auth[200],key[140],url[360],*body;
  time_t now=time(NULL); struct tm tmv=*localtime(&now);
  char start[48]; int daysInMonth;
  ProfileHighlight *ranking;
  int nRanking = 0;
  if (!d || !on) return 0;
  memset(d,0,sizeof *d);
  if (!trakt_headers(header,auth,sizeof auth,key,sizeof key)) return 0;
  static const char *months[]={"January","February","March","April","May","June","July","August","September","October","November","December"};
  snprintf(d->period,sizeof d->period,"%s %d",months[tmv.tm_mon],tmv.tm_year+1900);
  struct tm first=tmv;
  first.tm_mday=1;first.tm_hour=first.tm_min=first.tm_sec=0;first.tm_isdst=-1;
  time_t limit=mktime(&first);struct tm utc;
  gmtime_r(&limit,&utc);
  strftime(start,sizeof start,"%Y-%m-%dT%H%%3A%M%%3A%SZ",&utc);
  // Perfil e avatar. O avatar pode ser WebP no Trakt novo; o renderer so o
  // pede se o firmware aceitar, e a tela continua completa sem ele.
  body=net_download_com("https://api.trakt.tv/users/settings?extended=full",15,header);
  if(body){ const char *u=strstr(body,"\"user\""); const char *fu=u?js_end(strchr(u,'{')):NULL;
    if(u&&fu){js_text(u,fu,"name",d->name,sizeof d->name);js_text(u,fu,"username",d->user,sizeof d->user);
      js_text(u,fu,"full",d->avatar,sizeof d->avatar);} free(body); }
  snprintf(url,sizeof url,
    "https://api.trakt.tv/users/me/history?start_at=%s&extended=full&page=1&limit=100",start);
  body=net_download_com(url,25,header);
  if (!body || !strchr(body, '[')) { free(body); return 0; }
  ranking = calloc(100, sizeof *ranking);
  if (!ranking) { free(body); return 0; }
  d->partial = 1;
  snprintf(d->warning, sizeof d->warning,
           "A slice of the 100 most recent plays this month. Runtimes reported by Trakt.");
  { const char *p=strchr(body,'['); p=p?p+1:NULL;
    while(p&&*p){
      const char *f,*bm,*bs,*be,*obj,*fo; char watched[32]="",imdb[24]="",title[128]="";
      int runtime=0,t=0,e=0,hi=-1;
      while(*p&&(unsigned char)*p<=' ')p++; if(*p!='{')break; f=js_end(p);
      js_text(p,f,"watched_at",watched,sizeof watched);
      bm=strstr(p,"\"movie\""); bs=strstr(p,"\"show\""); be=strstr(p,"\"episode\"");
      obj=(bs&&bs<f)?bs:((bm&&bm<f)?bm:NULL); if(!obj){p=js_next(f);continue;}
      fo=js_end(strchr(obj,'{')); js_text(obj,fo,"title",title,sizeof title); js_text(obj,fo,"imdb",imdb,sizeof imdb);
      runtime=(int)js_num(obj,fo,"runtime",0); profileGenresJson(d,obj,fo);
      if(be&&be<f){const char *fe=js_end(strchr(be,'{'));t=(int)js_num(be,fe,"season",0);e=(int)js_num(be,fe,"number",0);
        {int re=(int)js_num(be,fe,"runtime",0);if(re>0)runtime=re;} d->episodes++;}
      else d->movies++;
      d->plays++; if (runtime > 0) d->minutes += runtime;
      { int y,m,day,h,mi,s;
        if(sscanf(watched,"%d-%d-%dT%d:%d:%d",&y,&m,&day,&h,&mi,&s)==6){
          struct tm wt={0},local;wt.tm_year=y-1900;wt.tm_mon=m-1;wt.tm_mday=day;
          wt.tm_hour=h;wt.tm_min=mi;wt.tm_sec=s;time_t stamp=timegm(&wt);
          localtime_r(&stamp,&local);
          if(local.tm_year==tmv.tm_year&&local.tm_mon==tmv.tm_mon&&local.tm_mday>=1&&local.tm_mday<=31)
            d->activity[local.tm_mday-1]++;
        }
      }
      for(int i=0;i<nRanking;i++)if(imdb[0]&&!strcmp(ranking[i].id,imdb)){hi=i;break;}
      if(hi<0&&imdb[0]&&nRanking<100){hi=nRanking++;ProfileHighlight *h=&ranking[hi];
        snprintf(h->id,sizeof h->id,"%s",imdb);snprintf(h->title,sizeof h->title,"%s",title);
        if(t>0&&e>0)snprintf(h->detail,sizeof h->detail,"T%dE%d",t,e);else snprintf(h->detail,sizeof h->detail,"Film");
        if(imdb[0]){snprintf(h->poster,sizeof h->poster,"https://images.metahub.space/poster/medium/%s/img",imdb);
          snprintf(h->backdrop,sizeof h->backdrop,"https://images.metahub.space/background/medium/%s/img",imdb);}}
      if(hi>=0){ranking[hi].plays++;if(runtime>0)ranking[hi].minutes+=runtime;}
      p=js_next(f);
    }
  }
  free(body);
  for(int i=0;i<nRanking;i++)for(int j=i+1;j<nRanking;j++)
    if(ranking[j].plays>ranking[i].plays){ProfileHighlight x=ranking[i];ranking[i]=ranking[j];ranking[j]=x;}
  d->nHighlights=nRanking<PROFILE_MAX_HIGHLIGHTS?nRanking:PROFILE_MAX_HIGHLIGHTS;
  memcpy(d->highlights,ranking,d->nHighlights*sizeof *ranking);
  free(ranking);
  daysInMonth=31; if(tmv.tm_mon==1) daysInMonth=((tmv.tm_year+1900)%4==0)?29:28;
  else if(tmv.tm_mon==3||tmv.tm_mon==5||tmv.tm_mon==8||tmv.tm_mon==10)daysInMonth=30;
  d->nDays=daysInMonth;
  {struct tm first=tmv;first.tm_mday=1;mktime(&first);d->firstDayWeek=first.tm_wday;}
  for(int i=0;i<d->nDays;i++)if(d->activity[i])d->daysActiveMonth++;
  // Um recorte mensal nao comprova a atividade anual.
  d->daysActiveYear=0;
  for(int i=tmv.tm_mday-1;i>=0&&i<d->nDays;i--){if(!d->activity[i])break;d->streakCurrent++;}
  // Ordena destaques e generos por volume para a leitura visual ser honesta.
  for(int i=0;i<d->nHighlights;i++)for(int j=i+1;j<d->nHighlights;j++)if(d->highlights[j].plays>d->highlights[i].plays){ProfileHighlight x=d->highlights[i];d->highlights[i]=d->highlights[j];d->highlights[j]=x;}
  for(int i=0;i<d->nGenres;i++)for(int j=i+1;j<d->nGenres;j++)if(d->genres[j].count>d->genres[i].count){ProfileGenre x=d->genres[i];d->genres[i]=d->genres[j];d->genres[j]=x;}
  printf("[trakt] profile: %d plays, %d min, %d highlights\n",d->plays,d->minutes,d->nHighlights);fflush(stdout);
  return 1;
}

int trakt_list(const char *which, CatItem *output, int max) {
  const char *header[4];
  char auth[200], key[140], url[160], *body;
  const char *p;
  int n = 0, step;
  if (!on) return 0;
  snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
  snprintf(key, sizeof key, "trakt-api-key: %s", client);
  header[0] = auth; header[1] = "trakt-api-version: 2"; header[2] = key; header[3] = NULL;

  // Filmes e series vem em endpoints separados; misturar as duas listas na
  // mesma fileira e o que o dono ve como "Minha Lista".
  for (step = 0; step < 2 && n < max; step++) {
    const char *kind = step ? "shows" : "movies";
    snprintf(url, sizeof url, "https://api.trakt.tv/sync/%s/%s", which, kind);
    body = net_download_com(url, 25, header);
    if (!body) continue;
    p = strchr(body, '[');
    p = p ? p + 1 : NULL;
    while (p && *p && n < max) {
      const char *f;
      while (*p && (unsigned char)*p <= ' ') p++;
      if (*p != '{') break;
      f = js_end(p);
      {
        CatItem *d = &output[n];
        const char *block = strstr(p, step ? "\"show\"" : "\"movie\"");
        char imdb[24] = "";
        memset(d, 0, sizeof *d);
        if (block && block < f) {
          const char *fb = js_end(strchr(block, '{'));
          js_text(block, fb, "title", d->title, sizeof d->title);
          js_text(block, fb, "imdb", imdb, sizeof imdb);
        }
        if (imdb[0]) {
          snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
          snprintf(d->kind, sizeof d->kind, "%s", step ? "series" : "movie");
          if (!strcmp(which, "watchlist")) d->inList = 1;
          else                            d->inCollection = 1;
          // Arte SEM consultar: as URLs do metahub sao deterministicas pelo id
          // do IMDb (verificado, 200 em todos os testados). Uma consulta por
          // item custava ~0,3 s e limitava a lista a dez; assim ela pode ter o
          // tamanho que o dono tem, e a imagem so e baixada quando aparece na
          // tela — o tex_cache ja faz isso.
          snprintf(d->poster, sizeof d->poster,
                   // "medium" e nao "small", e a diferenca NAO e tamanho: o
                   // metahub serve poster/small como image/WEBP e poster/medium
                   // como image/jpeg. O libSDL2_image DESTA TV carrega libjpeg,
                   // libpng16 e libtiff por dlopen e NAO carrega libwebp — a
                   // unica string de erro de formato dentro dele e "WEBP images
                   // are not supported". (A libwebp.so.7 existe no sistema; o
                   // SDL2_image e que nao foi compilado com ela.)
                   //
                   // Efeito do small: TODO card vindo do Trakt (watchlist,
                   // colecao, a Biblioteca inteira) nunca decodificava — e pior,
                   // o cache nao guarda falha, entao cada quadro tentava de novo
                   // e queimava uma vaga de decode. Era a maior causa de "nao
                   // aparecem todos os posteres".
                   //
                   // Custo: 105 KB contra 31 KB. Barato pela arte existir.
                   "https://images.metahub.space/poster/medium/%s/img", imdb);
          snprintf(d->backdrop, sizeof d->backdrop,
                   "https://images.metahub.space/background/medium/%s/img", imdb);
          snprintf(d->logo, sizeof d->logo,
                   "https://images.metahub.space/logo/medium/%s/img", imdb);
          snprintf(d->genre, sizeof d->genre, "%s",
                   step ? "TV Show" : "Film");
          snprintf(d->age_rating, sizeof d->age_rating, "14");
          n++;
        }
      }
      p = js_next(f);
    }
    free(body);
  }
  printf("[trakt] %s: %d\n", which, n);
  fflush(stdout);
  return n;
}

// --- gravar progresso -------------------------------------------------------

static char brandId[64];
static double brandPos, brandDuration;
static pthread_t threadBrand;
static int threadBrandAlive;

static void *sendBrand(void *u) {
  const char *header[4];
  char auth[200], key[140], body[400], *r;
  char id[24];
  int t = 0, e = 0;
  const char *dp;
  double pct;
  (void)u;
  snprintf(id, sizeof id, "%s", brandId);
  dp = strchr(id, ':');
  if (dp) { sscanf(dp + 1, "%d:%d", &t, &e); *(char *)dp = 0; }
  pct = 100.0 * brandPos / brandDuration;
  if (pct < 0.0) pct = 0.0;
  if (pct > 100.0) pct = 100.0;

  snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
  snprintf(key, sizeof key, "trakt-api-key: %s", client);
  header[0] = auth; header[1] = "trakt-api-version: 2"; header[2] = key; header[3] = NULL;

  // Pause preserva o ponto; stop registra a conclusao. Mantemos o limiar
  // conservador de 90% deste cliente. Pause sozinho nunca conclui o episodio.
  if (t > 0 && e > 0)
    snprintf(body, sizeof body,
             "{\"show\":{\"ids\":{\"imdb\":\"%s\"}},"
             "\"episode\":{\"season\":%d,\"number\":%d},\"progress\":%.2f}",
             id, t, e, pct);
  else
    snprintf(body, sizeof body,
             "{\"movie\":{\"ids\":{\"imdb\":\"%s\"}},\"progress\":%.2f}",
             id, pct);

  r = net_post(pct >= 90 ? "https://api.trakt.tv/scrobble/stop" :
                             "https://api.trakt.tv/scrobble/pause", 20, header, body);
  printf("[trakt] %s %s %.1f%% -> %s\n", pct>=90?"stop":"pause",brandId,pct,r?"ok":"failed");
  fflush(stdout);
  free(r);
  threadBrandAlive = 0;
  return NULL;
}

void trakt_mark(const char *imdb, double posSeg, double durationSeg) {
  if (!on || !imdb || !*imdb || durationSeg <= 1.0 || threadBrandAlive) return;
  snprintf(brandId, sizeof brandId, "%s", imdb);
  brandPos = posSeg; brandDuration = durationSeg;
  threadBrandAlive = 1;
  if (pthread_create(&threadBrand, NULL, sendBrand, NULL) != 0) threadBrandAlive = 0;
  else pthread_detach(threadBrand);
}

// --- WATCHLIST: escrever e ler ------------------------------------------------
//
// O botao "+" da tela de titulo so mexia num vetor local (biblioteca.c), entao
// a lista do dono nos outros aparelhos nunca soube. Agora ele fala com o Trakt,
// que ja e a fonte de verdade do resto do app.
//
// O ESTADO tambem importa: sem ler de volta, o botao mostrava "+" mesmo para um
// titulo que ja estava na lista, e um segundo toque adicionaria de novo.
// ci->naLista ja e preenchido por trakt_lista na descoberta; o que faltava era
// manter esse campo em dia depois de uma escrita nossa.
static char targetList[24];
static char targetListKind[8];
static int  targetAdd, threadListAlive;
static pthread_t threadList;

static void *sendList(void *u) {
  const char *header[4];
  char auth[200], key[140], url[120], body[200], id[24], kindItemBuf[8];
  char *resp;
  int status = 0, confirmed;
  int add;
  (void)u;
  pthread_mutex_lock(&lockList);
  snprintf(id, sizeof id, "%s", targetList);
  snprintf(kindItemBuf, sizeof kindItemBuf, "%s", targetListKind);
  add = targetAdd;
  pthread_mutex_unlock(&lockList);
  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) {
    stateWrite(&listState, TK_OP_FAILURE);
    pthread_mutex_lock(&lockList); threadListAlive = 0; pthread_mutex_unlock(&lockList);
    return NULL;
  }
  // O tipo faz parte da intencao: mandar filme e serie juntos deixa a API
  // resolver o IMDb no escopo errado e torna a confirmacao ambigua.
  if (!strcmp(kindItemBuf, "series"))
    snprintf(body, sizeof body, "{\"shows\":[{\"ids\":{\"imdb\":\"%s\"}}]}", id);
  else
    snprintf(body, sizeof body, "{\"movies\":[{\"ids\":{\"imdb\":\"%s\"}}]}", id);
  snprintf(url, sizeof url, "https://api.trakt.tv/sync/watchlist%s",
           add ? "" : "/remove");
  resp = net_post_st(url, 20, header, body, &status);
  confirmed = status >= 200 && status < 300;
  stateWrite(&listState, confirmed ? TK_OP_CONFIRMED : TK_OP_FAILURE);
  printf("[trakt] watchlist %s %s (%s) -> %s (HTTP %d)\n",
         add ? "add" : "del", id, kindItemBuf,
         confirmed ? "confirmed" : "failed", status);
  fflush(stdout);
  free(resp);
  pthread_mutex_lock(&lockList); threadListAlive = 0; pthread_mutex_unlock(&lockList);
  return NULL;
}

// --- marcar/desmarcar como ASSISTIDO -----------------------------------------
//
// Endpoint DIFERENTE do trakt_marcar: aquele e /scrobble/pause ("parei aqui"),
// que o player usa ao sair. Este e /sync/history ("assisti"), que e o que o
// botao do olho quer dizer.
//
// Nao dava para reaproveitar trakt_marcar: ele guarda `durSeg <= 1.0 -> return`
// para nao mandar scrobble com duracao invalida, e o chamador do olho passava
// exatamente dur=1.0 — a funcao voltava na primeira linha e NADA era enviado. O
// botao parecia funcionar (o espelho local mudava) e o Trakt nunca sabia.
static pthread_t threadHistory;
static int       threadHistoryAlive, historyAdd;
static char      targetHistory[24];
static char      targetHistoryKind[8];

static void *sendHistory(void *u) {
  const char *header[4];
  char auth[200], key[140], url[120], body[200], id[24], kindItemBuf[8];
  char *resp;
  int status = 0, confirmed;
  int mark;
  (void)u;
  pthread_mutex_lock(&lockHistory);
  snprintf(id, sizeof id, "%s", targetHistory);
  snprintf(kindItemBuf, sizeof kindItemBuf, "%s", targetHistoryKind);
  mark = historyAdd;
  pthread_mutex_unlock(&lockHistory);
  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) {
    stateWrite(&historyState, TK_OP_FAILURE);
    pthread_mutex_lock(&lockHistory); threadHistoryAlive = 0; pthread_mutex_unlock(&lockHistory);
    return NULL;
  }
  // O escopo do comando e explicito. Para serie, o alvo e o show, nao um
  // episodio derivado de progresso e nem um segundo vetor de tipo oposto.
  if (!strcmp(kindItemBuf, "series"))
    snprintf(body, sizeof body, "{\"shows\":[{\"ids\":{\"imdb\":\"%s\"}}]}", id);
  else
    snprintf(body, sizeof body, "{\"movies\":[{\"ids\":{\"imdb\":\"%s\"}}]}", id);
  snprintf(url, sizeof url, "https://api.trakt.tv/sync/history%s",
           mark ? "" : "/remove");
  resp = net_post_st(url, 20, header, body, &status);
  confirmed = status >= 200 && status < 300;
  stateWrite(&historyState, confirmed ? TK_OP_CONFIRMED : TK_OP_FAILURE);
  if (confirmed) cat_history_set_id(id, kindItemBuf, mark);
  printf("[trakt] history %s %s (%s) -> %s (HTTP %d)\n",
         mark ? "add" : "del", id, kindItemBuf,
         confirmed ? "confirmed" : "failed", status);
  fflush(stdout);
  free(resp);
  pthread_mutex_lock(&lockHistory); threadHistoryAlive = 0; pthread_mutex_unlock(&lockHistory);
  return NULL;
}

int trakt_watched_kind(const char *imdb, const char *kind, int mark) {
  const char *dp;
  if (!on || !imdb || imdb[0] != 't') {
    stateWrite(&historyState, TK_OP_FAILURE);
    return 0;
  }
  pthread_mutex_lock(&lockHistory);
  if (threadHistoryAlive) {
    pthread_mutex_unlock(&lockHistory);
    return 0;
  }
  // "tt123:2:5" (episodio) vira "tt123": o historico e do TITULO.
  dp = strchr(imdb, ':');
  { size_t k = dp ? (size_t)(dp - imdb) : strlen(imdb);
    if (k >= sizeof targetHistory) k = sizeof targetHistory - 1;
    memcpy(targetHistory, imdb, k); targetHistory[k] = 0; }
  snprintf(targetHistoryKind, sizeof targetHistoryKind, "%s", kind_item(kind, imdb));
  historyAdd = mark;
  stateWrite(&historyState, TK_OP_PENDING);
  threadHistoryAlive = 1;
  pthread_mutex_unlock(&lockHistory);
  if (pthread_create(&threadHistory, NULL, sendHistory, NULL) != 0) {
    pthread_mutex_lock(&lockHistory); threadHistoryAlive = 0; pthread_mutex_unlock(&lockHistory);
    stateWrite(&historyState, TK_OP_FAILURE);
    return 0;
  }
  else pthread_detach(threadHistory);
  return 1;
}

int trakt_watchlist_kind(const char *imdb, const char *kind, int add) {
  const char *dp;
  if (!on || !imdb || imdb[0] != 't') {
    stateWrite(&listState, TK_OP_FAILURE);
    return 0;
  }
  pthread_mutex_lock(&lockList);
  if (threadListAlive) {
    pthread_mutex_unlock(&lockList);
    return 0;
  }
  dp = strchr(imdb, ':');
  { size_t k = dp ? (size_t)(dp - imdb) : strlen(imdb);
    if (k >= sizeof targetList) k = sizeof targetList - 1;
    memcpy(targetList, imdb, k); targetList[k] = 0; }
  snprintf(targetListKind, sizeof targetListKind, "%s", kind_item(kind, imdb));
  targetAdd = add;
  stateWrite(&listState, TK_OP_PENDING);
  threadListAlive = 1;
  pthread_mutex_unlock(&lockList);
  if (pthread_create(&threadList, NULL, sendList, NULL) != 0) {
    pthread_mutex_lock(&lockList); threadListAlive = 0; pthread_mutex_unlock(&lockList);
    stateWrite(&listState, TK_OP_FAILURE);
    return 0;
  }
  else pthread_detach(threadList);
  return 1;
}

void trakt_watched(const char *imdb, int mark) {
  (void)trakt_watched_kind(imdb, cat_kind_by_imdb(imdb), mark);
}

void trakt_watchlist(const char *imdb, int add) {
  (void)trakt_watchlist_kind(imdb, cat_kind_by_imdb(imdb), add);
}
