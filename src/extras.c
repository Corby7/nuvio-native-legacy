#include "extras.h"
#include "trakt.h"
#include "rede.h"
#include "js.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int  notaTrakt, votosTrakt;
static struct { char user[40]; char texto[420]; int curtidas; } coment[EX_COMENT_MAX];
static int  nComent;
static struct { char titulo[120], ano[8], imdb[16]; } rel[EX_REL_MAX];
static int  nRel;

static char idPedido[24], idEmCurso[24];
static int  serieEmCurso, fioVivo;
static pthread_t fio;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

// O Trakt devolve a nota como fracao de 0 a 10 com casas ("7.83521"); o resto
// do app guarda nota em 0..100 inteiro, como o campo `nota` do catalogo.
static int para100(double v) {
  int n = (int)(v * 10.0 + 0.5);
  if (n < 0) n = 0;
  if (n > 100) n = 100;
  return n;
}

// Um comentario pode ter quebras de linha e aspas escapadas; o desenho e de uma
// caixa de texto corrida, entao troca-se tudo por espaco. js_texto ja resolve o
// escape de aspas e transforma \\uXXXX em espaco.
static void numaLinha(char *s) {
  for (; *s; s++) if (*s == '\n' || *s == '\r' || *s == '\t') *s = ' ';
}

static void *buscar(void *arg) {
  const char *cab[4];
  char aut[200], chave[140], url[200], id[24];
  const char *tipo;
  char *corpo;
  (void)arg;

  pthread_mutex_lock(&trava);
  snprintf(id, sizeof id, "%s", idEmCurso);
  tipo = serieEmCurso ? "shows" : "movies";
  pthread_mutex_unlock(&trava);

  if (!trakt_cabecalhos(cab, aut, sizeof aut, chave, sizeof chave)) {
    pthread_mutex_lock(&trava); fioVivo = 0; pthread_mutex_unlock(&trava);
    return NULL;
  }

  // --- nota ---
  snprintf(url, sizeof url, "https://api.trakt.tv/%s/%s/ratings", tipo, id);
  corpo = rede_baixar_com(url, 12, cab);
  if (corpo) {
    int n = para100(js_num(corpo, NULL, "rating", 0.0));
    int v = (int)js_num(corpo, NULL, "votes", 0.0);
    free(corpo);
    pthread_mutex_lock(&trava);
    if (!strcmp(id, idPedido)) { notaTrakt = n; votosTrakt = v; }
    pthread_mutex_unlock(&trava);
  }

  // --- comentarios, os mais curtidos primeiro ---
  snprintf(url, sizeof url,
           "https://api.trakt.tv/%s/%s/comments/likes?limit=%d", tipo, id,
           EX_COMENT_MAX);
  corpo = rede_baixar_com(url, 12, cab);
  if (corpo) {
    struct { char u[40]; char t[420]; int c; } achado[EX_COMENT_MAX];
    int n = 0;
    // p+1 e nao js_prox: js_prox recebe o FIM do elemento anterior, e aqui
    // ainda nao ha anterior. Com js_prox o primeiro item era pulado e, em
    // resposta de tres itens, sobrava lixo — as duas listas vinham vazias.
    const char *p = strchr(corpo, '[');
    p = p ? p + 1 : NULL;
    while (p && n < EX_COMENT_MAX) {
      const char *f = js_fim(p);
      achado[n].u[0] = achado[n].t[0] = 0;
      js_texto(p, f, "comment", achado[n].t, sizeof achado[n].t);
      // "username" esta dentro do objeto `user`; js_texto varre a faixa toda e
      // a unica ocorrencia dessa chave no item e essa.
      js_texto(p, f, "username", achado[n].u, sizeof achado[n].u);
      achado[n].c = (int)js_num(p, f, "likes", 0.0);
      numaLinha(achado[n].t);
      if (achado[n].t[0]) n++;
      p = js_prox(f);
    }
    free(corpo);
    pthread_mutex_lock(&trava);
    if (!strcmp(id, idPedido)) {
      int k;
      for (k = 0; k < n; k++) {
        snprintf(coment[k].user, sizeof coment[k].user, "%s", achado[k].u);
        snprintf(coment[k].texto, sizeof coment[k].texto, "%s", achado[k].t);
        coment[k].curtidas = achado[k].c;
      }
      nComent = n;
    }
    pthread_mutex_unlock(&trava);
  }

  // --- relacionados ---
  snprintf(url, sizeof url, "https://api.trakt.tv/%s/%s/related?limit=%d",
           tipo, id, EX_REL_MAX);
  corpo = rede_baixar_com(url, 15, cab);
  if (corpo) {
    struct { char t[120], a[8], i[16]; } achado[EX_REL_MAX];
    int n = 0;
    // p+1 e nao js_prox: js_prox recebe o FIM do elemento anterior, e aqui
    // ainda nao ha anterior. Com js_prox o primeiro item era pulado e, em
    // resposta de tres itens, sobrava lixo — as duas listas vinham vazias.
    const char *p = strchr(corpo, '[');
    p = p ? p + 1 : NULL;
    while (p && n < EX_REL_MAX) {
      const char *f = js_fim(p);
      double ano;
      achado[n].t[0] = achado[n].i[0] = 0;
      js_texto(p, f, "title", achado[n].t, sizeof achado[n].t);
      js_texto(p, f, "imdb", achado[n].i, sizeof achado[n].i);
      ano = js_num(p, f, "year", 0.0);
      if (ano > 1800.0) snprintf(achado[n].a, sizeof achado[n].a, "%d", (int)ano);
      else achado[n].a[0] = 0;
      if (achado[n].t[0] && achado[n].i[0]) n++;
      p = js_prox(f);
    }
    free(corpo);
    pthread_mutex_lock(&trava);
    if (!strcmp(id, idPedido)) {
      int k;
      for (k = 0; k < n; k++) {
        snprintf(rel[k].titulo, sizeof rel[k].titulo, "%s", achado[k].t);
        snprintf(rel[k].ano, sizeof rel[k].ano, "%s", achado[k].a);
        snprintf(rel[k].imdb, sizeof rel[k].imdb, "%s", achado[k].i);
      }
      nRel = n;
    }
    pthread_mutex_unlock(&trava);
  }

  printf("[extras] %s -> nota=%d coment=%d rel=%d\n", id, notaTrakt, nComent, nRel);
  fflush(stdout);
  pthread_mutex_lock(&trava);
  fioVivo = 0;
  pthread_mutex_unlock(&trava);
  return NULL;
}

void extras_pedir(const char *imdb, int serie) {
  char id[24];
  const char *dp;
  if (!imdb || imdb[0] != 't' || !trakt_ativo()) return;
  // O campo do catalogo pode vir com episodio ("tt9737326:2:1"), que e o
  // formato que os addons de fonte usam. O Trakt so conhece o id do TITULO —
  // com o sufixo ele responde 404 e as tres abas ficavam vazias em toda serie.
  dp = strchr(imdb, ':');
  if (dp) { size_t n = (size_t)(dp - imdb);
            if (n >= sizeof id) n = sizeof id - 1;
            memcpy(id, imdb, n); id[n] = 0; }
  else snprintf(id, sizeof id, "%s", imdb);
  imdb = id;
  pthread_mutex_lock(&trava);
  if (!strcmp(idPedido, imdb)) { pthread_mutex_unlock(&trava); return; }
  snprintf(idPedido, sizeof idPedido, "%s", imdb);
  notaTrakt = votosTrakt = nComent = nRel = 0;
  if (fioVivo) { pthread_mutex_unlock(&trava); return; }
  snprintf(idEmCurso, sizeof idEmCurso, "%s", imdb);
  serieEmCurso = serie;
  fioVivo = 1;
  pthread_mutex_unlock(&trava);
  if (pthread_create(&fio, NULL, buscar, NULL) != 0) fioVivo = 0;
  else pthread_detach(fio);
}

int extras_nota_trakt(void)  { return notaTrakt; }
int extras_votos_trakt(void) { return votosTrakt; }

int extras_n_comentarios(void) { return nComent; }
const char *extras_comentario_usuario(int i) {
  return (i >= 0 && i < nComent) ? coment[i].user : "";
}
const char *extras_comentario_texto(int i) {
  return (i >= 0 && i < nComent) ? coment[i].texto : "";
}
int extras_comentario_curtidas(int i) {
  return (i >= 0 && i < nComent) ? coment[i].curtidas : 0;
}

int extras_n_relacionados(void) { return nRel; }
const char *extras_relacionado_titulo(int i) {
  return (i >= 0 && i < nRel) ? rel[i].titulo : "";
}
const char *extras_relacionado_ano(int i) {
  return (i >= 0 && i < nRel) ? rel[i].ano : "";
}
const char *extras_relacionado_imdb(int i) {
  return (i >= 0 && i < nRel) ? rel[i].imdb : "";
}
