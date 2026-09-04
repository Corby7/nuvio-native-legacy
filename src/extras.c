#include "extras.h"
#include "trakt.h"
#include "net.h"
#include "js.h"
#include "discover.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int  scoreTrakt, votesTrakt;
static int  scores[EX_NSOURCES];
static char mdbKey[80];
static char dirArtEx[512];

// Nome do provedor na api do mdbList E nome do arquivo de marca em art/marcas.
// A ordem e a do enum, que e a do renderExternalRatingsRow do web.
static const char *SOURCE[EX_NSOURCES] = {
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
static int inTenths(double v) {
  int n = (int)(v * 10.0 + 0.5);
  if (n < 0) n = 0;
  if (n > 1000) n = 1000;
  return n;
}

// 1 quando a nota da fonte e uma PORCENTAGEM; 0 quando e nota de 0 a 10.
int extras_source_percentual(int source) {
  return source != EX_IMDB && source != EX_LETTERBOXD;
}

void extras_set_key(const char *key) {
  if (!key || !*key) return;
  snprintf(mdbKey, sizeof mdbKey, "%s", key);
  printf("[extras] mdblist: key from the account\n");
  fflush(stdout);
}

void extras_load(const char *dirArt) {
  char path[600];
  FILE *f;
  snprintf(dirArtEx, sizeof dirArtEx, "%s", dirArt ? dirArt : ".");
  snprintf(path, sizeof path, "%s/mdblist.txt", dirArt ? dirArt : ".");
  f = fopen(path, "r");
  if (!f) { printf("[extras] mdblist missing\n"); fflush(stdout); return; }
  if (fgets(mdbKey, sizeof mdbKey, f)) {
    char *end = mdbKey + strlen(mdbKey);
    while (end > mdbKey && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
  }
  fclose(f);
  printf("[extras] mdblist %s\n", mdbKey[0] ? "ok" : "empty"); fflush(stdout);
}

int extras_score(int source) {
  return (source >= 0 && source < EX_NSOURCES) ? scores[source] : 0;
}
const char *extras_source_brand(int source) {
  return (source >= 0 && source < EX_NSOURCES) ? SOURCE[source] : "";
}

const char *extras_path_brand(int source) {
  // ABSOLUTO. O cache de textura chama IMG_Load com o caminho como veio, e o
  // diretorio de trabalho do app nao e a pasta da arte — com "marcas/x.png"
  // relativo o arquivo simplesmente nao era achado e o cartao saia sem logo,
  // sem erro nenhum. O catalogo ja faz assim (catalogo.c:79).
  static char cam[600];
  if (source < 0 || source >= EX_NSOURCES) return "";
  snprintf(cam, sizeof cam, "%s/brands/%s.png", dirArtEx, SOURCE[source]);
  return cam;
}

// Caminho de uma marca que NAO e fonte de nota — o wordmark do Trakt, hoje.
// Existe pelo mesmo motivo absoluto de cima: caminho relativo faz o IMG_Load
// falhar em silencio, e o desenho some sem erro nenhum.
const char *extras_path_brand_name(const char *name) {
  static char cam[600];
  if (!name || !name[0]) return "";
  snprintf(cam, sizeof cam, "%s/brands/%s.png", dirArtEx, name);
  return cam;
}
// `nota` e o user_rating do Trakt (0..10); 0 quando quem comentou nao avaliou.
// A referencia mostra "10/10  17 curtidas" no rodape do cartao, e sem a nota o
// rodape ficava so com o numero de curtidas — metade da informacao.
static struct { char user[40]; char text[420]; int likes; int score; } comment[EX_COMMENT_MAX];

// COMENTARIOS DO EPISODIO, o outro lado do seletor "Série | Episódio" que a
// referencia poe acima dos cartoes. Sao uma consulta DIFERENTE
// (/shows/<id>/seasons/<t>/episodes/<e>/comments/likes), nao um filtro da lista
// da serie: o Trakt guarda as duas separadas, e comentario de episodio nunca
// aparece na lista da serie.
//
// Vem sob demanda — so quando o dono escolhe "Episódio" —, porque o custo e uma
// viagem por episodio e a maioria das visitas nunca troca de aba.
static struct { char user[40]; char text[420]; int likes; int score; } commentEp[EX_COMMENT_MAX];
static int  nCommentEp;
static int  epTempCurrent, epNumCurrent;    // de que episodio a lista acima e
static int  epThreadAlive;
static char epShow[24];
static int  epReqTemp, epReqNum;
static int  nComment;
static struct { char title[120], year[8], imdb[16], poster[200]; } rel[EX_REL_MAX];
// Vistos: um bit por episodio, ate 40 episodios em 20 temporadas. Vetor fixo
// porque a consulta acontece no DESENHO de cada card, a cada quadro — uma
// busca em lista ali custaria mais que a resposta.
#define EX_VIS_T 20
#define EX_VIS_E 40
static unsigned char watched[EX_VIS_T][EX_VIS_E];
static int progressReady, nextT, nextE;
static int  nRel;
static struct { int number; int nEps; struct { int ep, score; } eps[EX_EP_MAX]; }
            seasons[EX_TEMP_MAX];
static int  nSeasons;
static char colName[80];
// Ficha tecnica e trailers: mesma viagem /movie/<id> da colecao.
static char profileStatus[32], profileCountries[160], profileCert[12], profileRelease[16];
static int  profileDuration;
static struct { char yt[16], name[80], mini[80]; } trailer[EX_TRAILER_MAX];
static int  nTrailer;
static struct { char title[120], year[8]; long tmdb; } col[EX_COL_MAX];
static int  nCol;
static long tmdbInProgress;

static char idRequest[24], idInProgress[24];
static int  seriesInProgress, seriesRequest, threadAlive;
static long tmdbRequest;
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *fetch(void *arg);

static int requestStillCurrent(const char *id) {
  int current;
  pthread_mutex_lock(&lock);
  current = !strcmp(id, idRequest);
  pthread_mutex_unlock(&lock);
  return current;
}

// Termina um pedido e, se o usuario abriu outro titulo durante a consulta,
// inicia imediatamente o pedido mais recente. Antes idPedido era trocado mas
// nenhum novo fio nascia: a tela seguinte permanecia vazia indefinidamente.
static void finishSearch(const char *id) {
  int resume = 0;
  pthread_mutex_lock(&lock);
  if (strcmp(idRequest, id)) {
    snprintf(idInProgress, sizeof idInProgress, "%s", idRequest);
    seriesInProgress = seriesRequest;
    tmdbInProgress = tmdbRequest;
    resume = 1;
  } else {
    threadAlive = 0;
  }
  pthread_mutex_unlock(&lock);
  if (resume) {
    if (pthread_create(&thread, NULL, fetch, NULL) != 0) {
      pthread_mutex_lock(&lock); threadAlive = 0; pthread_mutex_unlock(&lock);
    } else pthread_detach(thread);
  }
}

// O Trakt devolve a nota como fracao de 0 a 10 com casas ("7.83521"); o resto
// do app guarda nota em 0..100 inteiro, como o campo `nota` do catalogo.
static int to100(double v) {
  int n = (int)(v * 10.0 + 0.5);
  if (n < 0) n = 0;
  if (n > 100) n = 100;
  return n;
}

// Um comentario pode ter quebras de linha e aspas escapadas; o desenho e de uma
// caixa de texto corrida, entao troca-se tudo por espaco. js_texto ja resolve o
// escape de aspas e transforma \\uXXXX em espaco.
static void numaLine(char *s) {
  for (; *s; s++) if (*s == '\n' || *s == '\r' || *s == '\t') *s = ' ';
}

static void *fetch(void *arg) {
  const char *header[4];
  char auth[200], key[140], url[200], id[24];
  const char *kind;
  char *body;
  int series;
  long tmdbId;
  (void)arg;

  pthread_mutex_lock(&lock);
  snprintf(id, sizeof id, "%s", idInProgress);
  series = seriesInProgress;
  tmdbId = tmdbInProgress;
  kind = series ? "shows" : "movies";
  pthread_mutex_unlock(&lock);

  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) {
    finishSearch(id);
    return NULL;
  }

  // O QUE JA FOI VISTO VEM PRIMEIRO.
  //
  // Estava por ULTIMO, depois de ratings, comentarios e de um
  // `seasons?extended=episodes,full` que traz a serie inteira — quatro viagens
  // antes de a marca de assistido aparecer no card do episodio. E ela e o dado
  // mais visivel da tela e o que decide o rotulo do botao primario
  // ("Retomar"/"Próximo"), entao era exatamente o ultimo a chegar e o primeiro
  // que o dono nota faltando.
  // --- episodios ja assistidos (so serie) ---
  if (series) {
    snprintf(url, sizeof url,
             "https://api.trakt.tv/shows/%s/progress/watched", id);
    body = net_download_com(url, 20, header);
    if (body) {
      unsigned char new[EX_VIS_T][EX_VIS_E];
      const char *p = js_array(body, NULL, "seasons");
      memset(new, 0, sizeof new);
      while (p) {
        const char *f = js_end(p);
        int t = (int)js_num(p, f, "number", -1.0);
        if (t >= 0 && t < EX_VIS_T) {
          const char *q = js_array(p, f, "episodes");
          while (q) {
            const char *qf = js_end(q);
            int en = (int)js_num(q, qf, "number", -1.0);
            // "completed" e booleano; js_num nao le true/false, entao a leitura
            // e pelo texto — foi assim que a primeira versao marcou tudo como
            // nao visto sem erro nenhum.
            const char *c = strstr(q, "\"completed\"");
            int watched = 0;
            if (c && c < qf) { const char *v = c + 12;
                               while (*v == ' ' || *v == ':') v++;
                               watched = (*v == 't'); }
            if (watched && en > 0 && en < EX_VIS_E) new[t][en] = 1;
            q = js_next(qf);
          }
        }
        p = js_next(f);
      }
      int pt = 0, pe = 0;
      const char *next = strstr(body, "\"next_episode\"");
      if (next && (next = strchr(next, ':'))) {
        next++;
        while (*next == ' ' || *next == '\n' || *next == '\r' || *next == '\t') next++;
        if (*next == '{') {
          const char *end = js_end(next);
          pt = (int)js_num(next, end, "season", 0);
          pe = (int)js_num(next, end, "number", 0);
        }
      }
      int valid = strstr(body, "\"seasons\"") != NULL;
      free(body);
      pthread_mutex_lock(&lock);
      if (!strcmp(id, idRequest) && valid) {
        memcpy(watched, new, sizeof watched);
        nextT = pt; nextE = pe; progressReady = 1;
      }
      pthread_mutex_unlock(&lock);
    }
  }

  // O usuario ja abriu outro titulo. Nao gastar varias viagens opcionais com
  // uma tela que nao existe mais; entrega o fio ao pedido pendente.
  if (!requestStillCurrent(id)) { finishSearch(id); return NULL; }


  // --- nota ---
  snprintf(url, sizeof url, "https://api.trakt.tv/%s/%s/ratings", kind, id);
  body = net_download_com(url, 12, header);
  if (body) {
    int n = to100(js_num(body, NULL, "rating", 0.0));
    int v = (int)js_num(body, NULL, "votes", 0.0);
    free(body);
    pthread_mutex_lock(&lock);
    if (!strcmp(id, idRequest)) {
      scoreTrakt = n; votesTrakt = v;
      // Sem chave do mdbList esta e a UNICA nota do Trakt que teremos; com
      // chave, o passo seguinte sobrescreve com a que o mdbList devolver, que
      // e a mesma fonte que o web mostra.
      // notaTrakt esta em 0..100 (a api do Trakt devolve 0..10). No vetor a
      // escala e "cru x 10" e a fonte trakt e percentual, entao 6.7 -> 67% ->
      // 670. Sem esta conversao o cartao mostrava 6.7% quando nao havia
      // mdbList.
      if (!scores[EX_TRAKT]) scores[EX_TRAKT] = n * 10;
    }
    pthread_mutex_unlock(&lock);
  }

  // --- notas do mdbList, se o dono tiver chave ---
  if (series && requestStillCurrent(id)) {
    snprintf(url,sizeof url,"https://api.trakt.tv/shows/%s?extended=full",id);
    body=net_download_com(url,8,header);
    if(body) {
      char state[32]="";
      js_text(body,NULL,"status",state,sizeof state);
      free(body);
      pthread_mutex_lock(&lock);
      if(!strcmp(id,idRequest)) snprintf(profileStatus,sizeof profileStatus,"%s",state);
      pthread_mutex_unlock(&lock);
    }
  }

  //
  // Um POST por provedor, como o web faz (fetchProviderRating): a api aceita
  // "ids" em lote mas so um provedor por chamada. Sao sete chamadas curtas; o
  // fio ja e proprio, entao nao atrapalha o desenho.
  if (mdbKey[0]) {
    const char *headerJ[3];
    char kj[64];
    char bodyPost[80];
    int k;
    snprintf(kj, sizeof kj, "content-type: application/json");
    headerJ[0] = kj; headerJ[1] = NULL; headerJ[2] = NULL;
    snprintf(bodyPost, sizeof bodyPost,
             "{\"ids\":[\"%s\"],\"provider\":\"imdb\"}", id);
    for (k = 0; k < EX_NSOURCES; k++) {
      char u[300], *rp;
      snprintf(u, sizeof u, "https://api.mdblist.com/rating/%s/%s?apikey=%s",
               series ? "show" : "movie", SOURCE[k], mdbKey);
      rp = net_post(u, 12, headerJ, bodyPost);
      if (!rp) continue;
      { double v = js_num(rp, NULL, "rating", -1.0);
        free(rp);
        if (v >= 0.0) {
          int c = inTenths(v);
          pthread_mutex_lock(&lock);
          if (!strcmp(id, idRequest)) scores[k] = c;
          pthread_mutex_unlock(&lock);
        } }
    }
  }

  // --- comentarios, os mais curtidos primeiro ---
  snprintf(url, sizeof url,
           "https://api.trakt.tv/%s/%s/comments/likes?limit=%d", kind, id,
           EX_COMMENT_MAX);
  body = net_download_com(url, 12, header);
  if (body) {
    struct { char u[40]; char t[420]; int c; int score; } found[EX_COMMENT_MAX];
    int n = 0;
    // p+1 e nao js_prox: js_prox recebe o FIM do elemento anterior, e aqui
    // ainda nao ha anterior. Com js_prox o primeiro item era pulado e, em
    // resposta de tres itens, sobrava lixo — as duas listas vinham vazias.
    const char *p = strchr(body, '[');
    p = p ? p + 1 : NULL;
    while (p && n < EX_COMMENT_MAX) {
      const char *f = js_end(p);
      found[n].u[0] = found[n].t[0] = 0;
      js_text(p, f, "comment", found[n].t, sizeof found[n].t);
      // "username" esta dentro do objeto `user`; js_texto varre a faixa toda e
      // a unica ocorrencia dessa chave no item e essa.
      js_text(p, f, "username", found[n].u, sizeof found[n].u);
      found[n].c = (int)js_num(p, f, "likes", 0.0);
      found[n].score = (int)js_num(p, f, "user_rating", 0.0);
      numaLine(found[n].t);
      if (found[n].t[0]) n++;
      p = js_next(f);
    }
    free(body);
    pthread_mutex_lock(&lock);
    if (!strcmp(id, idRequest)) {
      int k;
      for (k = 0; k < n; k++) {
        snprintf(comment[k].user, sizeof comment[k].user, "%s", found[k].u);
        snprintf(comment[k].text, sizeof comment[k].text, "%s", found[k].t);
        comment[k].likes = found[k].c;
        comment[k].score = found[k].score;
      }
      nComment = n;
    }
    pthread_mutex_unlock(&lock);
  }

  // --- notas por episodio, so em serie ---
  if (series) {
    snprintf(url, sizeof url,
             "https://api.trakt.tv/shows/%s/seasons?extended=episodes,full", id);
    body = net_download_com(url, 20, header);
    if (body) {
      int nt = 0;
      const char *p = strchr(body, '[');
      p = p ? p + 1 : NULL;
      while (p && nt < EX_TEMP_MAX) {
        const char *f = js_end(p);
        int num = (int)js_num(p, f, "number", -1.0);
        // Temporada 0 e "especiais"; o web filtra `value > 0`.
        if (num > 0) {
          const char *q = js_array(p, f, "episodes");
          int ne = 0;
          while (q && ne < EX_EP_MAX) {
            const char *qf = js_end(q);
            int en = (int)js_num(q, qf, "number", -1.0);
            double r = js_num(q, qf, "rating", 0.0);
            if (en > 0) {
              seasons[nt].eps[ne].ep = en;
              seasons[nt].eps[ne].score = (int)(r * 10.0 + 0.5);
              ne++;
            }
            q = js_next(qf);
          }
          if (ne > 0) { seasons[nt].number = num; seasons[nt].nEps = ne; nt++; }
        }
        p = js_next(f);
      }
      free(body);
      pthread_mutex_lock(&lock);
      if (!strcmp(id, idRequest)) nSeasons = nt;
      pthread_mutex_unlock(&lock);
    }
  }

  // --- colecao (so filme, e so quando ja sabemos o id do TMDB) ---
  if (!series) {
    const char *key = disc_key_tmdb();
    long idCol = 0, idMovie = tmdbId;
    char name[80] = "";
    // O id do TMDB so fica no catalogo DEPOIS do enriquecimento do elenco; na
    // PRIMEIRA abertura de um titulo ele ainda e 0, e a aba nao apareceria
    // justamente na visita em que o dono esta olhando. /find resolve na hora.
    if (key && key[0] && idMovie <= 0) {
      snprintf(url, sizeof url,
               "https://api.themoviedb.org/3/find/%s?api_key=%s"
               "&external_source=imdb_id", id, key);
      body = net_download(url, 15);
      if (body) {
        const char *v = js_array(body, NULL, "movie_results");
        if (v) idMovie = (long)js_num(v, js_end(v), "id", 0.0);
        free(body);
      }
    }
    if (key && key[0] && idMovie > 0) {
      // `append_to_response` faz o TMDB devolver release_dates e videos DENTRO
      // deste mesmo corpo. Antes esta chamada ja acontecia e o parse lia so
      // belongs_to_collection: status, runtime, release_date e os paises
      // chegavam e eram descartados. Agora a ficha inteira e os trailers saem
      // daqui, sem nenhuma viagem a mais.
      snprintf(url, sizeof url,
               "%s/movie/%ld?api_key=%s&language=pt-BR"
               "&append_to_response=release_dates,videos",
               "https://api.themoviedb.org/3", idMovie, key);
      body = net_download(url, 15);
      if (body) {
        // A ficha abaixo escreve varios campos globais. Segura a mesma trava
        // usada por extras_pedir para que uma troca de titulo nao limpe os
        // campos no meio do parse e receba, logo depois, metade da ficha velha.
        pthread_mutex_lock(&lock);
        if (strcmp(id, idRequest)) {
          pthread_mutex_unlock(&lock);
          free(body);
          finishSearch(id);
          return NULL;
        }
        const char *endC = body + strlen(body);
        const char *b = strstr(body, "\"belongs_to_collection\"");
        if (b) {
          const char *o = strchr(b, '{');
          if (o) { const char *of = js_end(o);
                   idCol = (long)js_num(o, of, "id", 0.0);
                   js_text(o, of, "name", name, sizeof name); }
        }

        // --- ficha tecnica ---
        js_text(body, endC, "status", profileStatus, sizeof profileStatus);
        js_text(body, endC, "release_date", profileRelease, sizeof profileRelease);
        profileDuration = (int)js_num(body, endC, "runtime", 0.0);

        // production_countries e um array de objetos; junta os nomes com
        // virgula, como a referencia mostra ("United States of America,
        // Canada"). Para de acrescentar quando o campo enche, em vez de cortar
        // um nome pela metade.
        { const char *p2 = js_array(body, endC, "production_countries");
          profileCountries[0] = 0;
          while (p2) {
            char pn[80] = "";
            const char *pf = js_end(p2);
            js_text(p2, pf, "name", pn, sizeof pn);
            if (pn[0]) {
              size_t used = strlen(profileCountries);
              size_t fits  = sizeof profileCountries - used;
              size_t wants  = strlen(pn) + (used ? 2 : 0) + 1;
              if (wants > fits) break;
              snprintf(profileCountries + used, fits, "%s%s", used ? ", " : "", pn);
            }
            p2 = js_next(pf);
          } }

        // Classificacao etaria: release_dates.results[] tem um bloco por pais,
        // e cada bloco tem release_dates[] com `certification`. Preferimos BR;
        // na falta, US; na falta das duas, a primeira nao-vazia que aparecer.
        // Muitos paises trazem a chave com string VAZIA, e aceitar a primeira
        // ocorrencia sem olhar o conteudo enchia o selo de nada.
        { const char *res = js_array(body, endC, "results");
          char br[12] = "", us[12] = "", qq[12] = "";
          while (res) {
            const char *rf = js_end(res);
            char pais[8] = "", c[12] = "";
            js_text(res, rf, "iso_3166_1", pais, sizeof pais);
            { const char *d = js_array(res, rf, "release_dates");
              while (d && !c[0]) {
                const char *df = js_end(d);
                js_text(d, df, "certification", c, sizeof c);
                d = js_next(df);
              } }
            if (c[0]) {
              if      (!strcmp(pais, "BR")) snprintf(br, sizeof br, "%s", c);
              else if (!strcmp(pais, "US")) snprintf(us, sizeof us, "%s", c);
              else if (!qq[0])              snprintf(qq, sizeof qq, "%s", c);
            }
            res = js_next(rf);
          }
          snprintf(profileCert, sizeof profileCert, "%s",
                   br[0] ? br : us[0] ? us : qq); }

        // Trailers: videos.results[]. So YouTube (o unico host cuja miniatura
        // e obtivel por URL previsivel) e so o que for Trailer ou Teaser — o
        // TMDB mistura ali featurette, clipe e cena de bastidor.
        { const char *v = js_array(body, endC, "results");
          // `results` aparece duas vezes no corpo (release_dates e videos);
          // procura a partir do bloco de videos para nao pegar o errado.
          const char *vid = strstr(body, "\"videos\"");
          if (vid) v = js_array(vid, endC, "results");
          while (v && nTrailer < EX_TRAILER_MAX) {
            const char *vf = js_end(v);
            char site[24] = "", kind[24] = "", key[16] = "", nm[80] = "";
            js_text(v, vf, "site", site, sizeof site);
            js_text(v, vf, "type", kind, sizeof kind);
            js_text(v, vf, "key",  key,  sizeof key);
            js_text(v, vf, "name", nm,   sizeof nm);
            if (key[0] && !strcmp(site, "YouTube") &&
                (!strcmp(kind, "Trailer") || !strcmp(kind, "Teaser"))) {
              int k = nTrailer++;
              snprintf(trailer[k].yt,   sizeof trailer[k].yt,   "%s", key);
              snprintf(trailer[k].name, sizeof trailer[k].name, "%s",
                       nm[0] ? nm : "Trailer");
              snprintf(trailer[k].mini, sizeof trailer[k].mini,
                       "https://img.youtube.com/vi/%s/hqdefault.jpg", key);
            }
            v = js_next(vf);
          } }

        pthread_mutex_unlock(&lock);
        free(body);
      }
    }
    if (idCol > 0) {
      snprintf(url, sizeof url, "%s/collection/%ld?api_key=%s&language=pt-BR",
               "https://api.themoviedb.org/3", idCol, key);
      body = net_download(url, 15);
      if (body) {
        struct { char t[120], a[8]; long id; } ach[EX_COL_MAX];
        int nc = 0;
        const char *p = js_array(body, NULL, "parts");
        while (p && nc < EX_COL_MAX) {
          const char *f = js_end(p);
          char date[16] = "";
          ach[nc].t[0] = ach[nc].a[0] = 0;
          js_text(p, f, "title", ach[nc].t, sizeof ach[nc].t);
          js_text(p, f, "release_date", date, sizeof date);
          if (strlen(date) >= 4) { memcpy(ach[nc].a, date, 4); ach[nc].a[4] = 0; }
          ach[nc].id = (long)js_num(p, f, "id", 0.0);
          if (ach[nc].t[0] && ach[nc].id > 0) nc++;
          p = js_next(f);
        }
        free(body);
        pthread_mutex_lock(&lock);
        if (!strcmp(id, idRequest)) {
          int k;
          snprintf(colName, sizeof colName, "%s", name);
          for (k = 0; k < nc; k++) {
            snprintf(col[k].title, sizeof col[k].title, "%s", ach[k].t);
            snprintf(col[k].year, sizeof col[k].year, "%s", ach[k].a);
            col[k].tmdb = ach[k].id;
          }
          nCol = nc;
        }
        pthread_mutex_unlock(&lock);
      }
    }
  }

  // Relacionados sao opcionais e podem custar mais uma viagem. Se o usuario
  // ja abriu outra obra, encadeia a mais recente agora em vez de prolongar a
  // espera com dados que serao descartados.
  if (!requestStillCurrent(id)) { finishSearch(id); return NULL; }

  // --- relacionados ---
  snprintf(url, sizeof url,
           "https://api.trakt.tv/%s/%s/related?limit=%d&extended=images",
           kind, id, EX_REL_MAX);
  body = net_download_com(url, 15, header);
  if (body) {
    struct { char t[120], a[8], i[16], po[200]; } found[EX_REL_MAX];
    int n = 0;
    // p+1 e nao js_prox: js_prox recebe o FIM do elemento anterior, e aqui
    // ainda nao ha anterior. Com js_prox o primeiro item era pulado e, em
    // resposta de tres itens, sobrava lixo — as duas listas vinham vazias.
    const char *p = strchr(body, '[');
    p = p ? p + 1 : NULL;
    while (p && n < EX_REL_MAX) {
      const char *f = js_end(p);
      double year;
      found[n].t[0] = found[n].i[0] = 0;
      js_text(p, f, "title", found[n].t, sizeof found[n].t);
      js_text(p, f, "imdb", found[n].i, sizeof found[n].i);
      // Procurar "poster" no item inteiro pega o campo ERRADO: o Trakt manda
      // `"colors":{"poster":["#D8D5CB",...]}` ANTES de
      // `"images":{"poster":[...]}`, e o log da primeira versao mostrou
      // `poster=https://#D8D5CB` — a cor media da arte, nao a arte. A busca
      // comeca dentro do objeto `images`.
      { const char *img = strstr(p, "\"images\"");
        const char *v = (img && img < f) ? js_array(img, f, "poster") : NULL;
        found[n].po[0] = 0;
        if (v && *v == '"') {
          const char *e = strchr(v + 1, '"');
          size_t k = e ? (size_t)(e - v - 1) : 0;
          // O Trakt devolve o caminho SEM esquema ("media.trakt.tv/..."); sem o
          // https o cache de textura trata como arquivo local e nao acha nada.
          if (k > 0 && k + 9 < sizeof found[n].po) {
            memcpy(found[n].po, "https://", 8);
            memcpy(found[n].po + 8, v + 1, k);
            found[n].po[8 + k] = 0;
          }
        } }
      year = js_num(p, f, "year", 0.0);
      if (year > 1800.0) snprintf(found[n].a, sizeof found[n].a, "%d", (int)year);
      else found[n].a[0] = 0;
      if (found[n].t[0] && found[n].i[0]) n++;
      p = js_next(f);
    }
    free(body);
    pthread_mutex_lock(&lock);
    if (!strcmp(id, idRequest)) {
      int k;
      for (k = 0; k < n; k++) {
        snprintf(rel[k].title, sizeof rel[k].title, "%s", found[k].t);
        snprintf(rel[k].year, sizeof rel[k].year, "%s", found[k].a);
        snprintf(rel[k].imdb, sizeof rel[k].imdb, "%s", found[k].i);
        snprintf(rel[k].poster, sizeof rel[k].poster, "%s", found[k].po);
      }
      nRel = n;
    }
    pthread_mutex_unlock(&lock);
  }

  { int k, q = 0;
    for (k = 0; k < EX_NSOURCES; k++) if (scores[k]) q++;
    printf("[extras] %s -> scores=%d/%d comments=%d rel=%d seasons=%d\n", id, q,
           EX_NSOURCES, nComment, nRel, nSeasons); }
  printf("[extras] collection \"%s\" -> %d | rel[0] poster=%s\n", colName, nCol,
         nRel ? rel[0].poster : "(none)"); fflush(stdout);
  fflush(stdout);
  finishSearch(id);
  return NULL;
}

void extras_request(const char *imdb, int series, long tmdbId) {
  char id[24];
  const char *dp;
  if (!imdb || imdb[0] != 't' || !trakt_active()) return;
  // O campo do catalogo pode vir com episodio ("tt9737326:2:1"), que e o
  // formato que os addons de fonte usam. O Trakt so conhece o id do TITULO —
  // com o sufixo ele responde 404 e as tres abas ficavam vazias em toda serie.
  dp = strchr(imdb, ':');
  if (dp) { size_t n = (size_t)(dp - imdb);
            if (n >= sizeof id) n = sizeof id - 1;
            memcpy(id, imdb, n); id[n] = 0; }
  else snprintf(id, sizeof id, "%s", imdb);
  imdb = id;
  pthread_mutex_lock(&lock);
  if (!strcmp(idRequest, imdb)) { pthread_mutex_unlock(&lock); return; }
  snprintf(idRequest, sizeof idRequest, "%s", imdb);
  seriesRequest = series;
  tmdbRequest = tmdbId;
  scoreTrakt = votesTrakt = nComment = nRel = nSeasons = nCol = 0;
  colName[0] = 0;
  nTrailer = profileDuration = 0;
  profileStatus[0] = profileCountries[0] = profileCert[0] = profileRelease[0] = 0;
  memset(watched, 0, sizeof watched);
  progressReady = nextT = nextE = 0;
  memset(scores, 0, sizeof scores);
  if (threadAlive) { pthread_mutex_unlock(&lock); return; }
  snprintf(idInProgress, sizeof idInProgress, "%s", imdb);
  seriesInProgress = series;
  tmdbInProgress = tmdbId;
  threadAlive = 1;
  pthread_mutex_unlock(&lock);
  if (pthread_create(&thread, NULL, fetch, NULL) != 0) threadAlive = 0;
  else pthread_detach(thread);
}

int extras_score_trakt(void)  { return scoreTrakt; }
int extras_votes_trakt(void) { return votesTrakt; }

int extras_n_comments(void) { return nComment; }
const char *extras_comment_user(int i) {
  return (i >= 0 && i < nComment) ? comment[i].user : "";
}
const char *extras_comment_text(int i) {
  return (i >= 0 && i < nComment) ? comment[i].text : "";
}
int extras_comment_likes(int i) {
  return (i >= 0 && i < nComment) ? comment[i].likes : 0;
}

// --- comentarios do EPISODIO --------------------------------------------------

static void *fetchEpComment(void *arg) {
  const char *header[4];
  char auth[200], key[140], url[260], show[24];
  char *body;
  int t, e;
  (void)arg;

  pthread_mutex_lock(&lock);
  snprintf(show, sizeof show, "%s", epShow);
  t = epReqTemp; e = epReqNum;
  pthread_mutex_unlock(&lock);

  if (!trakt_headers(header, auth, sizeof auth, key, sizeof key)) {
    pthread_mutex_lock(&lock); epThreadAlive = 0; pthread_mutex_unlock(&lock);
    return NULL;
  }
  snprintf(url, sizeof url,
           "https://api.trakt.tv/shows/%s/seasons/%d/episodes/%d/comments/likes?limit=%d",
           show, t, e, EX_COMMENT_MAX);
  body = net_download_com(url, 12, header);
  if (body) {
    struct { char u[40]; char t[420]; int c; int score; } found[EX_COMMENT_MAX];
    int n = 0;
    // p+1 e nao js_prox, pelo mesmo motivo da lista da serie: ainda nao ha
    // elemento anterior de onde partir.
    const char *p = strchr(body, '[');
    p = p ? p + 1 : NULL;
    while (p && n < EX_COMMENT_MAX) {
      const char *f = js_end(p);
      found[n].u[0] = found[n].t[0] = 0;
      js_text(p, f, "comment", found[n].t, sizeof found[n].t);
      js_text(p, f, "username", found[n].u, sizeof found[n].u);
      found[n].c = (int)js_num(p, f, "likes", 0.0);
      found[n].score = (int)js_num(p, f, "user_rating", 0.0);
      numaLine(found[n].t);
      if (found[n].t[0]) n++;
      p = js_next(f);
    }
    free(body);
    pthread_mutex_lock(&lock);
    // So publica se o dono ainda esta no mesmo episodio: trocar de episodio
    // enquanto isto volta faria a lista antiga aparecer sob o rotulo novo.
    if (t == epReqTemp && e == epReqNum) {
      int k;
      for (k = 0; k < n; k++) {
        snprintf(commentEp[k].user, sizeof commentEp[k].user, "%s", found[k].u);
        snprintf(commentEp[k].text, sizeof commentEp[k].text, "%s", found[k].t);
        commentEp[k].likes = found[k].c;
        commentEp[k].score = found[k].score;
      }
      nCommentEp = n;
      epTempCurrent = t; epNumCurrent = e;
    }
    pthread_mutex_unlock(&lock);
  }
  pthread_mutex_lock(&lock); epThreadAlive = 0; pthread_mutex_unlock(&lock);
  return NULL;
}

void extras_request_comments_ep(const char *imdbSeries, int season, int episode) {
  pthread_t f;
  if (!imdbSeries || !imdbSeries[0] || season <= 0 || episode <= 0) return;
  pthread_mutex_lock(&lock);
  // Mesmo episodio ja carregado (ou em voo): nao repete a viagem.
  if (epThreadAlive ||
      (season == epTempCurrent && episode == epNumCurrent && nCommentEp > 0)) {
    pthread_mutex_unlock(&lock);
    return;
  }
  // O id pode vir como "tt123:2:4" da lista de episodios; o Trakt quer so a
  // serie.
  { const char *dp;
    snprintf(epShow, sizeof epShow, "%s", imdbSeries);
    dp = strchr(epShow, ':');
    if (dp) *(char *)dp = 0; }
  epReqTemp = season; epReqNum = episode;
  nCommentEp = 0;                 // limpa: a lista velha e de outro episodio
  epTempCurrent = epNumCurrent = 0;
  epThreadAlive = 1;
  pthread_mutex_unlock(&lock);
  if (pthread_create(&f, NULL, fetchEpComment, NULL) != 0) {
    pthread_mutex_lock(&lock); epThreadAlive = 0; pthread_mutex_unlock(&lock);
  } else {
    pthread_detach(f);
  }
}

int extras_n_comments_ep(void) { return nCommentEp; }
int extras_comments_ep_loading(void) { return epThreadAlive; }
const char *extras_comment_ep_user(int i) {
  return (i >= 0 && i < nCommentEp) ? commentEp[i].user : "";
}
const char *extras_comment_ep_text(int i) {
  return (i >= 0 && i < nCommentEp) ? commentEp[i].text : "";
}
int extras_comment_ep_likes(int i) {
  return (i >= 0 && i < nCommentEp) ? commentEp[i].likes : 0;
}
int extras_comment_ep_score(int i) {
  return (i >= 0 && i < nCommentEp) ? commentEp[i].score : 0;
}

int extras_comment_score(int i) {
  return (i >= 0 && i < nComment) ? comment[i].score : 0;
}

const char *extras_profile_status(void)        { return profileStatus; }
int         extras_profile_duration(void)       { return profileDuration; }
const char *extras_profile_countries(void)        { return profileCountries; }
const char *extras_profile_age_rating(void) { return profileCert; }
const char *extras_profile_release(void)    { return profileRelease; }

int extras_n_trailers(void) { return nTrailer; }
const char *extras_trailer_yt(int i) {
  return (i >= 0 && i < nTrailer) ? trailer[i].yt : "";
}
const char *extras_trailer_name(int i) {
  return (i >= 0 && i < nTrailer) ? trailer[i].name : "";
}
const char *extras_trailer_thumb(int i) {
  return (i >= 0 && i < nTrailer) ? trailer[i].mini : "";
}

const char *extras_collection_name(void) { return colName; }
int extras_n_collection(void) { return nCol; }
const char *extras_collection_title(int i) {
  return (i >= 0 && i < nCol) ? col[i].title : "";
}
const char *extras_collection_year(int i) {
  return (i >= 0 && i < nCol) ? col[i].year : "";
}
long extras_collection_tmdb(int i) { return (i >= 0 && i < nCol) ? col[i].tmdb : 0; }

int extras_n_seasons(void) { return nSeasons; }
int extras_season_number(int t) {
  return (t >= 0 && t < nSeasons) ? seasons[t].number : 0;
}
int extras_n_eps(int t) { return (t >= 0 && t < nSeasons) ? seasons[t].nEps : 0; }
int extras_ep_number(int t, int i) {
  return (t >= 0 && t < nSeasons && i >= 0 && i < seasons[t].nEps) ? seasons[t].eps[i].ep : 0;
}
int extras_ep_score(int t, int i) {
  return (t >= 0 && t < nSeasons && i >= 0 && i < seasons[t].nEps) ? seasons[t].eps[i].score : 0;
}

int extras_n_related(void) { return nRel; }
const char *extras_related_title(int i) {
  return (i >= 0 && i < nRel) ? rel[i].title : "";
}
const char *extras_related_year(int i) {
  return (i >= 0 && i < nRel) ? rel[i].year : "";
}
const char *extras_related_imdb(int i) {
  return (i >= 0 && i < nRel) ? rel[i].imdb : "";
}
const char *extras_related_poster(int i) {
  return (i >= 0 && i < nRel) ? rel[i].poster : "";
}

int extras_ep_watched(int season, int episode) {
  if (season < 0 || season >= EX_VIS_T) return 0;
  if (episode < 0 || episode >= EX_VIS_E) return 0;
  pthread_mutex_lock(&lock);
  int v = watched[season][episode];
  pthread_mutex_unlock(&lock);
  return v;
}

int extras_progress_ready(void) {
  pthread_mutex_lock(&lock);
  int ready = progressReady;
  pthread_mutex_unlock(&lock);
  return ready;
}
int extras_next_episode(int *t, int *e) {
  pthread_mutex_lock(&lock);
  int ok = progressReady && nextT > 0 && nextE > 0;
  if (ok) { *t = nextT; *e = nextE; }
  pthread_mutex_unlock(&lock);
  return ok;
}
