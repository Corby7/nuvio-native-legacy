#include "addons.h"
#include "streams.h"
#include "rede.h"
#include "js.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

#define ADD_MAX 12

// `fonte` marca quem realmente entrega stream. Descoberto pelo manifesto: o
// Xperience declara resources catalog/meta/subtitles e NENHUM stream, entao
// respondia {"streams":[]} para tudo. Consultar quem nao fornece e um
// round-trip jogado fora em CADA abertura de titulo.
static struct { char nome[64]; char base[600]; int fonte, catalogo, legenda; } addon[ADD_MAX];
static int nAddon;
static AddEstado estado = ADD_PARADO;
static pthread_t fio;
static char alvoId[64], alvoTipo[16];
static int  fioVivo;

// --- leitura do arquivo de configuracao -------------------------------------

int addons_carregar(const char *dirArte) {
  char caminho[600], linha[900];
  FILE *f;
  snprintf(caminho, sizeof caminho, "%s/addons.txt", dirArte ? dirArte : ".");
  f = fopen(caminho, "r");
  if (!f) { printf("[addons] sem %s\n", caminho); return 0; }
  nAddon = 0;
  while (nAddon < ADD_MAX && fgets(linha, sizeof linha, f)) {
    char *tab = strchr(linha, '\t');
    char *fim;
    size_t n;
    // TAB e nao "|" como separador: nome de addon contem "|" de verdade
    // ("AIOStreams | ElfHosted") e partir no primeiro pipe corrompia a URL.
    if (!tab) continue;
    *tab = 0;
    fim = tab + 1 + strlen(tab + 1);
    while (fim > tab + 1 && (fim[-1] == '\n' || fim[-1] == '\r' || fim[-1] == ' ')) *--fim = 0;
    if (linha[0] == '#' || !tab[1]) continue;
    // Terceira coluna (opcional): 1 = fornece stream. Ausente vale 1, para
    // arquivo antigo continuar funcionando.
    addon[nAddon].fonte = 1;
    addon[nAddon].catalogo = 1;
    addon[nAddon].legenda = 0;
    { char *tab2 = strchr(tab + 1, '\t');
      if (tab2) {
        char *tab3;
        *tab2 = 0;
        tab3 = strchr(tab2 + 1, '\t');
        if (tab3) {
          char *tab4 = strchr(tab3 + 1, '\t');
          *tab3 = 0;
          if (tab4) { *tab4 = 0; addon[nAddon].legenda = atoi(tab4 + 1); }
          addon[nAddon].catalogo = atoi(tab3 + 1);
        }
        addon[nAddon].fonte = atoi(tab2 + 1);
      } }
    snprintf(addon[nAddon].nome, sizeof addon[nAddon].nome, "%s", linha);
    snprintf(addon[nAddon].base, sizeof addon[nAddon].base, "%s", tab + 1);
    // A URL guardada aponta para o manifesto; a base e ela sem esse sufixo.
    n = strlen(addon[nAddon].base);
    if (n > 14 && !strcmp(addon[nAddon].base + n - 14, "/manifest.json"))
      addon[nAddon].base[n - 14] = 0;
    else while (n && addon[nAddon].base[n - 1] == '/') addon[nAddon].base[--n] = 0;
    nAddon++;
  }
  fclose(f);
  { int f = 0, k;
    for (k = 0; k < nAddon; k++) f += addon[k].fonte;
    printf("[addons] %d configurados, %d fornecem stream\n", nAddon, f); }
  return nAddon;
}

int addons_n(void) { return nAddon; }

const char *addons_base(int i) {
  return (i >= 0 && i < nAddon) ? addon[i].base : "";
}

// Quarta coluna de addons.txt. Como a de stream, ausente vale 1 — arquivo
// antigo continua funcionando, so faz uma consulta a mais que pode dar vazio.
int addons_tem_catalogo(int i) {
  return (i >= 0 && i < nAddon) ? addon[i].catalogo : 0;
}
AddEstado addons_estado(void) { return estado; }

// --- leitura tolerante de JSON ----------------------------------------------
// Um analisador completo nao se paga aqui: o formato e conhecido e raso, e o
// que importa e nunca travar com campo faltando. Cada funcao devolve o que
// achou ou nada, e quem chama decide.

static const char *pulaEspaco(const char *p) {
  while (*p && (unsigned char)*p <= ' ') p++;
  return p;
}

// Copia o valor textual de "chave" dentro do objeto que comeca em `obj`,
// respeitando escapes. Devolve 1 se achou.
static int campoTexto(const char *obj, const char *fimObj, const char *chave,
                      char *dst, size_t tam) {
  char busca[48];
  const char *p;
  size_t k = 0;
  snprintf(busca, sizeof busca, "\"%s\"", chave);
  p = strstr(obj, busca);
  if (!p || p >= fimObj) return 0;
  p = pulaEspaco(p + strlen(busca));
  if (*p != ':') return 0;
  p = pulaEspaco(p + 1);
  if (*p != '"') return 0;
  p++;
  while (*p && *p != '"' && k + 1 < tam) {
    if (*p == '\\' && p[1]) {
      p++;
      // \u..... vira "?" de proposito: os nomes vem cheios de emoji e o texto
      // e so para exibicao. Decodificar UTF-16 aqui seria trabalho sem retorno.
      if (*p == 'u') { p += 5; dst[k++] = ' '; continue; }
      if (*p == 'n' || *p == 't' || *p == 'r') { p++; dst[k++] = ' '; continue; }
    }
    dst[k++] = *p++;
  }
  dst[k] = 0;
  return k > 0;
}

// Acha o fim do objeto JSON que comeca em `p` (que aponta para '{').
static const char *fimObjeto(const char *p) {
  int prof = 0, texto = 0;
  for (; *p; p++) {
    if (texto) { if (*p == '\\') p++; else if (*p == '"') texto = 0; continue; }
    if (*p == '"') texto = 1;
    else if (*p == '{') prof++;
    else if (*p == '}' && --prof == 0) return p + 1;
  }
  return p;
}

// --- classificacao da fonte -------------------------------------------------
// Os addons nao declaram resolucao nem Dolby Vision em campo proprio: vem tudo
// escrito no nome e na descricao ("2160p", "4K", "DV", "Atmos"). Ler dali e o
// unico jeito, e e o mesmo que o app web ja faz.

static int contem(const char *palheiro, const char *agulha) {
  size_t n = strlen(agulha);
  const char *p = palheiro;
  for (; *p; p++)
    if (!strncasecmp(p, agulha, n)) return 1;
  return 0;
}

static void classificar(Stream *s, const char *texto) {
  if (contem(texto, "2160") || contem(texto, "4k") || contem(texto, "uhd")) s->altura = 2160;
  else if (contem(texto, "1440")) s->altura = 1440;
  else if (contem(texto, "1080")) s->altura = 1080;
  else if (contem(texto, "720"))  s->altura = 720;
  else if (contem(texto, "480"))  s->altura = 480;
  else s->altura = 0;
  // "DV" sozinho da falso positivo dentro de palavras (DVD, DVDRip), por isso
  // so as formas inequivocas.
  s->dolbyVision = contem(texto, "dolby vision") || contem(texto, "dovi") ||
                   contem(texto, "dv|") || contem(texto, " dv ") ||
                   contem(texto, "[dv]") || contem(texto, "hdr-dv");
  s->dolbyAtmos  = contem(texto, "atmos");
  // MP4 progressivo: a extensao aparece na URL ou no nome do arquivo. Serve
  // para a regra de escolha do dono, que prefere MP4 4K Dolby Vision.
  s->mp4 = contem(s->url, ".mp4") || contem(texto, ".mp4");
  s->tamanhoMB = 0;
  { // Tamanho vem escrito no texto ("57.8 GB", "1.4 GB", "900 MB"). Andar para
    // tras a partir da unidade e o jeito de achar o numero sem analisador.
    const char *u = strstr(texto, " GB");
    double mult = 1024.0;
    if (!u) { u = strstr(texto, " MB"); mult = 1.0; }
    if (u) {
      const char *ini = u;
      while (ini > texto && (isdigit((unsigned char)ini[-1]) || ini[-1] == '.')) ini--;
      if (ini < u) s->tamanhoMB = (long)(atof(ini) * mult);
    }
  }
}

// --- busca ------------------------------------------------------------------

static int extrair(const char *json, const char *nomeAddon, Stream *out, int max) {
  const char *p = strstr(json, "\"streams\"");
  int n = 0;
  if (!p) return 0;
  p = strchr(p, '[');
  if (!p) return 0;
  p++;
  while (n < max) {
    const char *ini, *fim;
    Stream s;
    char nome[160], desc[400], url[1024];
    p = pulaEspaco(p);
    if (*p != '{') break;
    ini = p; fim = fimObjeto(p);
    memset(&s, 0, sizeof s);
    nome[0] = desc[0] = url[0] = 0;
    campoTexto(ini, fim, "url", url, sizeof url);
    // Sem URL direta nao ha o que tocar: infoHash e torrent, e nao ha cliente
    // de torrent aqui. Pular em silencio e melhor que listar o que nao abre.
    if (url[0]) {
      campoTexto(ini, fim, "name", nome, sizeof nome);
      if (!campoTexto(ini, fim, "description", desc, sizeof desc))
        campoTexto(ini, fim, "title", desc, sizeof desc);
      snprintf(s.url, sizeof s.url, "%s", url);
      snprintf(s.provedor, sizeof s.provedor, "%s", nomeAddon);
      { char junto[600];
        snprintf(junto, sizeof junto, "%s %s %s", nome, desc, url);
        classificar(&s, junto); }
      snprintf(s.rotulo, sizeof s.rotulo, "%s%s%.60s",
               nome[0] ? nome : nomeAddon, desc[0] ? "  \xc2\xb7  " : "", desc);
      out[n++] = s;
    }
    p = fim;
    p = pulaEspaco(p);
    if (*p == ',') p++;
    else break;
  }
  return n;
}

// --- legendas ---------------------------------------------------------------

static Legenda legs[LEG_MAX];
static int nLegs;
static pthread_t fioLeg;
static int fioLegVivo;
static char legId[64], legTipo[16];

int addons_n_legendas(void) { return nLegs; }
const Legenda *addons_legenda(int i) {
  return (i >= 0 && i < nLegs) ? &legs[i] : NULL;
}

// Idiomas que interessam a esta casa, na ordem em que devem aparecer. Trazer as
// 70 que o OpenSubtitles devolve seria uma lista impossivel de percorrer com
// controle remoto.
static const char *IDIOMAS[] = { "pob", "por", "pt", "eng", "en", "spa", "es" };

static int posIdioma(const char *l) {
  size_t i;
  for (i = 0; i < sizeof IDIOMAS / sizeof *IDIOMAS; i++)
    if (!strcasecmp(l, IDIOMAS[i])) return (int)i;
  return -1;
}

static const char *nomeIdioma(const char *c) {
  if (!strcasecmp(c, "pob")) return "Portugues (BR)";
  if (!strcasecmp(c, "por") || !strcasecmp(c, "pt")) return "Portugues";
  if (!strcasecmp(c, "eng") || !strcasecmp(c, "en")) return "Ingles";
  if (!strcasecmp(c, "spa") || !strcasecmp(c, "es")) return "Espanhol";
  return c;
}

static void *buscarLegendas(void *u) {
  int i;
  (void)u;
  nLegs = 0;
  for (i = 0; i < nAddon && nLegs < LEG_MAX; i++) {
    char url[900], *corpo;
    const char *p;
    // Addon que nao declara legenda nao e consultado: o AIOStreams responderia
    // vazio e o Xperience tambem, dois round-trips sem retorno.
    if (!addon[i].legenda) continue;
    snprintf(url, sizeof url, "%s/subtitles/%s/%s.json",
             addon[i].base, legTipo, legId);
    corpo = rede_baixar(url, 25);
    if (!corpo) continue;
    p = js_array(corpo, NULL, "subtitles");
    { int melhorPos = 99;
      // Duas passadas: primeiro descobre o melhor idioma disponivel, depois
      // recolhe so as dele. Assim a lista nao mistura portugues com hungaro.
      const char *q = p;
      while (q) {
        const char *f = js_fim(q);
        char l[16] = "";
        if (js_texto(q, f, "lang", l, sizeof l)) {
          int k = posIdioma(l);
          if (k >= 0 && k < melhorPos) melhorPos = k;
        }
        q = js_prox(f);
      }
      q = p;
      while (q && nLegs < LEG_MAX) {
        const char *f = js_fim(q);
        char l[16] = "", nome[120] = "";
        Legenda *d = &legs[nLegs];
        if (js_texto(q, f, "lang", l, sizeof l) && posIdioma(l) == melhorPos &&
            js_texto(q, f, "url", d->url, sizeof d->url)) {
          js_texto(q, f, "subtitleFileName", nome, sizeof nome);
          if (!nome[0]) js_texto(q, f, "movieReleaseName", nome, sizeof nome);
          snprintf(d->idioma, sizeof d->idioma, "%s", l);
          snprintf(d->rotulo, sizeof d->rotulo, "%s%s%.36s",
                   nomeIdioma(l), nome[0] ? "  \xc2\xb7  " : "", nome);
          nLegs++;
        }
        q = js_prox(f);
      } }
    free(corpo);
  }
  printf("[legendas] %d\n", nLegs);
  fflush(stdout);
  fioLegVivo = 0;
  return NULL;
}

void addons_buscar_legendas(const char *imdb, const char *tipo) {
  int serie;
  if (!nAddon || !imdb || !*imdb || fioLegVivo) return;
  serie = tipo && !strcmp(tipo, "series");
  if (serie && !strchr(imdb, ':'))
    snprintf(legId, sizeof legId, "%s:1:1", imdb);
  else
    snprintf(legId, sizeof legId, "%s", imdb);
  snprintf(legTipo, sizeof legTipo, "%s", serie ? "series" : "movie");
  fioLegVivo = 1;
  if (pthread_create(&fioLeg, NULL, buscarLegendas, NULL) != 0) fioLegVivo = 0;
  else pthread_detach(fioLeg);
}

static void *buscar(void *u) {
  Stream achados[STREAM_MAX];
  int n = 0, i;
  (void)u;
  for (i = 0; i < nAddon && n < STREAM_MAX; i++) {
    char url[900];
    if (!addon[i].fonte) continue;
    char *corpo;
    int k;
    snprintf(url, sizeof url, "%s/stream/%s/%s.json",
             addon[i].base, alvoTipo, alvoId);
    corpo = rede_baixar(url, 25);
    if (!corpo) { printf("[addons] %s: sem resposta\n", addon[i].nome); continue; }
    k = extrair(corpo, addon[i].nome, achados + n, STREAM_MAX - n);
    printf("[addons] %s: %d fontes (%u bytes)\n",
           addon[i].nome, k, (unsigned)strlen(corpo));
    n += k;
    free(corpo);
  }
  if (n) { stream_definir_lista(achados, n); estado = ADD_PRONTO; }
  else   { estado = ADD_VAZIO; }
  printf("[addons] total %d\n", n);
  fflush(stdout);
  fioVivo = 0;
  return NULL;
}

void addons_buscar(const char *imdb, const char *tipo) {
  int serie;
  if (!nAddon || !imdb || !*imdb || fioVivo) return;
  serie = tipo && !strcmp(tipo, "series");
  // Serie SEM episodio devolve lista vazia, com HTTP 200 e sem erro nenhum
  // (medido: 14 bytes de resposta). O identificador tem de ser
  // "tt1234567:temporada:episodio". Como o catalogo ainda nao traz lista de
  // episodios, assume T1E1 — e o mesmo lugar onde o episodio real entra quando
  // houver.
  if (serie && !strchr(imdb, ':'))
    snprintf(alvoId, sizeof alvoId, "%s:1:1", imdb);
  else
    snprintf(alvoId, sizeof alvoId, "%s", imdb);
  snprintf(alvoTipo, sizeof alvoTipo, "%s", tipo && *tipo ? tipo : "movie");
  estado = ADD_BUSCANDO;
  fioVivo = 1;
  if (pthread_create(&fio, NULL, buscar, NULL) != 0) { fioVivo = 0; estado = ADD_PARADO; }
  else pthread_detach(fio);
}

void addons_encerrar(void) { estado = ADD_PARADO; }
