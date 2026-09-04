#include "director.h"
#include "discover.h"
#include "net.h"
#include "js.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>

#define DIR_MAX 16
typedef struct {
  char name[96];
  int state;               // 0 vazio, 1 em voo, 2 pronto, 3 falhou
  unsigned attempts;
  uint64_t retryIn;
  long tmdb;
  char photo[200], hero[200], bio[1400], meta[220], known[240];
} Profile;
static Profile profiles[DIR_MAX];
static int nProfiles;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t nowMs(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t delayRetry(unsigned attempts) {
  static const uint64_t waits[] = { 1500, 5000, 30000, 300000 };
  unsigned i = attempts ? attempts - 1 : 0;
  if (i >= sizeof waits / sizeof waits[0]) i = sizeof waits / sizeof waits[0] - 1;
  return waits[i];
}

static int sameName(const char *a, const char *b) {
  while (a && *a == ' ') a++;
  while (b && *b == ' ') b++;
  if (!a || !b) return 0;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    a++; b++;
  }
  while (*a == ' ') a++;
  while (*b == ' ') b++;
  return *a == 0 && *b == 0;
}

// TMDB returns known_for_department already localised to whatever language the
// request asked for, so the Portuguese and Spanish spellings are DATA coming
// back from the API, not interface text. Translating them away would silently
// stop recognising directors on any localised response.
static int isDirecting(const char *department) {
  return department && (!strcasecmp(department, "Directing") ||
          !strcasecmp(department, "Director") ||
          !strcasecmp(department, "Direção") ||
          !strcasecmp(department, "Direccíon"));
}

static void failed(Profile *f) {
  pthread_mutex_lock(&lock);
  f->state = 3;
  f->retryIn = nowMs() + delayRetry(f->attempts);
  pthread_mutex_unlock(&lock);
}

static Profile *find(const char *name) {
  for (int i = 0; i < nProfiles; i++)
    if (!strcasecmp(profiles[i].name, name)) return &profiles[i];
  return NULL;
}

static void encode(const char *s, char *dst, size_t cap) {
  size_t z = 0;
  for (const unsigned char *c = (const unsigned char *)s; *c && z + 4 < cap; c++) {
    if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'))
      dst[z++] = *c;
    else if (*c == ' ') dst[z++] = '+';
    else { snprintf(dst + z, 4, "%%%02X", *c); z += 3; }
  }
  dst[z] = 0;
}

static void *fetch(void *arg) {
  Profile *f = arg;
  const char *key = disc_key_tmdb();
  char url[700], enc[300], *body;
  long id = 0;
  char photo[200] = "", hero[200] = "", known[240] = "", bio[1400] = "", birth[16] = "", place[120] = "";
  if (!key || !key[0]) goto failure;
  encode(f->name, enc, sizeof enc);
  snprintf(url, sizeof url, "https://api.themoviedb.org/3/search/person?api_key=%s"
           "&language=pt-BR&query=%s", key, enc);
  body = net_download(url, 15);
  if (!body) goto failure;
  { const char *r = js_array(body, NULL, "results");
    while (r && !id) {
      const char *fr = js_end(r);
      char cam[128] = "", person[96] = "", department[64] = "";
      js_text(r, fr, "name", person, sizeof person);
      js_text(r, fr, "known_for_department", department, sizeof department);
      // O resultado precisa casar com o nome pedido. Se o TMDB explicita
      // outra área, rejeitamos o homônimo em vez de roubar o retrato de um
      // ator com o mesmo nome. Departamento vazio ainda será confirmado pelo
      // endpoint da pessoa.
      if (sameName(person, f->name) &&
          (!department[0] || isDirecting(department))) {
        id = (long)js_num(r, fr, "id", 0.0);
        if (js_text(r, fr, "profile_path", cam, sizeof cam) && cam[0] == '/')
          // w500 fica borrado quando o retrato sobe para o hero/detalhe da TV.
          // A origem original preserva cabelo, olhos e recorte; o cache limita
          // o decode ao tamanho real de desenho, então não pesa a tela inteira.
          snprintf(photo, sizeof photo, "https://image.tmdb.org/t/p/original%s", cam);
          { const char *k = js_array(r, fr, "known_for"); size_t z = 0; int n = 0;
          while (k && n < 4) {
            const char *fk = js_end(k); char t[96] = "";
            if (!js_text(k, fk, "title", t, sizeof t)) js_text(k, fk, "name", t, sizeof t);
            if (!hero[0]) {
              char camHero[128] = "";
              if (js_text(k, fk, "backdrop_path", camHero, sizeof camHero) && camHero[0] == '/')
                snprintf(hero, sizeof hero, "https://image.tmdb.org/t/p/original%s", camHero);
            }
            if (t[0]) {
              if (z && z + 5 < sizeof known) { memcpy(known + z, " \xc2\xb7 ", 4); z += 4; }
              size_t l = strlen(t); if (z + l + 1 >= sizeof known) break;
              memcpy(known + z, t, l); z += l; known[z] = 0; n++;
            }
            k = js_next(fk);
          } }
      }
      r = js_next(fr);
    }
  }
  free(body);
  if (!id) goto failure;
  for (int tent = 0; tent < 2 && !bio[0]; tent++) {
    snprintf(url, sizeof url, "https://api.themoviedb.org/3/person/%ld?api_key=%s&language=%s",
             id, key, tent ? "en-US" : "pt-BR");
    body = net_download(url, 15);
    if (!body) continue;
    { char department[64] = "";
      js_text(body, NULL, "known_for_department", department, sizeof department);
      if (department[0] && !isDirecting(department)) { free(body); goto failure; }
    }
    js_text(body, NULL, "biography", bio, sizeof bio);
    if (!tent) {
      js_text(body, NULL, "birthday", birth, sizeof birth);
      js_text(body, NULL, "place_of_birth", place, sizeof place);
    }
    free(body);
  }
  // Quebras de paragrafo viram espaco: no hero cabem tres linhas.
  for (char *p = bio; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
  pthread_mutex_lock(&lock);
  f->tmdb = id;
  snprintf(f->photo, sizeof f->photo, "%s", photo);
  snprintf(f->hero, sizeof f->hero, "%s", hero);
  snprintf(f->bio, sizeof f->bio, "%s", bio);
  snprintf(f->known, sizeof f->known, "%s", known);
  { char date[64] = ""; size_t z;
    if (birth[0]) disc_date_long(birth, date, sizeof date);
    snprintf(f->meta, sizeof f->meta, "Director");
    z = strlen(f->meta);
    if (date[0]) z += snprintf(f->meta + z, sizeof f->meta - z, " \xc2\xb7 Born in %s", date);
    if (place[0] && z < sizeof f->meta) snprintf(f->meta + z, sizeof f->meta - z, " \xc2\xb7 %s", place);
  }
  f->state = 2;
  f->attempts = 0;
  f->retryIn = 0;
  pthread_mutex_unlock(&lock);
  printf("[director] %s: tmdb=%ld bio=%d known='%s'\n", f->name, id, (int)strlen(bio), known);
  fflush(stdout);
  return NULL;
failure:
  // Nao publica foto parcial antes da identidade ser confirmada. A proxima
  // chamada pode tentar novamente com backoff, sem bloquear o fio de desenho.
  failed(f);
  return NULL;
}

void director_request(const char *name) {
  if (!name || !name[0]) return;
  uint64_t now = nowMs();
  pthread_mutex_lock(&lock);
  Profile *f = find(name);
  if (!f) {
    if (nProfiles >= DIR_MAX) { pthread_mutex_unlock(&lock); return; }
    f = &profiles[nProfiles++]; memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
  }
  if (f->state == 1 || f->state == 2 ||
      (f->state == 3 && now < f->retryIn)) {
    pthread_mutex_unlock(&lock); return;
  }
  f->state = 1;
  f->attempts++;
  pthread_mutex_unlock(&lock);
  pthread_t t; pthread_attr_t at;
  pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &at, fetch, f) != 0) {
    // Mesmo a falha local de criar a thread precisa entrar no mesmo backoff
    // das falhas de rede; caso contrario cada frame tentaria criar outra.
    failed(f);
  }
  pthread_attr_destroy(&at);
}

static int profileReady(const char *name) {
  int ready = 0;
  pthread_mutex_lock(&lock);
  { const Profile *f = name ? find(name) : NULL;
    ready = f && f->state == 2; }
  pthread_mutex_unlock(&lock);
  return ready;
}
int director_ready(const char *name) { return profileReady(name); }
const char *director_photo(const char *name) {
  static char photoReturn[200];
  pthread_mutex_lock(&lock);
  const Profile *f = name ? find(name) : NULL;
  // A imagem so e publicada junto da identidade confirmada; uma resposta
  // parcial nunca pode vestir o detalhe com a foto de um homonimo.
  if (f && f->photo[0] && f->state == 2)
    snprintf(photoReturn, sizeof photoReturn, "%s", f->photo);
  else
    photoReturn[0] = 0;
  pthread_mutex_unlock(&lock);
  return photoReturn;
}
const char *director_hero(const char *name) {
  static char heroReturn[200];
  pthread_mutex_lock(&lock);
  { const Profile *f = name ? find(name) : NULL;
    if (f && f->hero[0] && f->state == 2)
      snprintf(heroReturn, sizeof heroReturn, "%s", f->hero);
    else
      heroReturn[0] = 0;
  }
  pthread_mutex_unlock(&lock);
  return heroReturn;
}
const char *director_bio(const char *name) {
  static char value[1400]; value[0] = 0;
  pthread_mutex_lock(&lock);
  { const Profile *f = name ? find(name) : NULL;
    if (f && f->state == 2) snprintf(value, sizeof value, "%s", f->bio); }
  pthread_mutex_unlock(&lock); return value;
}
const char *director_meta(const char *name) {
  static char value[220]; value[0] = 0;
  pthread_mutex_lock(&lock);
  { const Profile *f = name ? find(name) : NULL;
    if (f && f->state == 2) snprintf(value, sizeof value, "%s", f->meta); }
  pthread_mutex_unlock(&lock); return value;
}
const char *director_known(const char *name) {
  static char value[240]; value[0] = 0;
  pthread_mutex_lock(&lock);
  { const Profile *f = name ? find(name) : NULL;
    if (f && f->state == 2) snprintf(value, sizeof value, "%s", f->known); }
  pthread_mutex_unlock(&lock); return value;
}
