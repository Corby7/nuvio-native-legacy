#include "extras.h"
#include "trakt.h"
#include "rede.h"
#include "js.h"
#include "descoberta.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int  notaTrakt, votosTrakt;
static int  notas[EX_NFONTES];
static char mdbChave[80];
static char dirArteEx[512];

// Nome do provedor na api do mdbList E nome do arquivo de marca em art/marcas.
// A ordem e a do enum, que e a do renderExternalRatingsRow do web.
static const char *FONTE[EX_NFONTES] = {
  "trakt", "imdb", "tmdb", "tomatoes", "audience", "metacritic", "letterboxd"
};

// A ESCALA MUDA POR PROVEDOR, e nao do jeito que parece. CONFERIDO na api com
// tt9737326: imdb=6.2 (0..10), mas trakt=66 e tmdb=70 — os dois em PORCENTAGEM,
// junto com tomatoes=59, audience=46 e metacritic=53. Eu tinha suposto que
// trakt e tmdb viessem em 0..10 como o imdb, e o resultado era "10.0" nos dois
// (66 x 10 estourava o teto e grudava no maximo).
//
// Guardamos o valor CRU multiplicado por 10, para o imdb caber em inteiro sem
// perder a casa decimal; quem desenha divide de novo conforme o provedor.
//
// DIVERGENCIA ANOTADA: o web nao reescala nada — formatMdbListRating
// (metaDetailsScreen.js:732) imprime o numero como veio, entao la o TMDB
// aparece como "70.0" ao lado de um IMDb "6.2". Aqui o TMDB vira "70%", que e
// o que o numero de fato e.
static int emDecimos(double v) {
  int n = (int)(v * 10.0 + 0.5);
  if (n < 0) n = 0;
  if (n > 1000) n = 1000;
  return n;
}

// 1 quando a nota da fonte e uma PORCENTAGEM; 0 quando e nota de 0 a 10.
int extras_fonte_percentual(int fonte) {
  return fonte != EX_IMDB && fonte != EX_LETTERBOXD;
}

void extras_carregar(const char *dirArte) {
  char caminho[600];
  FILE *f;
  snprintf(dirArteEx, sizeof dirArteEx, "%s", dirArte ? dirArte : ".");
  snprintf(caminho, sizeof caminho, "%s/mdblist.txt", dirArte ? dirArte : ".");
  f = fopen(caminho, "r");
  if (!f) { printf("[extras] mdblist ausente\n"); fflush(stdout); return; }
  if (fgets(mdbChave, sizeof mdbChave, f)) {
    char *fim = mdbChave + strlen(mdbChave);
    while (fim > mdbChave && (fim[-1] == '\n' || fim[-1] == '\r')) *--fim = 0;
  }
  fclose(f);
  printf("[extras] mdblist %s\n", mdbChave[0] ? "ok" : "vazio"); fflush(stdout);
}

int extras_nota(int fonte) {
  return (fonte >= 0 && fonte < EX_NFONTES) ? notas[fonte] : 0;
}
const char *extras_fonte_marca(int fonte) {
  return (fonte >= 0 && fonte < EX_NFONTES) ? FONTE[fonte] : "";
}

const char *extras_caminho_marca(int fonte) {
  // ABSOLUTO. O cache de textura chama IMG_Load com o caminho como veio, e o
  // diretorio de trabalho do app nao e a pasta da arte — com "marcas/x.png"
  // relativo o arquivo simplesmente nao era achado e o cartao saia sem logo,
  // sem erro nenhum. O catalogo ja faz assim (catalogo.c:79).
  static char cam[600];
  if (fonte < 0 || fonte >= EX_NFONTES) return "";
  snprintf(cam, sizeof cam, "%s/marcas/%s.png", dirArteEx, FONTE[fonte]);
  return cam;
}
static struct { char user[40]; char texto[420]; int curtidas; } coment[EX_COMENT_MAX];
static int  nComent;
static struct { char titulo[120], ano[8], imdb[16], poster[200]; } rel[EX_REL_MAX];
// Vistos: um bit por episodio, ate 40 episodios em 20 temporadas. Vetor fixo
// porque a consulta acontece no DESENHO de cada card, a cada quadro — uma
// busca em lista ali custaria mais que a resposta.
#define EX_VIS_T 20
#define EX_VIS_E 40
static unsigned char vistos[EX_VIS_T][EX_VIS_E];
static int  nRel;
static struct { int numero; int nEps; struct { int ep, nota; } eps[EX_EP_MAX]; }
            temps[EX_TEMP_MAX];
static int  nTemps;
static char colNome[80];
static struct { char titulo[120], ano[8]; long tmdb; } col[EX_COL_MAX];
static int  nCol;
static long tmdbEmCurso;

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
    if (!strcmp(id, idPedido)) {
      notaTrakt = n; votosTrakt = v;
      // Sem chave do mdbList esta e a UNICA nota do Trakt que teremos; com
      // chave, o passo seguinte sobrescreve com a que o mdbList devolver, que
      // e a mesma fonte que o web mostra.
      // notaTrakt esta em 0..100 (a api do Trakt devolve 0..10). No vetor a
      // escala e "cru x 10" e a fonte trakt e percentual, entao 6.7 -> 67% ->
      // 670. Sem esta conversao o cartao mostrava 6.7% quando nao havia
      // mdbList.
      if (!notas[EX_TRAKT]) notas[EX_TRAKT] = n * 10;
    }
    pthread_mutex_unlock(&trava);
  }

  // --- notas do mdbList, se o dono tiver chave ---
  //
  // Um POST por provedor, como o web faz (fetchProviderRating): a api aceita
  // "ids" em lote mas so um provedor por chamada. Sao sete chamadas curtas; o
  // fio ja e proprio, entao nao atrapalha o desenho.
  if (mdbChave[0]) {
    const char *cabJ[3];
    char kj[64];
    char corpoPost[80];
    int k;
    snprintf(kj, sizeof kj, "content-type: application/json");
    cabJ[0] = kj; cabJ[1] = NULL; cabJ[2] = NULL;
    snprintf(corpoPost, sizeof corpoPost,
             "{\"ids\":[\"%s\"],\"provider\":\"imdb\"}", id);
    for (k = 0; k < EX_NFONTES; k++) {
      char u[300], *rp;
      snprintf(u, sizeof u, "https://api.mdblist.com/rating/%s/%s?apikey=%s",
               serieEmCurso ? "show" : "movie", FONTE[k], mdbChave);
      rp = rede_postar(u, 12, cabJ, corpoPost);
      if (!rp) continue;
      { double v = js_num(rp, NULL, "rating", -1.0);
        free(rp);
        if (v >= 0.0) {
          int c = emDecimos(v);
          pthread_mutex_lock(&trava);
          if (!strcmp(id, idPedido)) notas[k] = c;
          pthread_mutex_unlock(&trava);
        } }
    }
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

  // --- notas por episodio, so em serie ---
  if (serieEmCurso) {
    snprintf(url, sizeof url,
             "https://api.trakt.tv/shows/%s/seasons?extended=episodes,full", id);
    corpo = rede_baixar_com(url, 20, cab);
    if (corpo) {
      int nt = 0;
      const char *p = strchr(corpo, '[');
      p = p ? p + 1 : NULL;
      while (p && nt < EX_TEMP_MAX) {
        const char *f = js_fim(p);
        int num = (int)js_num(p, f, "number", -1.0);
        // Temporada 0 e "especiais"; o web filtra `value > 0`.
        if (num > 0) {
          const char *q = js_array(p, f, "episodes");
          int ne = 0;
          while (q && ne < EX_EP_MAX) {
            const char *qf = js_fim(q);
            int en = (int)js_num(q, qf, "number", -1.0);
            double r = js_num(q, qf, "rating", 0.0);
            if (en > 0) {
              temps[nt].eps[ne].ep = en;
              temps[nt].eps[ne].nota = (int)(r * 10.0 + 0.5);
              ne++;
            }
            q = js_prox(qf);
          }
          if (ne > 0) { temps[nt].numero = num; temps[nt].nEps = ne; nt++; }
        }
        p = js_prox(f);
      }
      free(corpo);
      pthread_mutex_lock(&trava);
      if (!strcmp(id, idPedido)) nTemps = nt;
      pthread_mutex_unlock(&trava);
    }
  }

  // --- episodios ja assistidos (so serie) ---
  if (serieEmCurso) {
    snprintf(url, sizeof url,
             "https://api.trakt.tv/shows/%s/progress/watched", id);
    corpo = rede_baixar_com(url, 20, cab);
    if (corpo) {
      unsigned char novo[EX_VIS_T][EX_VIS_E];
      const char *p = js_array(corpo, NULL, "seasons");
      memset(novo, 0, sizeof novo);
      while (p) {
        const char *f = js_fim(p);
        int t = (int)js_num(p, f, "number", -1.0);
        if (t >= 0 && t < EX_VIS_T) {
          const char *q = js_array(p, f, "episodes");
          while (q) {
            const char *qf = js_fim(q);
            int en = (int)js_num(q, qf, "number", -1.0);
            // "completed" e booleano; js_num nao le true/false, entao a leitura
            // e pelo texto — foi assim que a primeira versao marcou tudo como
            // nao visto sem erro nenhum.
            const char *c = strstr(q, "\"completed\"");
            int visto = 0;
            if (c && c < qf) { const char *v = c + 12;
                               while (*v == ' ' || *v == ':') v++;
                               visto = (*v == 't'); }
            if (visto && en > 0 && en < EX_VIS_E) novo[t][en] = 1;
            q = js_prox(qf);
          }
        }
        p = js_prox(f);
      }
      free(corpo);
      pthread_mutex_lock(&trava);
      if (!strcmp(id, idPedido)) memcpy(vistos, novo, sizeof vistos);
      pthread_mutex_unlock(&trava);
    }
  }

  // --- colecao (so filme, e so quando ja sabemos o id do TMDB) ---
  if (!serieEmCurso) {
    const char *chave = desc_chave_tmdb();
    long idCol = 0, idFilme = tmdbEmCurso;
    char nome[80] = "";
    // O id do TMDB so fica no catalogo DEPOIS do enriquecimento do elenco; na
    // PRIMEIRA abertura de um titulo ele ainda e 0, e a aba nao apareceria
    // justamente na visita em que o dono esta olhando. /find resolve na hora.
    if (chave && chave[0] && idFilme <= 0) {
      snprintf(url, sizeof url,
               "https://api.themoviedb.org/3/find/%s?api_key=%s"
               "&external_source=imdb_id", id, chave);
      corpo = rede_baixar(url, 15);
      if (corpo) {
        const char *v = js_array(corpo, NULL, "movie_results");
        if (v) idFilme = (long)js_num(v, js_fim(v), "id", 0.0);
        free(corpo);
      }
    }
    if (chave && chave[0] && idFilme > 0) {
      snprintf(url, sizeof url, "%s/movie/%ld?api_key=%s&language=pt-BR",
               "https://api.themoviedb.org/3", idFilme, chave);
      corpo = rede_baixar(url, 15);
      if (corpo) {
        const char *b = strstr(corpo, "\"belongs_to_collection\"");
        if (b) {
          const char *o = strchr(b, '{');
          if (o) { const char *of = js_fim(o);
                   idCol = (long)js_num(o, of, "id", 0.0);
                   js_texto(o, of, "name", nome, sizeof nome); }
        }
        free(corpo);
      }
    }
    if (idCol > 0) {
      snprintf(url, sizeof url, "%s/collection/%ld?api_key=%s&language=pt-BR",
               "https://api.themoviedb.org/3", idCol, chave);
      corpo = rede_baixar(url, 15);
      if (corpo) {
        struct { char t[120], a[8]; long id; } ach[EX_COL_MAX];
        int nc = 0;
        const char *p = js_array(corpo, NULL, "parts");
        while (p && nc < EX_COL_MAX) {
          const char *f = js_fim(p);
          char data[16] = "";
          ach[nc].t[0] = ach[nc].a[0] = 0;
          js_texto(p, f, "title", ach[nc].t, sizeof ach[nc].t);
          js_texto(p, f, "release_date", data, sizeof data);
          if (strlen(data) >= 4) { memcpy(ach[nc].a, data, 4); ach[nc].a[4] = 0; }
          ach[nc].id = (long)js_num(p, f, "id", 0.0);
          if (ach[nc].t[0] && ach[nc].id > 0) nc++;
          p = js_prox(f);
        }
        free(corpo);
        pthread_mutex_lock(&trava);
        if (!strcmp(id, idPedido)) {
          int k;
          snprintf(colNome, sizeof colNome, "%s", nome);
          for (k = 0; k < nc; k++) {
            snprintf(col[k].titulo, sizeof col[k].titulo, "%s", ach[k].t);
            snprintf(col[k].ano, sizeof col[k].ano, "%s", ach[k].a);
            col[k].tmdb = ach[k].id;
          }
          nCol = nc;
        }
        pthread_mutex_unlock(&trava);
      }
    }
  }

  // --- relacionados ---
  snprintf(url, sizeof url,
           "https://api.trakt.tv/%s/%s/related?limit=%d&extended=images",
           tipo, id, EX_REL_MAX);
  corpo = rede_baixar_com(url, 15, cab);
  if (corpo) {
    struct { char t[120], a[8], i[16], po[200]; } achado[EX_REL_MAX];
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
      // Procurar "poster" no item inteiro pega o campo ERRADO: o Trakt manda
      // `"colors":{"poster":["#D8D5CB",...]}` ANTES de
      // `"images":{"poster":[...]}`, e o log da primeira versao mostrou
      // `poster=https://#D8D5CB` — a cor media da arte, nao a arte. A busca
      // comeca dentro do objeto `images`.
      { const char *img = strstr(p, "\"images\"");
        const char *v = (img && img < f) ? js_array(img, f, "poster") : NULL;
        achado[n].po[0] = 0;
        if (v && *v == '"') {
          const char *e = strchr(v + 1, '"');
          size_t k = e ? (size_t)(e - v - 1) : 0;
          // O Trakt devolve o caminho SEM esquema ("media.trakt.tv/..."); sem o
          // https o cache de textura trata como arquivo local e nao acha nada.
          if (k > 0 && k + 9 < sizeof achado[n].po) {
            memcpy(achado[n].po, "https://", 8);
            memcpy(achado[n].po + 8, v + 1, k);
            achado[n].po[8 + k] = 0;
          }
        } }
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
        snprintf(rel[k].poster, sizeof rel[k].poster, "%s", achado[k].po);
      }
      nRel = n;
    }
    pthread_mutex_unlock(&trava);
  }

  { int k, q = 0;
    for (k = 0; k < EX_NFONTES; k++) if (notas[k]) q++;
    printf("[extras] %s -> notas=%d/%d coment=%d rel=%d temps=%d\n", id, q,
           EX_NFONTES, nComent, nRel, nTemps); }
  printf("[extras] colecao \"%s\" -> %d | rel[0] poster=%s\n", colNome, nCol,
         nRel ? rel[0].poster : "(sem)"); fflush(stdout);
  fflush(stdout);
  pthread_mutex_lock(&trava);
  fioVivo = 0;
  pthread_mutex_unlock(&trava);
  return NULL;
}

void extras_pedir(const char *imdb, int serie, long tmdbId) {
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
  notaTrakt = votosTrakt = nComent = nRel = nTemps = nCol = 0;
  colNome[0] = 0;
  memset(vistos, 0, sizeof vistos);
  memset(notas, 0, sizeof notas);
  if (fioVivo) { pthread_mutex_unlock(&trava); return; }
  snprintf(idEmCurso, sizeof idEmCurso, "%s", imdb);
  serieEmCurso = serie;
  tmdbEmCurso = tmdbId;
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

const char *extras_colecao_nome(void) { return colNome; }
int extras_n_colecao(void) { return nCol; }
const char *extras_colecao_titulo(int i) {
  return (i >= 0 && i < nCol) ? col[i].titulo : "";
}
const char *extras_colecao_ano(int i) {
  return (i >= 0 && i < nCol) ? col[i].ano : "";
}
long extras_colecao_tmdb(int i) { return (i >= 0 && i < nCol) ? col[i].tmdb : 0; }

int extras_n_temporadas(void) { return nTemps; }
int extras_temporada_numero(int t) {
  return (t >= 0 && t < nTemps) ? temps[t].numero : 0;
}
int extras_n_eps(int t) { return (t >= 0 && t < nTemps) ? temps[t].nEps : 0; }
int extras_ep_numero(int t, int i) {
  return (t >= 0 && t < nTemps && i >= 0 && i < temps[t].nEps) ? temps[t].eps[i].ep : 0;
}
int extras_ep_nota(int t, int i) {
  return (t >= 0 && t < nTemps && i >= 0 && i < temps[t].nEps) ? temps[t].eps[i].nota : 0;
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
const char *extras_relacionado_poster(int i) {
  return (i >= 0 && i < nRel) ? rel[i].poster : "";
}

int extras_ep_visto(int temporada, int episodio) {
  if (temporada < 0 || temporada >= EX_VIS_T) return 0;
  if (episodio < 0 || episodio >= EX_VIS_E) return 0;
  return vistos[temporada][episodio];
}
