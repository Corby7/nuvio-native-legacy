#include "addons.h"
#include "streams.h"
#include "rede.h"
#include "js.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>

#define ADD_MAX 12

// `fonte` marca quem realmente entrega stream. Descoberto pelo manifesto: o
// Xperience declara resources catalog/meta/subtitles e NENHUM stream, entao
// respondia {"streams":[]} para tudo. Consultar quem nao fornece e um
// round-trip jogado fora em CADA abertura de titulo.
static struct { char nome[64]; char base[600]; int fonte, catalogo, legenda; } addon[ADD_MAX];
static int nAddon;
static _Atomic AddEstado estado = ADD_PARADO;
static pthread_t fio;
static char alvoId[64], alvoTipo[16];
static int fioVivo;
static Stream *resultado;
static int nResultado;
static char pendId[64], pendTipo[16];

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

int addons_definir_lista(const AddonRemoto *nova, int n) {
  int i, aceitos = 0;
  if (!nova || n <= 0) {
    // Vazio nao substitui. Ver o comentario no cabecalho: uma resposta vazia
    // nao se distingue de uma delecao, e a diferenca entre as duas e a pessoa
    // ficar ou nao sem nenhuma fonte.
    printf("[addons] lista da conta veio vazia; mantendo a local (%d)\n", nAddon);
    return 0;
  }
  for (i = 0; i < n && aceitos < ADD_MAX; i++) {
    size_t k;
    if (!nova[i].url[0] || !nova[i].ativo) continue;
    snprintf(addon[aceitos].nome, sizeof addon[aceitos].nome, "%s",
             nova[i].nome[0] ? nova[i].nome : "Addon");
    snprintf(addon[aceitos].base, sizeof addon[aceitos].base, "%s", nova[i].url);
    k = strlen(addon[aceitos].base);
    if (k > 14 && !strcmp(addon[aceitos].base + k - 14, "/manifest.json"))
      addon[aceitos].base[k - 14] = 0;
    else while (k && addon[aceitos].base[k - 1] == '/') addon[aceitos].base[--k] = 0;
    // A conta nao diz o que cada addon fornece; o manifesto e que diria, e
    // consultar todos no arranque custaria uma viagem por addon. Assumir que
    // fornece tudo faz no maximo uma consulta vazia a mais por titulo — o
    // contrario (assumir que nao fornece) esconderia fontes de verdade.
    addon[aceitos].fonte = 1;
    addon[aceitos].catalogo = 1;
    addon[aceitos].legenda = 0;
    aceitos++;
  }
  if (aceitos == 0) {
    printf("[addons] a conta so tinha addons desligados; mantendo a local\n");
    return 0;
  }
  nAddon = aceitos;
  printf("[addons] %d vindos da conta\n", nAddon);
  return nAddon;
}

int addons_exportar(AddonRemoto *saida, int max) {
  int i, k = 0;
  for (i = 0; i < nAddon && k < max; i++) {
    snprintf(saida[k].nome, sizeof saida[k].nome, "%s", addon[i].nome);
    snprintf(saida[k].url, sizeof saida[k].url, "%s", addon[i].base);
    saida[k].ativo = 1;
    k++;
  }
  return k;
}

void addons_esquecer(void) {
  memset(addon, 0, sizeof addon);
  nAddon = 0;
  printf("[addons] lista esquecida (saiu da conta)\n");
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
AddEstado addons_estado(void) {
  AddEstado e = atomic_load(&estado);
  // Publica no fio da UI: nenhum desenho observa uma lista parcialmente escrita.
  if (fioVivo && e != ADD_BUSCANDO) {
    pthread_join(fio, NULL);
    fioVivo = 0;
    if (!pendId[0]) stream_definir_lista(resultado, nResultado);
    free(resultado); resultado = NULL; nResultado = 0;
    if (pendId[0]) {
      char id[64], tipo[16];
      snprintf(id, sizeof id, "%s", pendId);
      snprintf(tipo, sizeof tipo, "%s", pendTipo);
      pendId[0] = 0;
      addons_buscar(id, tipo);
      return ADD_BUSCANDO;
    }
  }
  return e;
}

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

// --- legendas ---------------------------------------------------------------

static Legenda legs[LEG_MAX];
static int nLegs;
static pthread_t fioLeg;
static int fioLegVivo, fioLegCriado, legParar;
static char legId[64], legTipo[16];
static unsigned legGeracao;
static pthread_mutex_t legTrava = PTHREAD_MUTEX_INITIALIZER;

int addons_n_legendas(void) {
  int n;
  pthread_mutex_lock(&legTrava); n = nLegs; pthread_mutex_unlock(&legTrava);
  return n;
}
const Legenda *addons_legenda(int i) {
  const Legenda *r = NULL;
  pthread_mutex_lock(&legTrava);
  if (i >= 0 && i < nLegs) r = &legs[i];
  pthread_mutex_unlock(&legTrava);
  return r;
}

// Idiomas que interessam a esta casa, na ordem em que devem aparecer. Trazer as
// 70 que o OpenSubtitles devolve seria uma lista impossivel de percorrer com
// controle remoto.
static const char *IDIOMAS_PT[] = {
  "pob", "pt-br", "pt_br", "ptb", "br", "por", "pt"
};
static const char *IDIOMAS_EN[] = {
  "eng", "en", "en-us", "en_us", "en-gb", "en_gb"
};

// 0 = portugues, 1 = ingles. O usuario pediu explicitamente estes dois grupos;
// espanhol nao entra mais como fallback silencioso. Variantes regionais sao
// normalizadas aqui, antes de ocupar uma das doze linhas da TV.
static int grupoIdioma(const char *l) {
  size_t i;
  for (i = 0; i < sizeof IDIOMAS_PT / sizeof *IDIOMAS_PT; i++)
    if (!strcasecmp(l, IDIOMAS_PT[i])) return 0;
  for (i = 0; i < sizeof IDIOMAS_EN / sizeof *IDIOMAS_EN; i++)
    if (!strcasecmp(l, IDIOMAS_EN[i])) return 1;
  return -1;
}

static const char *nomeIdioma(const char *c) {
  if (!strcasecmp(c, "pob") || !strcasecmp(c, "pt-br") ||
      !strcasecmp(c, "pt_br") || !strcasecmp(c, "ptb") || !strcasecmp(c, "br"))
    return "Portugues (BR)";
  if (!strcasecmp(c, "por") || !strcasecmp(c, "pt")) return "Portugues";
  if (grupoIdioma(c) == 1) return "Ingles";
  return c;
}

static int pedidoMudou(unsigned geracao) {
  int mudou;
  pthread_mutex_lock(&legTrava);
  mudou = legParar || geracao != legGeracao;
  pthread_mutex_unlock(&legTrava);
  return mudou;
}

static void episodioPedido(const char *id, int *temporada, int *episodio) {
  const char *p = strchr(id, ':');
  *temporada = *episodio = 0;
  if (p) sscanf(p + 1, "%d:%d", temporada, episodio);
}

static int episodioCorreto(const char *obj, const char *fim, int temporada, int episodio) {
  int t, e;
  if (temporada <= 0 || episodio <= 0) return 1;
  t = (int)js_num(obj, fim, "season", -1);
  e = (int)js_num(obj, fim, "episode", -1);
  // Alguns addons antigos nao devolvem os campos. Quando devolvem, eles sao
  // uma garantia: nunca mostre T2E3 numa busca por T2E4.
  if ((t >= 0 && t != temporada) || (e >= 0 && e != episodio)) return 0;
  if (t >= 0 || e >= 0) return 1;
  // Alguns addons omitem season/episode mas devolvem o episodio no nome do
  // arquivo. Antes aceitavamos S02E03 numa busca por T2E4 e depois fabricavamos
  // o rotulo T2E4 com base no pedido, escondendo o erro. Se o nome traz uma
  // identidade verificavel, ela precisa casar; nome sem marcador segue aceito.
  { char nome[160] = "", baixo[160]; size_t i;
    if (!js_texto(obj, fim, "subtitleFileName", nome, sizeof nome))
      js_texto(obj, fim, "movieReleaseName", nome, sizeof nome);
    for (i = 0; nome[i] && i + 1 < sizeof baixo; i++)
      baixo[i] = (char)tolower((unsigned char)nome[i]);
    baixo[i] = 0;
    for (i = 0; baixo[i]; i++) {
      int nt = -1, ne = -1;
      if (sscanf(baixo + i, "s%2de%2d", &nt, &ne) == 2 ||
          sscanf(baixo + i, "%2dx%2d", &nt, &ne) == 2)
        return nt == temporada && ne == episodio;
    }
  }
  return 1;
}

static void *buscarLegendas(void *u) {
  (void)u;
  for (;;) {
    Legenda achadas[LEG_MAX] = {{0}};
    char id[64], tipo[16];
    unsigned geracao;
    int nAchadas = 0, temporada, episodio, i;

    pthread_mutex_lock(&legTrava);
    if (legParar) { fioLegVivo = 0; pthread_mutex_unlock(&legTrava); return NULL; }
    snprintf(id, sizeof id, "%s", legId);
    snprintf(tipo, sizeof tipo, "%s", legTipo);
    geracao = legGeracao;
    pthread_mutex_unlock(&legTrava);
    episodioPedido(id, &temporada, &episodio);

    for (i = 0; i < nAddon && nAchadas < LEG_MAX; i++) {
      char url[900], *corpo;
      const char *p;
    // Addon que nao declara legenda nao e consultado: o AIOStreams responderia
    // vazio e o Xperience tambem, dois round-trips sem retorno.
      if (!addon[i].legenda) continue;
      snprintf(url, sizeof url, "%s/subtitles/%s/%s.json",
               addon[i].base, tipo, id);
      corpo = rede_baixar(url, 25);
      if (pedidoMudou(geracao)) { free(corpo); break; }
      if (!corpo) continue;
      p = js_array(corpo, NULL, "subtitles");
      {
        int grupo;
        // Uma passada por grupo garante ordem PT -> EN e evita que doze
        // resultados portugueses consumam a lista inteira antes do ingles.
        // Seis por idioma e um limite deliberado para navegacao por D-pad.
        for (grupo = 0; grupo < 2; grupo++) {
          const char *q = p;
          int noGrupo = 0, j;
          for (j = 0; j < nAchadas; j++)
            if (grupoIdioma(achadas[j].idioma) == grupo) noGrupo++;
          while (q && nAchadas < LEG_MAX && noGrupo < LEG_MAX / 2) {
            const char *f = js_fim(q);
            char l[16] = "", nome[120] = "";
            Legenda *d = &achadas[nAchadas];
            if (episodioCorreto(q, f, temporada, episodio) &&
                js_texto(q, f, "lang", l, sizeof l) && grupoIdioma(l) == grupo &&
                js_texto(q, f, "url", d->url, sizeof d->url)) {
              js_texto(q, f, "subtitleFileName", nome, sizeof nome);
              if (!nome[0]) js_texto(q, f, "movieReleaseName", nome, sizeof nome);
              snprintf(d->idioma, sizeof d->idioma, "%s", l);
              if (temporada > 0 && episodio > 0)
                snprintf(d->rotulo, sizeof d->rotulo, "T%dE%d  \xc2\xb7  %s%s%.22s",
                         temporada, episodio, nomeIdioma(l), nome[0] ? "  \xc2\xb7  " : "", nome);
              else
                snprintf(d->rotulo, sizeof d->rotulo, "%s%s%.36s",
                         nomeIdioma(l), nome[0] ? "  \xc2\xb7  " : "", nome);
              nAchadas++; noGrupo++;
            }
            q = js_prox(f);
          }
        }
      }
      free(corpo);
    }

    pthread_mutex_lock(&legTrava);
    if (legParar) { fioLegVivo = 0; pthread_mutex_unlock(&legTrava); return NULL; }
    if (geracao != legGeracao) { pthread_mutex_unlock(&legTrava); continue; }
    memcpy(legs, achadas, sizeof achadas);
    nLegs = nAchadas;
    fioLegVivo = 0;
    pthread_mutex_unlock(&legTrava);
    printf("[legendas] %s: %d\n", id, nAchadas);
    fflush(stdout);
    return NULL;
  }
}

void addons_buscar_legendas(const char *imdb, const char *tipo) {
  int serie, juntar = 0;
  char id[64], tp[16];
  if (!nAddon || !imdb || !*imdb) return;
  serie = tipo && !strcmp(tipo, "series");
  if (serie && !strchr(imdb, ':'))
    snprintf(id, sizeof id, "%s:1:1", imdb);
  else
    snprintf(id, sizeof id, "%s", imdb);
  snprintf(tp, sizeof tp, "%s", serie ? "series" : "movie");

  pthread_mutex_lock(&legTrava);
  if (!strcmp(id, legId) && !strcmp(tp, legTipo) && (fioLegVivo || nLegs > 0)) {
    pthread_mutex_unlock(&legTrava);
    return;
  }
  snprintf(legId, sizeof legId, "%s", id);
  snprintf(legTipo, sizeof legTipo, "%s", tp);
  legGeracao++;
  nLegs = 0;
  if (fioLegVivo) { pthread_mutex_unlock(&legTrava); return; }
  juntar = fioLegCriado;
  pthread_mutex_unlock(&legTrava);

  if (juntar) pthread_join(fioLeg, NULL);
  pthread_mutex_lock(&legTrava);
  fioLegCriado = 0;
  legParar = 0;
  fioLegVivo = 1;
  if (pthread_create(&fioLeg, NULL, buscarLegendas, NULL) != 0) fioLegVivo = 0;
  else fioLegCriado = 1;
  pthread_mutex_unlock(&legTrava);
}

// UM FIO POR ADDON DE FONTE.
//
// MEDIDO NA TV, na sessao do dono: 16,5 s entre abrir o titulo e ter uma fonte
// escolhida (detail_abrir 51098 -> fonte escolhida 67659). Eram consultas em
// SERIE com 25 s de timeout cada; um addon lento atrasa todos os outros, e a
// tela fica com "buscando" o tempo todo.
//
// Os addons sao independentes e `extrair` so escreve no balde que recebe, entao
// cada um le no proprio. A ORDEM e preservada na juncao: ela decide qual fonte
// o automatico ve primeiro, e trocar a ordem trocaria a fonte escolhida.
#define ADD_FIOS 4

typedef struct {
  int    idx;                 // qual addon
  Stream *achados;
  int    n;
} BaldeFonte;

static BaldeFonte *baldes;
static int nBaldes, proxBalde;
static pthread_mutex_t addTrava = PTHREAD_MUTEX_INITIALIZER;

static void *fioFontes(void *u) {
  (void)u;
  for (;;) {
    int meu, i;
    char url[900], *corpo;
    pthread_mutex_lock(&addTrava);
    if (proxBalde >= nBaldes) { pthread_mutex_unlock(&addTrava); return NULL; }
    meu = proxBalde++;
    pthread_mutex_unlock(&addTrava);
    i = baldes[meu].idx;
    snprintf(url, sizeof url, "%s/stream/%s/%s.json",
             addon[i].base, alvoTipo, alvoId);
    // 12 s e nao 25: com os addons em paralelo o timeout deixa de ser somado,
    // mas continua sendo o tempo que o dono espera pelo mais lento.
    corpo = rede_baixar(url, 12);
    if (!corpo) { printf("[addons] %s: sem resposta\n", addon[i].nome); continue; }
    baldes[meu].n = stream_extrair(corpo, addon[i].nome, &baldes[meu].achados);
    printf("[addons] %s: %d fontes (%u bytes)\n",
           addon[i].nome, baldes[meu].n, (unsigned)strlen(corpo));
    free(corpo);
  }
}

static void *buscar(void *u) {
  Stream *achados = NULL;
  int n = 0, i;
  (void)u;

  nBaldes = 0; proxBalde = 0;
  baldes = calloc((size_t)(nAddon > 0 ? nAddon : 1), sizeof(BaldeFonte));
  if (baldes)
    for (i = 0; i < nAddon; i++)
      if (addon[i].fonte) baldes[nBaldes++].idx = i;

  if (baldes && nBaldes > 0) {
    pthread_t fios[ADD_FIOS];
    int criados = 0, q;
    for (q = 0; q < ADD_FIOS && q < nBaldes; q++)
      if (pthread_create(&fios[criados], NULL, fioFontes, NULL) == 0) criados++;
    if (!criados) fioFontes(NULL);        // sem fios: em serie, mesmo resultado
    for (q = 0; q < criados; q++) pthread_join(fios[q], NULL);
    // Junta NA ORDEM DOS ADDONS, que e a ordem em que o dono os instalou.
    for (q = 0; q < nBaldes; q++) {
      int k = baldes[q].n;
      if (k > 0) {
        Stream *tmp = realloc(achados, sizeof(Stream) * (size_t)(n + k));
        if (tmp) { achados = tmp;
          memcpy(achados + n, baldes[q].achados, sizeof(Stream) * (size_t)k);
          n += k;
        } else printf("[addons] memoria insuficiente para %d fontes\n", k);
      }
      free(baldes[q].achados);
    }
  }
  free(baldes); baldes = NULL; nBaldes = 0;

  resultado = achados; nResultado = n;
  printf("[addons] total %d\n", n);
  fflush(stdout);
  atomic_store(&estado, n ? ADD_PRONTO : ADD_VAZIO);
  return NULL;
}

void addons_buscar(const char *imdb, const char *tipo) {
  int serie;
  if (!imdb || !*imdb) return;
  if (!nAddon) { stream_definir_lista(NULL, 0); estado = ADD_VAZIO; return; }
  if (fioVivo) {
    if (strcmp(imdb, alvoId) || strcmp(tipo ? tipo : "movie", alvoTipo)) {
      snprintf(pendId, sizeof pendId, "%s", imdb);
      snprintf(pendTipo, sizeof pendTipo, "%s", tipo ? tipo : "movie");
    }
    return;
  }
  stream_definir_lista(NULL, 0);
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
}

void addons_encerrar(void) {
  int juntarLeg;
  if (fioVivo) pthread_join(fio, NULL);
  fioVivo = 0;
  pthread_mutex_lock(&legTrava);
  legParar = 1; legGeracao++; juntarLeg = fioLegCriado;
  pthread_mutex_unlock(&legTrava);
  if (juntarLeg) pthread_join(fioLeg, NULL);
  pthread_mutex_lock(&legTrava);
  fioLegCriado = fioLegVivo = 0; nLegs = 0;
  pthread_mutex_unlock(&legTrava);
  free(resultado); resultado = NULL; nResultado = 0;
  estado = ADD_PARADO;
}
