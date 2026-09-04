#include "person.h"
#include "discover.h"
#include "net.h"
#include "js.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TMDB "https://api.themoviedb.org/3"

// Nomeada porque a ordenacao por popularidade precisa de uma variavel
// temporaria do mesmo tipo, e struct anonima nao permite declarar outra.
typedef struct {
  char t[120], p[80], y[8], po[160], im[16];
  // O credito de combined_credits NAO tem imdb_id — esse campo so existe no
  // endpoint detalhado do titulo. Sem guardar o id do TMDB e o tipo, nao havia
  // como traduzir o credito em nada que o Cinemeta entenda, e o OK nao fazia
  // coisa nenhuma (o `im` ficava sempre vazio).
  long tmdb;
  char kind[8];          // "movie" ou "tv"
  double pop;
} Credit;
static Credit cred[PES_MAX];
static int nCred;

static char name[80], photo[200], bio[1400], area[24];
static long idRequest, idInProgress;
static char nameSeed[80], photoSeed[200];
static int  ready, threadAlive;
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// known_for_department vem em ingles do TMDB mesmo com language=pt-BR.
static const char *translatesArea(const char *s) {
  if (!strcmp(s, "Acting"))    return "Atua\xc3\xa7\xc3\xa3" "o";
  if (!strcmp(s, "Directing")) return "Dire\xc3\xa7\xc3\xa3" "o";
  if (!strcmp(s, "Writing"))   return "Roteiro";
  if (!strcmp(s, "Production"))return "Produ\xc3\xa7\xc3\xa3" "o";
  if (!strcmp(s, "Sound"))     return "Som";
  if (!strcmp(s, "Camera"))    return "Fotografia";
  return s;
}

static void *fetch(void *arg) {
  char url[400], *body;
  long id;
  const char *key = disc_key_tmdb();
  (void)arg;

  pthread_mutex_lock(&lock);
  id = idInProgress;
  pthread_mutex_unlock(&lock);
  if (!key || !key[0]) {
    pthread_mutex_lock(&lock); threadAlive = 0; pthread_mutex_unlock(&lock);
    return NULL;
  }

  // O MESMO pedido do web (castDetailScreen.js:204): a ficha e os creditos
  // combinados numa chamada so. Separado seriam dois pedidos para desenhar uma
  // tela — e o segundo so serve se o primeiro deu certo.
  snprintf(url, sizeof url,
           "%s/person/%ld?api_key=%s&language=pt-BR"
           "&append_to_response=combined_credits", TMDB, id, key);
  body = net_download(url, 20);
  if (!body) {
    pthread_mutex_lock(&lock); threadAlive = 0; pthread_mutex_unlock(&lock);
    return NULL;
  }

  {
    char n[80] = "", f[160] = "", b[1400] = "", a[24] = "";
    Credit ach[PES_MAX];
    int k = 0;

    js_text(body, NULL, "name", n, sizeof n);
    js_text(body, NULL, "biography", b, sizeof b);
    js_text(body, NULL, "known_for_department", a, sizeof a);
    { char path[128] = "";
      if (js_text(body, NULL, "profile_path", path, sizeof path) &&
          path[0] == '/')
        snprintf(f, sizeof f, "https://image.tmdb.org/t/p/w342%s", path); }

    // `combined_credits.cast` — o array vem depois da chave, e js_array acha o
    // primeiro "cast" do documento. O objeto raiz nao tem outro "cast", entao
    // e este.
    { const char *p = js_array(body, NULL, "cast");
      while (p && k < PES_MAX) {
        const char *end = js_end(p);
        char date[16] = "", path[128] = "";
        ach[k].t[0] = ach[k].p[0] = ach[k].y[0] = ach[k].po[0] = ach[k].im[0] = 0;
        // Filme tem "title"/"release_date"; serie tem "name"/"first_air_date".
        if (!js_text(p, end, "title", ach[k].t, sizeof ach[k].t))
          js_text(p, end, "name", ach[k].t, sizeof ach[k].t);
        js_text(p, end, "character", ach[k].p, sizeof ach[k].p);
        if (!js_text(p, end, "release_date", date, sizeof date))
          js_text(p, end, "first_air_date", date, sizeof date);
        if (strlen(date) >= 4) { memcpy(ach[k].y, date, 4); ach[k].y[4] = 0; }
        if (js_text(p, end, "poster_path", path, sizeof path) &&
            path[0] == '/')
          snprintf(ach[k].po, sizeof ach[k].po,
                   "https://image.tmdb.org/t/p/w342%s", path);
        js_text(p, end, "imdb_id", ach[k].im, sizeof ach[k].im);
        ach[k].tmdb = (long)js_num(p, end, "id", 0.0);
        ach[k].kind[0] = 0;
        js_text(p, end, "media_type", ach[k].kind, sizeof ach[k].kind);
        ach[k].pop = js_num(p, end, "popularity", 0.0);
        if (ach[k].t[0]) k++;
        p = js_next(end);
      } }

    // Por POPULARIDADE, como o web (`right.popularity - left.popularity`). A
    // ordem que o TMDB devolve e cronologica, e com ela os trabalhos pelos
    // quais a pessoa e conhecida ficam no fim da lista.
    { int i, j;
      for (i = 1; i < k; i++) {
        Credit tmp = ach[i];
        for (j = i; j > 0 && ach[j-1].pop < tmp.pop; j--) ach[j] = ach[j-1];
        ach[j] = tmp;
      } }

    pthread_mutex_lock(&lock);
    if (id == idRequest) {
      int i;
      snprintf(name, sizeof name, "%s", n[0] ? n : nameSeed);
      snprintf(photo, sizeof photo, "%s", f[0] ? f : photoSeed);
      snprintf(bio,  sizeof bio,  "%s", b);
      snprintf(area, sizeof area, "%s", a[0] ? translatesArea(a) : "");
      for (i = 0; i < k; i++) {
        snprintf(cred[i].t, sizeof cred[i].t, "%s", ach[i].t);
        snprintf(cred[i].p,  sizeof cred[i].p,  "%s", ach[i].p);
        snprintf(cred[i].y,    sizeof cred[i].y,    "%s", ach[i].y);
        snprintf(cred[i].po, sizeof cred[i].po, "%s", ach[i].po);
        snprintf(cred[i].im,   sizeof cred[i].im,   "%s", ach[i].im);
        cred[i].tmdb = ach[i].tmdb;
        snprintf(cred[i].kind, sizeof cred[i].kind, "%s", ach[i].kind);
      }
      nCred = k;
      ready = 1;
    }
    pthread_mutex_unlock(&lock);
    printf("[person] %ld %s -> %d credits\n", id, n, k); fflush(stdout);
  }
  free(body);
  pthread_mutex_lock(&lock);
  threadAlive = 0;
  pthread_mutex_unlock(&lock);
  return NULL;
}

void person_request(long tmdbId, const char *nameKnown, const char *photoKnown) {
  if (tmdbId <= 0) return;
  pthread_mutex_lock(&lock);
  if (idRequest == tmdbId) { pthread_mutex_unlock(&lock); return; }
  idRequest = tmdbId;
  nCred = 0; ready = 0; bio[0] = 0; area[0] = 0;
  // O nome e a foto que o elenco ja tinha entram na hora, para a tela abrir
  // com conteudo em vez de vazia enquanto a rede responde.
  snprintf(nameSeed, sizeof nameSeed, "%s", nameKnown ? nameKnown : "");
  snprintf(photoSeed, sizeof photoSeed, "%s", photoKnown ? photoKnown : "");
  snprintf(name, sizeof name, "%s", nameSeed);
  snprintf(photo, sizeof photo, "%s", photoSeed);
  if (threadAlive) { pthread_mutex_unlock(&lock); return; }
  idInProgress = tmdbId;
  threadAlive = 1;
  pthread_mutex_unlock(&lock);
  if (pthread_create(&thread, NULL, fetch, NULL) != 0) threadAlive = 0;
  else pthread_detach(thread);
}

int  person_ready(void)     { return ready; }
const char *person_name(void){ return name; }
const char *person_photo(void){ return photo; }
const char *person_bio(void) { return bio; }
const char *person_area(void){ return area; }

int person_n_credits(void)  { return nCred; }
const char *person_credit_title(int i) {
  return (i >= 0 && i < nCred) ? cred[i].t : "";
}
const char *person_credit_role(int i) {
  return (i >= 0 && i < nCred) ? cred[i].p : "";
}
const char *person_credit_year(int i) {
  return (i >= 0 && i < nCred) ? cred[i].y : "";
}
const char *person_credit_poster(int i) {
  return (i >= 0 && i < nCred) ? cred[i].po : "";
}
const char *person_credit_imdb(int i) {
  return (i >= 0 && i < nCred) ? cred[i].im : "";
}
long person_credit_tmdb(int i) {
  return (i >= 0 && i < nCred) ? cred[i].tmdb : 0;
}
const char *person_credit_kind(int i) {
  return (i >= 0 && i < nCred) ? cred[i].kind : "";
}
