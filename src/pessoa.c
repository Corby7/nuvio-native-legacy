#include "pessoa.h"
#include "descoberta.h"
#include "rede.h"
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
  char tipo[8];          // "movie" ou "tv"
  double pop;
} Credito;
static Credito cred[PES_MAX];
static int nCred;

static char nome[80], foto[200], bio[1400], area[24];
static long idPedido, idEmCurso;
static char nomeSemente[80], fotoSemente[200];
static int  pronta, fioVivo;
static pthread_t fio;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

// known_for_department vem em ingles do TMDB mesmo com language=pt-BR.
static const char *traduzArea(const char *s) {
  if (!strcmp(s, "Acting"))    return "Atua\xc3\xa7\xc3\xa3" "o";
  if (!strcmp(s, "Directing")) return "Dire\xc3\xa7\xc3\xa3" "o";
  if (!strcmp(s, "Writing"))   return "Roteiro";
  if (!strcmp(s, "Production"))return "Produ\xc3\xa7\xc3\xa3" "o";
  if (!strcmp(s, "Sound"))     return "Som";
  if (!strcmp(s, "Camera"))    return "Fotografia";
  return s;
}

static void *buscar(void *arg) {
  char url[400], *corpo;
  long id;
  const char *chave = desc_chave_tmdb();
  (void)arg;

  pthread_mutex_lock(&trava);
  id = idEmCurso;
  pthread_mutex_unlock(&trava);
  if (!chave || !chave[0]) {
    pthread_mutex_lock(&trava); fioVivo = 0; pthread_mutex_unlock(&trava);
    return NULL;
  }

  // O MESMO pedido do web (castDetailScreen.js:204): a ficha e os creditos
  // combinados numa chamada so. Separado seriam dois pedidos para desenhar uma
  // tela — e o segundo so serve se o primeiro deu certo.
  snprintf(url, sizeof url,
           "%s/person/%ld?api_key=%s&language=pt-BR"
           "&append_to_response=combined_credits", TMDB, id, chave);
  corpo = rede_baixar(url, 20);
  if (!corpo) {
    pthread_mutex_lock(&trava); fioVivo = 0; pthread_mutex_unlock(&trava);
    return NULL;
  }

  {
    char n[80] = "", f[160] = "", b[1400] = "", a[24] = "";
    Credito ach[PES_MAX];
    int k = 0;

    js_texto(corpo, NULL, "name", n, sizeof n);
    js_texto(corpo, NULL, "biography", b, sizeof b);
    js_texto(corpo, NULL, "known_for_department", a, sizeof a);
    { char caminho[128] = "";
      if (js_texto(corpo, NULL, "profile_path", caminho, sizeof caminho) &&
          caminho[0] == '/')
        snprintf(f, sizeof f, "https://image.tmdb.org/t/p/w342%s", caminho); }

    // `combined_credits.cast` — o array vem depois da chave, e js_array acha o
    // primeiro "cast" do documento. O objeto raiz nao tem outro "cast", entao
    // e este.
    { const char *p = js_array(corpo, NULL, "cast");
      while (p && k < PES_MAX) {
        const char *fim = js_fim(p);
        char data[16] = "", caminho[128] = "";
        ach[k].t[0] = ach[k].p[0] = ach[k].y[0] = ach[k].po[0] = ach[k].im[0] = 0;
        // Filme tem "title"/"release_date"; serie tem "name"/"first_air_date".
        if (!js_texto(p, fim, "title", ach[k].t, sizeof ach[k].t))
          js_texto(p, fim, "name", ach[k].t, sizeof ach[k].t);
        js_texto(p, fim, "character", ach[k].p, sizeof ach[k].p);
        if (!js_texto(p, fim, "release_date", data, sizeof data))
          js_texto(p, fim, "first_air_date", data, sizeof data);
        if (strlen(data) >= 4) { memcpy(ach[k].y, data, 4); ach[k].y[4] = 0; }
        if (js_texto(p, fim, "poster_path", caminho, sizeof caminho) &&
            caminho[0] == '/')
          snprintf(ach[k].po, sizeof ach[k].po,
                   "https://image.tmdb.org/t/p/w342%s", caminho);
        js_texto(p, fim, "imdb_id", ach[k].im, sizeof ach[k].im);
        ach[k].tmdb = (long)js_num(p, fim, "id", 0.0);
        ach[k].tipo[0] = 0;
        js_texto(p, fim, "media_type", ach[k].tipo, sizeof ach[k].tipo);
        ach[k].pop = js_num(p, fim, "popularity", 0.0);
        if (ach[k].t[0]) k++;
        p = js_prox(fim);
      } }

    // Por POPULARIDADE, como o web (`right.popularity - left.popularity`). A
    // ordem que o TMDB devolve e cronologica, e com ela os trabalhos pelos
    // quais a pessoa e conhecida ficam no fim da lista.
    { int i, j;
      for (i = 1; i < k; i++) {
        Credito tmp = ach[i];
        for (j = i; j > 0 && ach[j-1].pop < tmp.pop; j--) ach[j] = ach[j-1];
        ach[j] = tmp;
      } }

    pthread_mutex_lock(&trava);
    if (id == idPedido) {
      int i;
      snprintf(nome, sizeof nome, "%s", n[0] ? n : nomeSemente);
      snprintf(foto, sizeof foto, "%s", f[0] ? f : fotoSemente);
      snprintf(bio,  sizeof bio,  "%s", b);
      snprintf(area, sizeof area, "%s", a[0] ? traduzArea(a) : "");
      for (i = 0; i < k; i++) {
        snprintf(cred[i].t, sizeof cred[i].t, "%s", ach[i].t);
        snprintf(cred[i].p,  sizeof cred[i].p,  "%s", ach[i].p);
        snprintf(cred[i].y,    sizeof cred[i].y,    "%s", ach[i].y);
        snprintf(cred[i].po, sizeof cred[i].po, "%s", ach[i].po);
        snprintf(cred[i].im,   sizeof cred[i].im,   "%s", ach[i].im);
        cred[i].tmdb = ach[i].tmdb;
        snprintf(cred[i].tipo, sizeof cred[i].tipo, "%s", ach[i].tipo);
      }
      nCred = k;
      pronta = 1;
    }
    pthread_mutex_unlock(&trava);
    printf("[pessoa] %ld %s -> %d creditos\n", id, n, k); fflush(stdout);
  }
  free(corpo);
  pthread_mutex_lock(&trava);
  fioVivo = 0;
  pthread_mutex_unlock(&trava);
  return NULL;
}

void pessoa_pedir(long tmdbId, const char *nomeConhecido, const char *fotoConhecida) {
  if (tmdbId <= 0) return;
  pthread_mutex_lock(&trava);
  if (idPedido == tmdbId) { pthread_mutex_unlock(&trava); return; }
  idPedido = tmdbId;
  nCred = 0; pronta = 0; bio[0] = 0; area[0] = 0;
  // O nome e a foto que o elenco ja tinha entram na hora, para a tela abrir
  // com conteudo em vez de vazia enquanto a rede responde.
  snprintf(nomeSemente, sizeof nomeSemente, "%s", nomeConhecido ? nomeConhecido : "");
  snprintf(fotoSemente, sizeof fotoSemente, "%s", fotoConhecida ? fotoConhecida : "");
  snprintf(nome, sizeof nome, "%s", nomeSemente);
  snprintf(foto, sizeof foto, "%s", fotoSemente);
  if (fioVivo) { pthread_mutex_unlock(&trava); return; }
  idEmCurso = tmdbId;
  fioVivo = 1;
  pthread_mutex_unlock(&trava);
  if (pthread_create(&fio, NULL, buscar, NULL) != 0) fioVivo = 0;
  else pthread_detach(fio);
}

int  pessoa_pronta(void)     { return pronta; }
const char *pessoa_nome(void){ return nome; }
const char *pessoa_foto(void){ return foto; }
const char *pessoa_bio(void) { return bio; }
const char *pessoa_area(void){ return area; }

int pessoa_n_creditos(void)  { return nCred; }
const char *pessoa_credito_titulo(int i) {
  return (i >= 0 && i < nCred) ? cred[i].t : "";
}
const char *pessoa_credito_papel(int i) {
  return (i >= 0 && i < nCred) ? cred[i].p : "";
}
const char *pessoa_credito_ano(int i) {
  return (i >= 0 && i < nCred) ? cred[i].y : "";
}
const char *pessoa_credito_poster(int i) {
  return (i >= 0 && i < nCred) ? cred[i].po : "";
}
const char *pessoa_credito_imdb(int i) {
  return (i >= 0 && i < nCred) ? cred[i].im : "";
}
long pessoa_credito_tmdb(int i) {
  return (i >= 0 && i < nCred) ? cred[i].tmdb : 0;
}
const char *pessoa_credito_tipo(int i) {
  return (i >= 0 && i < nCred) ? cred[i].tipo : "";
}
