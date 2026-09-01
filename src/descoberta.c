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
static char dirArteDesc[512];

void desc_tmdb(const char *dirArte) {
  char caminho[600];
  FILE *f;
  snprintf(dirArteDesc, sizeof dirArteDesc, "%s", dirArte ? dirArte : ".");
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

// --- fileiras da home: catalogos declarados pelos addons ---------------------
// Isto substitui a lista PREF fixa de quatro catalogos. O app web nao tem
// fileira fixa: cada fileira e um catalogo declarado no manifesto de um addon,
// e a ordem/visibilidade/nome saem de `homeCatalogPrefs`. Ver o comentario
// grande em catalogo.h, que traz o algoritmo de sortAndFilterRowsInternal.

#define DECL_MAX 64
// Quantos itens cada fileira mostra. A home desenha no maximo MAX_CARDS (12) e
// buscar mais e trafego que ninguem ve.
#define MAX_POR_FILEIRA 12

static CatFileira filsMontadas[CAT_FIL_MAX];
static int nFileirasMontadas;

typedef struct {
  char chave[192];      // homeCatalogKey:        <addonId>_<tipo>_<catalogoId>
  char desativar[352];  // homeCatalogDisableKey: <base>_<tipo>_<catalogoId>_<nome>
  char titulo[96];
  char tipo[8];
  char id[96];
  const char *base;
} Decl;

// Preferencias do dono, o equivalente local de `homeCatalogPrefs`. Arquivo de
// texto porque o do app web e um localStorage de outro processo — a mesma razao
// que ja valia para o progresso: aquele arquivo pertence a quem o mantem aberto,
// e escrever nele de fora corromperia o estado.
//
//   ordem     <chave>
//   desligada <chave-ou-chave-de-desativar>
//   titulo    <chave><TAB><titulo>
#define PREF_MAX 64
static char prefOrdem[PREF_MAX][192];   static int nPrefOrdem;
static char prefOff[PREF_MAX][352];     static int nPrefOff;
static struct { char chave[192], titulo[96]; } prefTit[PREF_MAX];
static int nPrefTit;
static void lerPrefs(void) {
  char caminho[600], linha[600];
  FILE *f;
  nPrefOrdem = nPrefOff = nPrefTit = 0;
  if (!dirArteDesc[0]) return;
  snprintf(caminho, sizeof caminho, "%s/fileiras.txt", dirArteDesc);
  f = fopen(caminho, "r");
  if (!f) return;
  while (fgets(linha, sizeof linha, f)) {
    char *fim = linha + strlen(linha);
    char *arg;
    while (fim > linha && (fim[-1] == '\n' || fim[-1] == '\r')) *--fim = 0;
    if (!linha[0] || linha[0] == '#') continue;
    arg = strchr(linha, ' ');
    if (!arg) continue;
    *arg++ = 0;
    while (*arg == ' ') arg++;
    if (!strcmp(linha, "ordem") && nPrefOrdem < PREF_MAX) {
      snprintf(prefOrdem[nPrefOrdem++], 192, "%s", arg);
    } else if (!strcmp(linha, "desligada") && nPrefOff < PREF_MAX) {
      snprintf(prefOff[nPrefOff++], 352, "%s", arg);
    } else if (!strcmp(linha, "titulo") && nPrefTit < PREF_MAX) {
      char *tab = strchr(arg, '\t');
      if (!tab) continue;
      *tab++ = 0;
      snprintf(prefTit[nPrefTit].chave, 192, "%s", arg);
      snprintf(prefTit[nPrefTit].titulo, 96, "%s", tab);
      nPrefTit++;
    }
  }
  fclose(f);
  printf("[desc] prefs de fileira: %d na ordem, %d desligadas, %d renomeadas\n",
         nPrefOrdem, nPrefOff, nPrefTit);
}

// A conferencia e contra DUAS chaves, como no web: quem desliga pela tela de
// ajustes grava a chave de desativar (que carrega a URL base e o nome), e quem
// desliga pela ordenacao grava a chave curta.
static int desligada(const Decl *d) {
  int i;
  for (i = 0; i < nPrefOff; i++)
    if (!strcmp(prefOff[i], d->chave) || !strcmp(prefOff[i], d->desativar)) return 1;
  return 0;
}

// formatCatalogRowTitle (js/ui/screens/home/homeUtils.js:62): primeira letra
// maiuscula e, se o nome ja NAO termina com o rotulo do tipo, " - <tipo>".
// E por isso que a home mostra "For You - Filme" e nao "for you".
static void formatarTitulo(const char *nome, const char *tipo, char *dst, size_t tam) {
  const char *rotulo = strcmp(tipo, "series") ? "Filme" : "S\xc3\xa9rie";
  const char *cru    = strcmp(tipo, "series") ? "Movie" : "Series";
  size_t ln = strlen(nome), lr = strlen(rotulo), lc = strlen(cru);
  int jaTem = 0;
  if (!nome[0]) { snprintf(dst, tam, "%s", rotulo); return; }
  if (ln >= lr && !strcasecmp(nome + ln - lr, rotulo)) jaTem = 1;
  if (ln >= lc && !strcasecmp(nome + ln - lc, cru))    jaTem = 1;
  if (jaTem) snprintf(dst, tam, "%s", nome);
  else       snprintf(dst, tam, "%s - %s", nome, rotulo);
  if (dst[0] >= 'a' && dst[0] <= 'z') dst[0] = (char)(dst[0] - 32);
}

// Le <base>/manifest.json e acrescenta os catalogos declarados.
static int lerManifesto(const char *base, Decl *saida, int max) {
  char url[900], addonId[96] = "", nome[96], tipo[8], id[96];
  char *corpo;
  const char *p, *fim;
  int n = 0;
  snprintf(url, sizeof url, "%s/manifest.json", base);
  corpo = rede_baixar(url, 20);
  if (!corpo) return 0;
  fim = corpo + strlen(corpo);
  js_texto(corpo, fim, "id", addonId, sizeof addonId);
  p = js_array(corpo, fim, "catalogs");
  while (p && n < max) {
    const char *f = js_fim(p);
    tipo[0] = id[0] = nome[0] = 0;
    js_texto(p, f, "type", tipo, sizeof tipo);
    js_texto(p, f, "id",   id,   sizeof id);
    js_texto(p, f, "name", nome, sizeof nome);
    // Sem tipo ou sem id nao da para montar a URL do catalogo; e um catalogo
    // que nao responde e pior que uma fileira a menos.
    if (tipo[0] && id[0]) {
      Decl *d = &saida[n];
      memset(d, 0, sizeof *d);
      d->base = base;
      snprintf(d->tipo, sizeof d->tipo, "%s", tipo);
      snprintf(d->id,   sizeof d->id,   "%s", id);
      snprintf(d->chave, sizeof d->chave, "%s_%s_%s",
               addonId[0] ? addonId : base, tipo, id);
      snprintf(d->desativar, sizeof d->desativar, "%s_%s_%s_%s", base, tipo, id, nome);
      formatarTitulo(nome, tipo, d->titulo, sizeof d->titulo);
      n++;
    }
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

  // As fileiras vem dos CATALOGOS declarados nos manifestos dos addons, e nao
  // de uma lista fixa. A ordem, o que fica de fora e os nomes seguem o
  // algoritmo do web (sortAndFilterRowsInternal), com as preferencias lidas de
  // art/fileiras.txt.
  {
    Decl decls[DECL_MAX];
    int nDecl = 0, k;
    CatFileira fil[CAT_FIL_MAX];
    int nFil = 0;
    // A fileira 0 e "Continuar assistindo", que ja foi montada acima. Ela e
    // SINTETICA: nao esta na ordem do web e nao pode ser desligada por chave —
    // no app ela existe sempre que ha progresso.
    if (n > 0) {
      CatFileira *f0 = &fil[nFil++];
      memset(f0, 0, sizeof *f0);
      snprintf(f0->chave,  sizeof f0->chave,  "continue_watching");
      snprintf(f0->titulo, sizeof f0->titulo, "Continuar assistindo");
      snprintf(f0->tipo,   sizeof f0->tipo,   "movie");
      f0->ini = 0; f0->n = n;
    }

    lerPrefs();
    for (i = 0; i < addons_n() && nDecl < DECL_MAX; i++) {
      if (!addons_tem_catalogo(i)) continue;
      nDecl += lerManifesto(addons_base(i), decls + nDecl, DECL_MAX - nDecl);
    }
    printf("[desc] %d catalogos declarados pelos addons\n", nDecl);

    // ensureOrderKeysWithPrefs: a ordem salva primeiro, e as chaves NOVAS
    // acrescentadas no fim. Catalogo que o addon passou a declarar hoje entra
    // por ultimo, nao no meio — e o que evita a home se reorganizar sozinha.
    {
      int ordem[DECL_MAX];
      int nOrdem = 0, j;
      char vistos[DECL_MAX];
      memset(vistos, 0, sizeof vistos);
      for (k = 0; k < nPrefOrdem; k++)
        for (j = 0; j < nDecl; j++)
          if (!vistos[j] && !strcmp(decls[j].chave, prefOrdem[k])) {
            ordem[nOrdem++] = j; vistos[j] = 1; break;
          }
      for (j = 0; j < nDecl; j++) if (!vistos[j]) ordem[nOrdem++] = j;

      for (k = 0; k < nOrdem && nFil < CAT_FIL_MAX; k++) {
        Decl *d = &decls[ordem[k]];
        int got, t;
        if (desligada(d)) continue;
        // customTitles ganha do nome do manifesto.
        for (t = 0; t < nPrefTit; t++)
          if (!strcmp(prefTit[t].chave, d->chave)) {
            snprintf(d->titulo, sizeof d->titulo, "%s", prefTit[t].titulo);
            break;
          }
        GARANTE(MAX_POR_FILEIRA + 2);
        got = lerCatalogo(d->base, d->tipo, d->id, lote + n, cap - n,
                          MAX_POR_FILEIRA);
        if (!got) continue;   // fileira vazia nao vira titulo pendurado
        {
          CatFileira *f = &fil[nFil++];
          memset(f, 0, sizeof *f);
          snprintf(f->chave,  sizeof f->chave,  "%s", d->chave);
          snprintf(f->titulo, sizeof f->titulo, "%s", d->titulo);
          snprintf(f->tipo,   sizeof f->tipo,   "%s", d->tipo);
          f->ini = n; f->n = got;
        }
        n += got;
        printf("[desc] fileira %d: %s (%d)\n", nFil - 1, d->titulo, got);
      }
      nFileirasMontadas = nFil;
      memcpy(filsMontadas, fil, sizeof(CatFileira) * (size_t)nFil);
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
    cat_definir_tudo(lote, n, filsMontadas, nFileirasMontadas);
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
          // "2024-11-15T..." -> "15 de novembro de 2024".
          //
          // O web NAO escreve 15/11/2024: usa
          // `toLocaleDateString(undefined, {month:"long", day:"numeric",
          // year:"numeric"})` (metaDetailsScreen.js:1387), e no locale do
          // aparelho isso da a data por extenso. O formato numerico que estava
          // aqui era invencao do port.
          //
          // Por extenso e sempre: quem decide encurtar para so o ano e quem
          // DESENHA, conforme `showFullReleaseDate` — a preferencia pode mudar
          // com o app aberto, e a descoberta so roda uma vez. O ano sao os
          // quatro ultimos caracteres da string.
          static const char *MES[12] = {
            "janeiro", "fevereiro", "mar\xc3\xa7o", "abril", "maio", "junho",
            "julho", "agosto", "setembro", "outubro", "novembro", "dezembro"
          };
          if (strlen(d) >= 10 && d[4] == '-') {
            int mes = (d[5] - '0') * 10 + (d[6] - '0');
            int dia = (d[8] - '0') * 10 + (d[9] - '0');
            if (mes >= 1 && mes <= 12)
              snprintf(e->data, sizeof e->data, "%d de %s de %c%c%c%c",
                       dia, MES[mes - 1], d[0], d[1], d[2], d[3]);
            else
              snprintf(e->data, sizeof e->data, "%c%c%c%c", d[0], d[1], d[2], d[3]);
          } }
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
