#include "diretor.h"
#include "descoberta.h"
#include "rede.h"
#include "js.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DIR_MAX 16
typedef struct {
  char nome[96];
  int estado;               // 0 vazio, 1 em voo, 2 pronto, 3 falhou
  char foto[200], bio[1400], meta[220], conhecido[240];
} Ficha;
static Ficha fichas[DIR_MAX];
static int nFichas;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

static Ficha *achar(const char *nome) {
  for (int i = 0; i < nFichas; i++)
    if (!strcasecmp(fichas[i].nome, nome)) return &fichas[i];
  return NULL;
}

static void codificar(const char *s, char *dst, size_t cap) {
  size_t z = 0;
  for (const unsigned char *c = (const unsigned char *)s; *c && z + 4 < cap; c++) {
    if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'))
      dst[z++] = *c;
    else if (*c == ' ') dst[z++] = '+';
    else { snprintf(dst + z, 4, "%%%02X", *c); z += 3; }
  }
  dst[z] = 0;
}

static void *buscar(void *arg) {
  Ficha *f = arg;
  const char *chave = desc_chave_tmdb();
  char url[700], enc[300], *corpo;
  long id = 0;
  char foto[200] = "", conhecido[240] = "", bio[1400] = "", nasc[16] = "", lugar[120] = "";
  if (!chave || !chave[0]) goto falha;
  codificar(f->nome, enc, sizeof enc);
  snprintf(url, sizeof url, "https://api.themoviedb.org/3/search/person?api_key=%s"
           "&language=pt-BR&query=%s", chave, enc);
  corpo = rede_baixar(url, 15);
  if (!corpo) goto falha;
  { const char *r = js_array(corpo, NULL, "results");
    if (r) {
      const char *fr = js_fim(r);
      char cam[128] = "";
      id = (long)js_num(r, fr, "id", 0.0);
      if (js_texto(r, fr, "profile_path", cam, sizeof cam) && cam[0] == '/')
        snprintf(foto, sizeof foto, "https://image.tmdb.org/t/p/w500%s", cam);
      // known_for: titulos por que o TMDB o reconhece. "title" e filme,
      // "name" e serie.
      { const char *k = js_array(r, fr, "known_for"); size_t z = 0; int n = 0;
        while (k && n < 4) {
          const char *fk = js_fim(k); char t[96] = "";
          if (!js_texto(k, fk, "title", t, sizeof t)) js_texto(k, fk, "name", t, sizeof t);
          if (t[0]) {
            if (z && z + 5 < sizeof conhecido) { memcpy(conhecido + z, " \xc2\xb7 ", 4); z += 4; }
            size_t l = strlen(t); if (z + l + 1 >= sizeof conhecido) break;
            memcpy(conhecido + z, t, l); z += l; conhecido[z] = 0; n++;
          }
          k = js_prox(fk);
        } }
    } }
  free(corpo);
  if (!id) goto falha;
  for (int tent = 0; tent < 2 && !bio[0]; tent++) {
    snprintf(url, sizeof url, "https://api.themoviedb.org/3/person/%ld?api_key=%s&language=%s",
             id, chave, tent ? "en-US" : "pt-BR");
    corpo = rede_baixar(url, 15);
    if (!corpo) break;
    js_texto(corpo, NULL, "biography", bio, sizeof bio);
    if (!tent) {
      js_texto(corpo, NULL, "birthday", nasc, sizeof nasc);
      js_texto(corpo, NULL, "place_of_birth", lugar, sizeof lugar);
    }
    free(corpo);
  }
  // Quebras de paragrafo viram espaco: no hero cabem tres linhas.
  for (char *p = bio; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
  pthread_mutex_lock(&trava);
  snprintf(f->foto, sizeof f->foto, "%s", foto);
  snprintf(f->bio, sizeof f->bio, "%s", bio);
  snprintf(f->conhecido, sizeof f->conhecido, "%s", conhecido);
  { char data[64] = ""; size_t z;
    if (nasc[0]) desc_data_extenso(nasc, data, sizeof data);
    snprintf(f->meta, sizeof f->meta, "Diretor");
    z = strlen(f->meta);
    if (data[0]) z += snprintf(f->meta + z, sizeof f->meta - z, " \xc2\xb7 Nascido em %s", data);
    if (lugar[0] && z < sizeof f->meta) snprintf(f->meta + z, sizeof f->meta - z, " \xc2\xb7 %s", lugar);
  }
  f->estado = 2;
  pthread_mutex_unlock(&trava);
  printf("[diretor] %s: tmdb=%ld bio=%d conhecido='%s'\n", f->nome, id, (int)strlen(bio), conhecido);
  fflush(stdout);
  return NULL;
falha:
  pthread_mutex_lock(&trava); f->estado = 3; pthread_mutex_unlock(&trava);
  return NULL;
}

void diretor_pedir(const char *nome) {
  if (!nome || !nome[0]) return;
  pthread_mutex_lock(&trava);
  Ficha *f = achar(nome);
  if (!f) {
    if (nFichas >= DIR_MAX) { pthread_mutex_unlock(&trava); return; }
    f = &fichas[nFichas++]; memset(f, 0, sizeof *f);
    snprintf(f->nome, sizeof f->nome, "%s", nome);
  }
  if (f->estado != 0) { pthread_mutex_unlock(&trava); return; }
  f->estado = 1;
  pthread_mutex_unlock(&trava);
  pthread_t t; pthread_attr_t at;
  pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &at, buscar, f) != 0) {
    pthread_mutex_lock(&trava); f->estado = 3; pthread_mutex_unlock(&trava);
  }
  pthread_attr_destroy(&at);
}

static const Ficha *pronta(const char *nome) {
  const Ficha *f = nome ? achar(nome) : NULL;
  return (f && f->estado == 2) ? f : NULL;
}
int diretor_pronto(const char *nome) { return pronta(nome) != NULL; }
const char *diretor_foto(const char *nome) { const Ficha *f = pronta(nome); return f ? f->foto : ""; }
const char *diretor_bio(const char *nome) { const Ficha *f = pronta(nome); return f ? f->bio : ""; }
const char *diretor_meta(const char *nome) { const Ficha *f = pronta(nome); return f ? f->meta : ""; }
const char *diretor_conhecido(const char *nome) { const Ficha *f = pronta(nome); return f ? f->conhecido : ""; }
