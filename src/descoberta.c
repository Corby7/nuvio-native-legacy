#include "descoberta.h"
#include "catalogo.h"
#include "addons.h"
#include "rede.h"
#include "js.h"
#include "trakt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define CINEMETA "https://v3-cinemeta.strem.io"
#define TMDB     "https://api.themoviedb.org/3"

// Chave do TMDB, em art/tmdb.txt. SEGREDO do dono (saiu do dist/nuvio.env.js do
// app web) — nao versionar. Sem ela o elenco continua so com nomes.
static char tmdbChave[64];

void desc_tmdb(const char *dirArte) {
  char caminho[600];
  FILE *f;
  snprintf(caminho, sizeof caminho, "%s/tmdb.txt", dirArte ? dirArte : ".");
  f = fopen(caminho, "r");
  if (!f) return;
  if (fgets(tmdbChave, sizeof tmdbChave, f)) {
    char *fim = tmdbChave + strlen(tmdbChave);
    while (fim > tmdbChave && (fim[-1] == '\n' || fim[-1] == '\r')) *--fim = 0;
  }
  fclose(f);
  printf("[desc] tmdb %s\n", tmdbChave[0] ? "ok" : "ausente");
}

// strstr que NAO passa de `fim`. O objeto da regiao BR termina antes das
// outras regioes na resposta do TMDB; procurar rent/buy no corpo inteiro
// pegaria o provedor de outra regiao quando a BR nao tivesse.
static const char *ate(const char *ini, const char *fim, const char *agulha) {
  size_t n = strlen(agulha);
  for (; ini && ini + n <= fim; ini++)
    if (*ini == agulha[0] && memcmp(ini, agulha, n) == 0) return ini;
  return NULL;
}

// Primeiro provedor do array `chave` dentro de [ini,fim): nome e logo no
// formato w92 do TMDB. flatrate/rent/buy sao arrays de provedores; o primeiro
// e o principal na pratica (o TMDB ordena por relevancia local).
static int provedorEntre(const char *ini, const char *fim, const char *chave,
                         char *nome, size_t nNome, char *logo, size_t nLogo) {
  const char *k = ate(ini, fim, chave);
  const char *item = k ? strchr(k, '{') : NULL;
  if (!item || item >= fim) return 0;
  const char *fi = js_fim(item);
  if (fi > fim) fi = fim;
  char caminho[128] = "";
  if (!js_texto(item, fi, "provider_name", nome, nNome)) return 0;
  if (js_texto(item, fi, "logo_path", caminho, sizeof caminho) && caminho[0] == '/')
    snprintf(logo, nLogo, "https://image.tmdb.org/t/p/w92%s", caminho);
  return 1;
}

// Preenche foto e personagem do elenco. O Cinemeta da so o NOME; o personagem
// e o retrato vem do TMDB, que precisa de duas viagens: achar o id dele pelo
// id do IMDb e so entao pedir os creditos.
static void fotosDoElenco(CatItem *d, const char *imdbSerie, int serie) {
  char url[400], *corpo;
  long idTmdb = 0;
  if (!tmdbChave[0] || d->nElenco < 1) return;
  snprintf(url, sizeof url, "%s/find/%s?api_key=%s&external_source=imdb_id",
           TMDB, imdbSerie, tmdbChave);
  corpo = rede_baixar(url, 20);
  if (!corpo) return;
  { const char *vet = serie ? "tv_results" : "movie_results";
    const char *p = js_array(corpo, NULL, vet);
    if (p) idTmdb = (long)js_num(p, js_fim(p), "id", 0); }
  free(corpo);
  if (!idTmdb) return;

  snprintf(url, sizeof url, "%s/%s/%ld/credits?api_key=%s",
           TMDB, serie ? "tv" : "movie", idTmdb, tmdbChave);
  corpo = rede_baixar(url, 20);
  if (!corpo) return;
  { const char *p = js_array(corpo, NULL, "cast");
    int k = 0;
    (void)0;
    while (p && k < d->nElenco) {
      const char *f = js_fim(p);
      char caminhoFoto[128] = "";
      js_texto(p, f, "character", d->elenco[k].papel, sizeof d->elenco[k].papel);
      if (js_texto(p, f, "profile_path", caminhoFoto, sizeof caminhoFoto) &&
          caminhoFoto[0] == '/')
        snprintf(d->elenco[k].foto, sizeof d->elenco[k].foto,
                 "https://image.tmdb.org/t/p/w185%s", caminhoFoto);
      // O TMDB devolve o elenco na mesma ordem de importancia que o Cinemeta,
      // entao casar por posicao acerta na pratica; casar por nome falharia nos
      // acentos e nos nomes escritos de forma diferente entre as duas bases.
      k++;
      p = js_prox(f);
    } }
  free(corpo);

  // Onde assistir. Os campos provLogo/provNome existiam no CatItem e NUNCA
  // eram preenchidos no caminho dinamico — o selo do streaming ficava vazio em
  // todo titulo. O TMDB responde por regiao; BR e a do dono.
  snprintf(url, sizeof url, "%s/%s/%ld/watch/providers?api_key=%s",
           TMDB, serie ? "tv" : "movie", idTmdb, tmdbChave);
  corpo = rede_baixar(url, 20);
  if (corpo) {
    const char *br = strstr(corpo, "\"BR\"");
    if (br) {
      const char *brObj = strchr(br, '{');
      const char *brFim = brObj ? js_fim(brObj) : NULL;
      if (brObj && brFim && brFim > brObj) {
        // flatrate = incluido na assinatura; rent = aluguel; buy = compra.
        // Se o titulo nao esta em streaming aqui, o selo fica vazio DE
        // PROPOSITO, em vez de anunciar aluguel como se fosse catalogo.
        provedorEntre(brObj, brFim, "\"flatrate\"",
                      d->provNome, sizeof d->provNome,
                      d->provLogo, sizeof d->provLogo);
        provedorEntre(brObj, brFim, "\"rent\"",
                      d->alugNome, sizeof d->alugNome,
                      d->alugLogo, sizeof d->alugLogo);
        provedorEntre(brObj, brFim, "\"buy\"",
                      d->compNome, sizeof d->compNome,
                      d->compLogo, sizeof d->compLogo);
      }
    }
    free(corpo);
  }
}

static int buscando;
static pthread_t fio, fioEp;
static int epItem = -1, epTemp, fioEpVivo;

int desc_buscando(void) { return buscando; }

// Um item do catalogo montado a partir de um meta do Stremio. Devolve 1 se
// deu para aproveitar (precisa de nome e de alguma arte).
static int deMeta(const char *ini, const char *fim, const char *tipo, CatItem *d) {
  char v[900];
  memset(d, 0, sizeof *d);
  if (!js_texto(ini, fim, "name", d->titulo, sizeof d->titulo)) return 0;
  // O poster e o unico obrigatorio: sem ele o card fica um retangulo cinza.
  if (!js_texto(ini, fim, "poster", d->poster, sizeof d->poster)) return 0;
  js_texto(ini, fim, "background", d->backdrop, sizeof d->backdrop);
  js_texto(ini, fim, "logo", d->logo, sizeof d->logo);
  if (!d->backdrop[0]) snprintf(d->backdrop, sizeof d->backdrop, "%s", d->poster);

  if (!js_texto(ini, fim, "imdb_id", d->imdb, sizeof d->imdb))
    js_texto(ini, fim, "id", d->imdb, sizeof d->imdb);
  snprintf(d->tipo, sizeof d->tipo, "%s", tipo);

  { // genero: "Filme · Acao · Drama"
    const char *g = js_array(ini, fim, "genres");
    char g1[48] = "", g2[48] = "";
    if (g) {
      const char *f1 = js_fim(g);
      (void)f1;
      // elementos de texto: copiar direto do array
      { const char *p = g; int k = 0;
        while (p && k < 2) {
          char tmp[48]; size_t n = 0;
          if (*p != '"') break;
          p++;
          while (*p && *p != '"' && n + 1 < sizeof tmp) tmp[n++] = *p++;
          tmp[n] = 0;
          if (k == 0) snprintf(g1, sizeof g1, "%s", tmp);
          else        snprintf(g2, sizeof g2, "%s", tmp);
          k++;
          p++;
          while (*p == ' ') p++;
          if (*p != ',') break;
          p++;
          while (*p == ' ') p++;
        } }
    }
    snprintf(d->genero, sizeof d->genero, "%s%s%s%s%s",
             strcmp(tipo, "series") ? "Filme" : "Programa de TV",
             g1[0] ? "  \xc2\xb7  " : "", g1,
             g2[0] ? "  \xc2\xb7  " : "", g2);
  }
  v[0] = 0;
  js_texto(ini, fim, "releaseInfo", v, sizeof v);
  { char dur[24] = "";
    js_texto(ini, fim, "runtime", dur, sizeof dur);
    // "2024–" vira "2024": o travessao de serie em andamento polui a linha.
    { char *tr = strstr(v, "\xe2\x80\x93"); if (tr) *tr = 0; }
    snprintf(d->meta, sizeof d->meta, "%.20s%s%.20s", v,
             (v[0] && dur[0]) ? "  \xc2\xb7  " : "", dur); }
  js_texto(ini, fim, "description", d->sinopse, sizeof d->sinopse);
  snprintf(d->classificacao, sizeof d->classificacao, "14");
  { double nota = js_num(ini, fim, "imdbRating", 0.0);
    d->nota = (int)(nota * 10.0 + 0.5) / 1; }
  if (d->nota > 99) d->nota /= 10;
  return 1;
}

// Le um catalogo (movie|series) de um addon e acrescenta ao vetor.
static int lerCatalogo(const char *base, const char *tipo, const char *id,
                       CatItem *saida, int max, int quantos) {
  char url[900];
  char *corpo;
  const char *p;
  int n = 0;
  snprintf(url, sizeof url, "%s/catalog/%s/%s.json", base, tipo, id);
  corpo = rede_baixar(url, 25);
  if (!corpo) return 0;
  p = js_array(corpo, NULL, "metas");
  while (p && n < max && n < quantos) {
    const char *f = js_fim(p);
    if (deMeta(p, f, tipo, &saida[n])) n++;
    p = js_prox(f);
  }
  free(corpo);
  return n;
}

static void *montar(void *u) {
  // O lote tambem cresce: era dimensionado por CAT_MAX e por isso herdava o
  // mesmo teto arbitrario.
  int cap = 128;
  CatItem *lote = malloc(sizeof(CatItem) * (size_t)cap);
  int n = 0, i;
  (void)u;
  if (!lote) { buscando = 0; return NULL; }

  // O "continue assistindo" vem PRIMEIRO e do Trakt. A home usa as primeiras
  // posicoes do catalogo nessa fileira, entao a ordem aqui e o que define o
  // que aparece la — e o historico tem de ganhar das recomendacoes.
  n += trakt_continuar(lote, 8);
#define GARANTE(quantos) do { \
    if (n + (quantos) > cap) { \
      int novoCap = cap; \
      CatItem *maior; \
      while (novoCap < n + (quantos)) novoCap *= 2; \
      maior = realloc(lote, sizeof(CatItem) * (size_t)novoCap); \
      if (maior) { lote = maior; cap = novoCap; } \
    } } while (0)

  // Depois o que e do dono (recomendacoes) e so entao o generico: se algum
  // catalogo falhar, o que se perde e a cauda e nao o comeco.
  static const char *PREF[][2] = {
    { "movie",  "recs_movies_for_you" },
    { "series", "recs_series_for_you" },
    { "movie",  "trending_movies" },
    { "series", "trending_series" },
  };
  for (i = 0; i < addons_n(); i++) {
    const char *base = addons_base(i);
    int k;
    if (!addons_tem_catalogo(i)) continue;
    for (k = 0; k < (int)(sizeof PREF / sizeof *PREF); k++) {
      GARANTE(12);
      int got = lerCatalogo(base, PREF[k][0], PREF[k][1],
                            lote + n, cap - n, 10);
      if (got) printf("[desc] %s/%s: %d\n", PREF[k][0], PREF[k][1], got);
      n += got;
    }
  }
  // Watchlist e colecao entram DEPOIS das recomendacoes, e nao antes.
  // A home usa as PRIMEIRAS posicoes do catalogo nas suas fileiras; com as
  // listas do Trakt na frente (e elas passam de 60 itens cada) as fileiras
  // viravam a watchlist inteira e as recomendacoes nunca apareciam. A
  // biblioteca varre o catalogo todo procurando as marcas, entao para ela
  // tanto faz onde estao.
  GARANTE(400);
  n += trakt_lista("watchlist",  lote + n, cap - n);
  GARANTE(400);
  n += trakt_lista("collection", lote + n, cap - n);
#undef GARANTE

  if (n) {
    cat_definir(lote, n);
    printf("[desc] catalogo montado com %d titulos\n", n);
  } else {
    printf("[desc] nada veio da rede; segue o catalogo do pacote\n");
  }
  fflush(stdout);
  free(lote);
  buscando = 0;
  return NULL;
}

void desc_iniciar(void) {
  if (buscando) return;
  buscando = 1;
  if (pthread_create(&fio, NULL, montar, NULL) != 0) buscando = 0;
  else pthread_detach(fio);
}

// --- episodios sob demanda ---------------------------------------------------

static void *buscarEps(void *u) {
  const CatItem *it = cat_item(epItem);
  char url[600], *corpo;
  CatEp *eps;
  int n = 0, alvo = epTemp;
  (void)u;
  if (!it || !it->imdb[0] || strcmp(it->tipo, "series")) { fioEpVivo = 0; return NULL; }
  { char serie[24]; const char *dp;
    snprintf(serie, sizeof serie, "%s", it->imdb);
    dp = strchr(serie, ':');
    if (dp) { if (!alvo) alvo = atoi(dp + 1); *(char *)dp = 0; }
    snprintf(url, sizeof url, "%s/meta/series/%s.json", CINEMETA, serie); }
  if (!alvo) alvo = 1;
  corpo = rede_baixar(url, 25);
  if (!corpo) { fioEpVivo = 0; return NULL; }
  // A MESMA resposta traz elenco, direcao e a lista de temporadas. Buscar de
  // novo para cada uma seria tres viagens ao mesmo lugar.
  {
    CatItem edit = *it;
    const char *c = js_array(corpo, NULL, "cast");
    int k = 0;
    while (c && k < 6) {
      size_t n2 = 0;
      const char *p2 = c;
      if (*p2 != '"') break;
      p2++;
      while (*p2 && *p2 != '"' && n2 + 1 < sizeof edit.elenco[k].nome)
        edit.elenco[k].nome[n2++] = *p2++;
      edit.elenco[k].nome[n2] = 0;
      edit.elenco[k].papel[0] = 0;   // o Cinemeta nao diz o personagem
      edit.elenco[k].foto[0] = 0;
      k++;
      p2++;
      while (*p2 == ' ') p2++;
      c = (*p2 == ',') ? p2 + 1 : NULL;
      while (c && *c == ' ') c++;
    }
    edit.nElenco = k;
    { const char *dr = js_array(corpo, NULL, "director");
      if (dr && *dr == '"') {
        size_t n2 = 0;
        dr++;
        while (*dr && *dr != '"' && n2 + 1 < sizeof edit.direcao)
          edit.direcao[n2++] = *dr++;
        edit.direcao[n2] = 0;
      } }
    // Temporadas presentes, sem repetir e em ordem.
    { const char *v = js_array(corpo, NULL, "videos");
      edit.nTemporadas = 0;
      while (v) {
        const char *fv = js_fim(v);
        int t2 = (int)js_num(v, fv, "season", -1);
        if (t2 > 0) {
          int j, achou = 0;
          for (j = 0; j < edit.nTemporadas; j++)
            if (edit.temporadas[j] == t2) { achou = 1; break; }
          if (!achou && edit.nTemporadas < 12) edit.temporadas[edit.nTemporadas++] = t2;
        }
        v = js_prox(fv);
      }
      { int i2, j2, tmp;
        for (i2 = 0; i2 < edit.nTemporadas; i2++)
          for (j2 = i2 + 1; j2 < edit.nTemporadas; j2++)
            if (edit.temporadas[j2] < edit.temporadas[i2]) {
              tmp = edit.temporadas[i2];
              edit.temporadas[i2] = edit.temporadas[j2];
              edit.temporadas[j2] = tmp;
            } } }
    { char base[24];
      const char *dp;
      snprintf(base, sizeof base, "%s", it->imdb);
      dp = strchr(base, ':');
      if (dp) *(char *)dp = 0;
      fotosDoElenco(&edit, base, !strcmp(it->tipo, "series")); }
    cat_atualizar_item(epItem, &edit);
    printf("[desc] %s: %d atores, dir='%s', %d temporadas\n",
           edit.titulo, edit.nElenco, edit.direcao, edit.nTemporadas);
    fflush(stdout);
  }

  eps = malloc(sizeof(CatEp) * 30);
  if (eps) {
    const char *p = js_array(corpo, NULL, "videos");
    while (p && n < 30) {
      const char *f = js_fim(p);
      int t = (int)js_num(p, f, "season", -1);
      if (t == alvo) {
        CatEp *e = &eps[n];
        memset(e, 0, sizeof *e);
        e->temporada = t;
        e->episodio = (int)js_num(p, f, "episode", 0);
        js_texto(p, f, "name", e->nome, sizeof e->nome);
        js_texto(p, f, "overview", e->sinopse, sizeof e->sinopse);
        js_texto(p, f, "thumbnail", e->thumb, sizeof e->thumb);
        { char d[24] = "";
          js_texto(p, f, "released", d, sizeof d);
          // "2024-04-11T..." -> "11/04/2024"
          if (strlen(d) >= 10 && d[4] == '-')
            snprintf(e->data, sizeof e->data, "%c%c/%c%c/%c%c%c%c",
                     d[8], d[9], d[5], d[6], d[0], d[1], d[2], d[3]); }
        n++;
      }
      p = js_prox(f);
    }
    if (n) cat_definir_episodios(epItem, eps, n);
    printf("[desc] %s T%d: %d episodios\n", it->titulo, alvo, n);
    fflush(stdout);
    free(eps);
  }
  free(corpo);
  fioEpVivo = 0;
  return NULL;
}

void desc_episodios(int indiceItem, int temporada) {
  if (fioEpVivo) return;
  // Ja tem episodios desta temporada carregados: nao refaz.
  { const CatEp *e = cat_episodio(indiceItem, 0);
    if (e && (temporada == 0 || e->temporada == temporada)) return; }
  epItem = indiceItem; epTemp = temporada;
  fioEpVivo = 1;
  if (pthread_create(&fioEp, NULL, buscarEps, NULL) != 0) fioEpVivo = 0;
  else pthread_detach(fioEp);
}
