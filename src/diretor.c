#include "diretor.h"
#include "descoberta.h"
#include "rede.h"
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
  char nome[96];
  int estado;               // 0 vazio, 1 em voo, 2 pronto, 3 falhou
  unsigned tentativas;
  uint64_t retryEm;
  long tmdb;
  char foto[200], hero[200], bio[1400], meta[220], conhecido[240];
} Ficha;
static Ficha fichas[DIR_MAX];
static int nFichas;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

static uint64_t agoraMs(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t atrasoRetry(unsigned tentativas) {
  static const uint64_t espera[] = { 1500, 5000, 30000, 300000 };
  unsigned i = tentativas ? tentativas - 1 : 0;
  if (i >= sizeof espera / sizeof espera[0]) i = sizeof espera / sizeof espera[0] - 1;
  return espera[i];
}

static int mesmoNome(const char *a, const char *b) {
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

static int eDirecao(const char *departamento) {
  return departamento && (!strcasecmp(departamento, "Directing") ||
          !strcasecmp(departamento, "Director") ||
          !strcasecmp(departamento, "Direção") ||
          !strcasecmp(departamento, "Direccíon"));
}

static void falhou(Ficha *f) {
  pthread_mutex_lock(&trava);
  f->estado = 3;
  f->retryEm = agoraMs() + atrasoRetry(f->tentativas);
  pthread_mutex_unlock(&trava);
}

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
  char foto[200] = "", hero[200] = "", conhecido[240] = "", bio[1400] = "", nasc[16] = "", lugar[120] = "";
  if (!chave || !chave[0]) goto falha;
  codificar(f->nome, enc, sizeof enc);
  snprintf(url, sizeof url, "https://api.themoviedb.org/3/search/person?api_key=%s"
           "&language=pt-BR&query=%s", chave, enc);
  corpo = rede_baixar(url, 15);
  if (!corpo) goto falha;
  { const char *r = js_array(corpo, NULL, "results");
    while (r && !id) {
      const char *fr = js_fim(r);
      char cam[128] = "", pessoa[96] = "", departamento[64] = "";
      js_texto(r, fr, "name", pessoa, sizeof pessoa);
      js_texto(r, fr, "known_for_department", departamento, sizeof departamento);
      // O resultado precisa casar com o nome pedido. Se o TMDB explicita
      // outra área, rejeitamos o homônimo em vez de roubar o retrato de um
      // ator com o mesmo nome. Departamento vazio ainda será confirmado pelo
      // endpoint da pessoa.
      if (mesmoNome(pessoa, f->nome) &&
          (!departamento[0] || eDirecao(departamento))) {
        id = (long)js_num(r, fr, "id", 0.0);
        if (js_texto(r, fr, "profile_path", cam, sizeof cam) && cam[0] == '/')
          // w500 fica borrado quando o retrato sobe para o hero/detalhe da TV.
          // A origem original preserva cabelo, olhos e recorte; o cache limita
          // o decode ao tamanho real de desenho, então não pesa a tela inteira.
          snprintf(foto, sizeof foto, "https://image.tmdb.org/t/p/original%s", cam);
          { const char *k = js_array(r, fr, "known_for"); size_t z = 0; int n = 0;
          while (k && n < 4) {
            const char *fk = js_fim(k); char t[96] = "";
            if (!js_texto(k, fk, "title", t, sizeof t)) js_texto(k, fk, "name", t, sizeof t);
            if (!hero[0]) {
              char camHero[128] = "";
              if (js_texto(k, fk, "backdrop_path", camHero, sizeof camHero) && camHero[0] == '/')
                snprintf(hero, sizeof hero, "https://image.tmdb.org/t/p/original%s", camHero);
            }
            if (t[0]) {
              if (z && z + 5 < sizeof conhecido) { memcpy(conhecido + z, " \xc2\xb7 ", 4); z += 4; }
              size_t l = strlen(t); if (z + l + 1 >= sizeof conhecido) break;
              memcpy(conhecido + z, t, l); z += l; conhecido[z] = 0; n++;
            }
            k = js_prox(fk);
          } }
      }
      r = js_prox(fr);
    }
  }
  free(corpo);
  if (!id) goto falha;
  for (int tent = 0; tent < 2 && !bio[0]; tent++) {
    snprintf(url, sizeof url, "https://api.themoviedb.org/3/person/%ld?api_key=%s&language=%s",
             id, chave, tent ? "en-US" : "pt-BR");
    corpo = rede_baixar(url, 15);
    if (!corpo) continue;
    { char departamento[64] = "";
      js_texto(corpo, NULL, "known_for_department", departamento, sizeof departamento);
      if (departamento[0] && !eDirecao(departamento)) { free(corpo); goto falha; }
    }
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
  f->tmdb = id;
  snprintf(f->foto, sizeof f->foto, "%s", foto);
  snprintf(f->hero, sizeof f->hero, "%s", hero);
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
  f->tentativas = 0;
  f->retryEm = 0;
  pthread_mutex_unlock(&trava);
  printf("[diretor] %s: tmdb=%ld bio=%d conhecido='%s'\n", f->nome, id, (int)strlen(bio), conhecido);
  fflush(stdout);
  return NULL;
falha:
  // Nao publica foto parcial antes da identidade ser confirmada. A proxima
  // chamada pode tentar novamente com backoff, sem bloquear o fio de desenho.
  falhou(f);
  return NULL;
}

void diretor_pedir(const char *nome) {
  if (!nome || !nome[0]) return;
  uint64_t agora = agoraMs();
  pthread_mutex_lock(&trava);
  Ficha *f = achar(nome);
  if (!f) {
    if (nFichas >= DIR_MAX) { pthread_mutex_unlock(&trava); return; }
    f = &fichas[nFichas++]; memset(f, 0, sizeof *f);
    snprintf(f->nome, sizeof f->nome, "%s", nome);
  }
  if (f->estado == 1 || f->estado == 2 ||
      (f->estado == 3 && agora < f->retryEm)) {
    pthread_mutex_unlock(&trava); return;
  }
  f->estado = 1;
  f->tentativas++;
  pthread_mutex_unlock(&trava);
  pthread_t t; pthread_attr_t at;
  pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &at, buscar, f) != 0) {
    // Mesmo a falha local de criar a thread precisa entrar no mesmo backoff
    // das falhas de rede; caso contrario cada frame tentaria criar outra.
    falhou(f);
  }
  pthread_attr_destroy(&at);
}

static int fichaPronta(const char *nome) {
  int pronta = 0;
  pthread_mutex_lock(&trava);
  { const Ficha *f = nome ? achar(nome) : NULL;
    pronta = f && f->estado == 2; }
  pthread_mutex_unlock(&trava);
  return pronta;
}
int diretor_pronto(const char *nome) { return fichaPronta(nome); }
const char *diretor_foto(const char *nome) {
  static char fotoRetorno[200];
  pthread_mutex_lock(&trava);
  const Ficha *f = nome ? achar(nome) : NULL;
  // A imagem so e publicada junto da identidade confirmada; uma resposta
  // parcial nunca pode vestir o detalhe com a foto de um homonimo.
  if (f && f->foto[0] && f->estado == 2)
    snprintf(fotoRetorno, sizeof fotoRetorno, "%s", f->foto);
  else
    fotoRetorno[0] = 0;
  pthread_mutex_unlock(&trava);
  return fotoRetorno;
}
const char *diretor_hero(const char *nome) {
  static char heroRetorno[200];
  pthread_mutex_lock(&trava);
  { const Ficha *f = nome ? achar(nome) : NULL;
    if (f && f->hero[0] && f->estado == 2)
      snprintf(heroRetorno, sizeof heroRetorno, "%s", f->hero);
    else
      heroRetorno[0] = 0;
  }
  pthread_mutex_unlock(&trava);
  return heroRetorno;
}
const char *diretor_bio(const char *nome) {
  static char valor[1400]; valor[0] = 0;
  pthread_mutex_lock(&trava);
  { const Ficha *f = nome ? achar(nome) : NULL;
    if (f && f->estado == 2) snprintf(valor, sizeof valor, "%s", f->bio); }
  pthread_mutex_unlock(&trava); return valor;
}
const char *diretor_meta(const char *nome) {
  static char valor[220]; valor[0] = 0;
  pthread_mutex_lock(&trava);
  { const Ficha *f = nome ? achar(nome) : NULL;
    if (f && f->estado == 2) snprintf(valor, sizeof valor, "%s", f->meta); }
  pthread_mutex_unlock(&trava); return valor;
}
const char *diretor_conhecido(const char *nome) {
  static char valor[240]; valor[0] = 0;
  pthread_mutex_lock(&trava);
  { const Ficha *f = nome ? achar(nome) : NULL;
    if (f && f->estado == 2) snprintf(valor, sizeof valor, "%s", f->conhecido); }
  pthread_mutex_unlock(&trava); return valor;
}
