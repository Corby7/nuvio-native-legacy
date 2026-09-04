#include "catalog.h"
#include "discover.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Alocado conforme chega, nao dimensionado por um numero chutado.
static CatItem *items;
static CatRow filters[CAT_FILTER_MAX];
static int nFilters;
static int nAllocated;
// 1 enquanto o catalogo na tela veio do cache em disco, e nao da rede desta
// sessao. Ver a nota em catalogo.h.
static int cameOfCache;

// Garante espaco para `quero` itens. Devolve 0 se nao deu (e o chamador segue
// com o que ja tinha, que e melhor que perder tudo).
static void ensureTracks(int count);

static int ensureSpace(int want) {
  CatItem *new;
  int target;
  if (want <= nAllocated) return 1;
  if (want > CAT_MAX) want = CAT_MAX;
  target = nAllocated ? nAllocated * 2 : 64;
  while (target < want) target *= 2;
  new = realloc(items, sizeof(CatItem) * (size_t)target);
  if (!new) return 0;
  memset(new + nAllocated, 0, sizeof(CatItem) * (size_t)(target - nAllocated));
  items = new;
  nAllocated = target;
  return 1;
}
static char dirWriting[512];

// Episodios de todos os titulos num vetor unico, com faixa por titulo. Uma
// matriz [titulo][episodio] gastaria memoria pelo pior caso em 40 titulos dos
// quais a maioria e filme e nao tem episodio nenhum.
#define CAT_EP_MAX 600
static CatEp eps[CAT_EP_MAX];
// Faixas de episodio por titulo, do mesmo tamanho do vetor de itens — que
// agora cresce, entao estes tambem.
static int  *epStart, *epCount, nEps;

// O progresso de reproducao e uma posicao, nao uma prova de que o titulo foi
// marcado como assistido. O historico do Trakt fica separado, por identidade
// estavel, para que uma troca do catalogo nao transforme indice em identidade
// e para que um progresso alto nao masque um historico real conhecido.
typedef struct {
  char imdb[32];
  char kind[8];
  int known;
  int watched;
} CatHistory;

static CatHistory history[CAT_MAX];
static int nHistory;

static void id_base(const char *origin, char *destination, size_t size) {
  size_t n = 0;
  if (!destination || size == 0) return;
  if (origin) {
    while (origin[n] && origin[n] != ':' && n + 1 < size) n++;
    memcpy(destination, origin, n);
  }
  destination[n] = 0;
}

static const char *kind_base(const char *kind) {
  if (kind && (!strcmp(kind, "series") || !strcmp(kind, "show"))) return "series";
  return "movie";
}

static int history_pos(const char *imdb, const char *kind, int create) {
  char id[32];
  int i;
  id_base(imdb, id, sizeof id);
  if (!id[0]) return -1;
  for (i = 0; i < nHistory; i++)
    if (!strcmp(history[i].imdb, id) &&
        !strcmp(history[i].kind, kind_base(kind))) return i;
  if (!create || nHistory >= CAT_MAX) return -1;
  snprintf(history[nHistory].imdb, sizeof history[nHistory].imdb, "%s", id);
  snprintf(history[nHistory].kind, sizeof history[nHistory].kind, "%s", kind_base(kind));
  return nHistory++;
}

// Leitura interna da modal: -1 = historico ainda nao consultado, 0 = nao
// visto confirmado, 1 = visto confirmado.
int cat_history_state_item(int index_) {
  const CatItem *it = cat_item(index_);
  int p;
  if (!it || !it->imdb[0]) return -1;
  p = history_pos(it->imdb, it->kind, 0);
  return p >= 0 && history[p].known ? history[p].watched : -1;
}

// Atualiza o retrato de historico somente depois de uma resposta 2xx do
// Trakt. A chave e o IMDb sem sufixo de episodio, nunca o indice do vetor.
void cat_history_set_id(const char *imdb, const char *kind, int watched) {
  int p = history_pos(imdb, kind, 1);
  if (p < 0) return;
  history[p].known = 1;
  history[p].watched = watched ? 1 : 0;
}

// Compatibilidade para chamadores antigos que so conhecem o IMDb. A serie e
// inferida do proprio catalogo quando possivel; o sufixo de episodio e o
// fallback para itens que ainda nao entraram no vetor.
const char *cat_kind_by_imdb(const char *imdb) {
  int i = cat_index_by_imdb(imdb);
  if (i >= 0 && cat_item(i)) return cat_item(i)->kind;
  return (imdb && strchr(imdb, ':')) ? "series" : "movie";
}

static void ensureTracks(int count) {
  int *a, *b;
  if (count < 1) return;
  a = realloc(epStart, sizeof(int) * (size_t)count);
  b = realloc(epCount, sizeof(int) * (size_t)count);
  if (a) epStart = a;
  if (b) epCount = b;
  if (epStart) memset(epStart, 0, sizeof(int) * (size_t)count);
  if (epCount) memset(epCount, 0, sizeof(int) * (size_t)count);
}
static int n = 0;

// Copia o campo ate o proximo '|' (ou fim de linha), sem estourar o destino.
static const char *field(const char *p, char *destination, size_t size) {
  size_t k = 0;
  while (*p && *p != '|' && *p != '\n') {
    if (k + 1 < size) destination[k++] = *p;
    p++;
  }
  destination[k] = 0;
  return (*p == '|') ? p + 1 : p;
}

int cat_load(const char *dirArt) {
  char path[600];
  snprintf(path, sizeof path, "%s/catalog.txt", dirArt);
  FILE *f = fopen(path, "r");
  if (!f) { printf("catalog: %s missing, carrying on without it\n", path); return 0; }

  char line[2048];
  n = 0;
  while (n < CAT_MAX && ensureSpace(n + 1) && fgets(line, sizeof line, f)) {
    if (line[0] == '\n' || line[0] == '#') continue;
    CatItem *it = &items[n];
    char rel[512];
    const char *p = line;
    p = field(p, rel, sizeof rel);
    // os caminhos no arquivo sao relativos a pasta de arte
    if (rel[0]) snprintf(it->backdrop, sizeof it->backdrop, "%s/%s", dirArt, rel);
    else it->backdrop[0] = 0;
    p = field(p, rel, sizeof rel);
    if (rel[0]) snprintf(it->poster, sizeof it->poster, "%s/%s", dirArt, rel);
    else it->poster[0] = 0;
    p = field(p, rel, sizeof rel);
    if (rel[0]) snprintf(it->logo, sizeof it->logo, "%s/%s", dirArt, rel);
    else it->logo[0] = 0;
    p = field(p, it->title, sizeof it->title);
    p = field(p, it->genre, sizeof it->genre);
    // O catalogo do pacote guarda o genero JA COMPOSTO e em ingles
    // ("Filme  ·  Science Fiction  ·  Action"). Traduz cada pedaco entre os
    // separadores; o primeiro ("Filme"/"Programa de TV") ja vem em portugues e
    // atravessa a tabela sem mudanca. Feito aqui, na leitura, porque `genero` e
    // lido por varias telas e traduzir no desenho deixaria cada uma resolver
    // por conta propria.
    { char output[sizeof it->genre]; size_t o = 0;
      const char *q = it->genre;
      const char *SEP = "  \xc2\xb7  ";
      while (*q && o + 1 < sizeof output) {
        const char *sp = strstr(q, SEP);
        char part[64]; size_t n = sp ? (size_t)(sp - q) : strlen(q);
        const char *pt;
        if (n >= sizeof part) n = sizeof part - 1;
        memcpy(part, q, n); part[n] = 0;
        pt = disc_genre_label(part);
        o += (size_t)snprintf(output + o, sizeof output - o, "%s%s",
                              o ? SEP : "", pt);
        if (!sp) break;
        q = sp + strlen(SEP);
      }
      if (o) snprintf(it->genre, sizeof it->genre, "%s", output); }
    p = field(p, it->meta, sizeof it->meta);
    p = field(p, it->age_rating, sizeof it->age_rating);
    field(p, it->synopsis, sizeof it->synopsis);
    it->imdb[0] = 0;
    snprintf(it->kind, sizeof it->kind, "movie");
    n++;
  }
  fclose(f);

  // ids.txt e um arquivo A PARTE, uma linha "tt1234567<TAB>movie|series" por
  // titulo, na mesma ordem. Ficou fora de catalogo.txt para nao mexer na ordem
  // das colunas de um arquivo que ja tem parser e dados. Sem ele o app roda
  // igual, so nao consegue perguntar fontes aos addons.
  snprintf(path, sizeof path, "%s/ids.txt", dirArt);
  f = fopen(path, "r");
  if (f) {
    int i = 0;
    while (i < n && fgets(line, sizeof line, f)) {
      char *tab = strchr(line, '\t');
      char *end;
      if (tab) {
        *tab = 0;
        snprintf(items[i].kind, sizeof items[i].kind, "%s", tab + 1);
        end = items[i].kind + strlen(items[i].kind);
        while (end > items[i].kind && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
      }
      snprintf(items[i].imdb, sizeof items[i].imdb, "%s", line);
      { char *e = items[i].imdb + strlen(items[i].imdb);
        while (e > items[i].imdb && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0; }
      i++;
    }
    fclose(f);
    printf("catalog: %d ids\n", i);
  }

  // Elenco vem num arquivo separado, uma linha por titulo, na mesma ordem:
  // "nome~papel~foto;nome~papel~foto|direcao". Separado porque tem tamanho bem
  // diferente do resto e mudaria a linha do catalogo a cada ator a mais.
  snprintf(path, sizeof path, "%s/cast.txt", dirArt);
  FILE *fe = fopen(path, "r");
  if (fe) {
    for (int i = 0; i < n && fgets(line, sizeof line, fe); i++) {
      char *bar = strchr(line, '|');
      if (bar) {
        *bar = 0;
        char *d = bar + 1, *end = d + strlen(d);
        while (end > d && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
        snprintf(items[i].directing, sizeof items[i].directing, "%s", d);
      }
      char *p2 = line;
      while (*p2 && items[i].nCast < 6) {
        char *pv = strchr(p2, ';');
        if (pv) *pv = 0;
        char *t1 = strchr(p2, '~');
        if (t1) {
          *t1 = 0;
          char *t2 = strchr(t1 + 1, '~');
          if (t2) *t2 = 0;
          int k = items[i].nCast;
          snprintf(items[i].cast[k].name, 64, "%s", p2);
          snprintf(items[i].cast[k].role, 64, "%s", t1 + 1);
          if (t2 && t2[1] && t2[1] != '\n')
            snprintf(items[i].cast[k].photo, 512, "%s/%s", dirArt, t2 + 1);
          items[i].nCast++;
        }
        if (!pv) break;
        p2 = pv + 1;
      }
    }
    fclose(fe);
  }

  // extra.txt: "nota|logoProv|nomeProv|progresso|temporada|episodio|restanteMin", na
  // mesma ordem. O progresso entrou como QUARTA coluna para nao invalidar
  // arquivos antigos: faltando, o campo fica 0 e a barra some, que e o
  // comportamento certo para quem nunca comecou o titulo.
  snprintf(path, sizeof path, "%s/extra.txt", dirArt);
  FILE *fx = fopen(path, "r");
  if (fx) {
    for (int i = 0; i < n && fgets(line, sizeof line, fx); i++) {
      char c1[32] = "", c2[512] = "", c3[64] = "", c4[16] = "";
      char c5[16] = "", c6[16] = "", c7[16] = "";
      const char *q = line;
      q = field(q, c1, sizeof c1);
      q = field(q, c2, sizeof c2);
      q = field(q, c3, sizeof c3);
      q = field(q, c4, sizeof c4);
      q = field(q, c5, sizeof c5);
      q = field(q, c6, sizeof c6);
      field(q, c7, sizeof c7);
      items[i].score = atoi(c1);
      items[i].progress = atoi(c4);
      items[i].season = atoi(c5);
      items[i].episode  = atoi(c6);
      items[i].remainingMin = atoi(c7);
      if (c2[0]) snprintf(items[i].providerLogo, sizeof items[i].providerLogo, "%s/%s", dirArt, c2);
      snprintf(items[i].providerName, sizeof items[i].providerName, "%s", c3);
    }
    fclose(fx);
  }

  // progresso.txt: "tt1234567<TAB>posicaoSeg<TAB>duracaoSeg" por linha, o que
  // ESTE app gravou. Vem depois de extra.txt de proposito — o que se assistiu
  // aqui e mais recente que o retrato trazido do app web.
  snprintf(path, sizeof path, "%s/progress.txt", dirArt);
  { FILE *fp = fopen(path, "r");
    int applied = 0;
    if (fp) {
      while (fgets(line, sizeof line, fp)) {
        char id[24]; double pos, duration; int i;
        int season = 0, episode = 0;
        if (sscanf(line, "%23s %lf %lf %d %d", id, &pos, &duration, &season, &episode) < 3 || duration <= 1.0) continue;
        for (i = 0; i < n; i++) {
          // O id do catalogo pode trazer episodio ("tt123:4:9"); comparar so o
          // prefixo do titulo, que e o que identifica a obra.
          if (!strncmp(items[i].imdb, id, strlen(id)) &&
              (items[i].imdb[strlen(id)] == 0 || items[i].imdb[strlen(id)] == ':')) {
            items[i].progress = (int)(100.0 * pos / duration);
            items[i].remainingMin = (int)((duration - pos) / 60.0 + 0.5);
            if(season>0 && episode>0) {
              if(items[i].season!=season || items[i].episode!=episode)
                items[i].nameEpisode[0]=0;
              items[i].season=season; items[i].episode=episode;
            }
            applied++;
            break;
          }
        }
      }
      fclose(fp);
      if (applied) printf("catalog: %d progress entries from this app\n", applied);
    }
    snprintf(dirWriting, sizeof dirWriting, "%s", dirArt);
  }

  // episodios.txt: "indice|temporada|episodio|nome|duracao|data|sinopse".
  // Indice na frente porque so parte dos titulos tem episodio — uma linha por
  // titulo, como nos outros arquivos, desperdicaria a maioria das linhas.
  snprintf(path, sizeof path, "%s/episodes.txt", dirArt);
  { FILE *fe2 = fopen(path, "r");
    nEps = 0;
    ensureTracks(nAllocated);
    if (fe2) {
      while (nEps < CAT_EP_MAX && fgets(line, sizeof line, fe2)) {
        char c1[8], c2[8], c3[8];
        const char *q = line;
        int target;
        CatEp *ep = &eps[nEps];
        memset(ep, 0, sizeof *ep);
        q = field(q, c1, sizeof c1);
        target = atoi(c1);
        if (target < 0 || target >= n) continue;
        q = field(q, c2, sizeof c2);
        q = field(q, c3, sizeof c3);
        ep->season = atoi(c2);
        ep->episode  = atoi(c3);
        q = field(q, ep->name, sizeof ep->name);
        q = field(q, ep->duration, sizeof ep->duration);
        q = field(q, ep->date, sizeof ep->date);
        q = field(q, ep->synopsis, sizeof ep->synopsis);
        { char rel[512] = "";
          field(q, rel, sizeof rel);
          if (rel[0]) snprintf(ep->thumb, sizeof ep->thumb, "%s/%s", dirArt, rel); }
        if (!epCount[target]) epStart[target] = nEps;
        epCount[target]++;
        nEps++;
      }
      fclose(fe2);
      printf("catalog: %d episodes\n", nEps);
    }
  }

  int comCast = 0;
  for (int i = 0; i < n; i++) if (items[i].nCast) comCast++;
  printf("catalog: %d titles, %d with cast (item0: %d actors, dir='%s')\n",
         n, comCast, n ? items[0].nCast : 0, n ? items[0].directing : "");
  return n;
}

// --- CACHE EM DISCO ----------------------------------------------------------
//
// Ver a nota em catalogo.h. O cabecalho carrega a versao E o sizeof(CatItem):
// e o sizeof que protege de verdade, porque acrescentar um campo na struct
// muda o layout sem que ninguem se lembre de subir a versao a mao.
#define CACHE_MAGIC  0x4E56434Bu   /* "NVCK" */
#define CACHE_VERSION 1

typedef struct {
  unsigned magic, version, sizeItem, sizeRow;
  int nItems, nRows;
} CacheHeader;

static void pathCache(const char *dirArt, char *dst, size_t size) {
  snprintf(dst, size, "%s/catalog-net.bin", dirArt ? dirArt : ".");
}

// Chamado pela descoberta quando o catalogo COMPLETO da rede substitui o do
// cache. A partir daqui a tela ja e a desta sessao.
void cat_cache_replaced(void) { cameOfCache = 0; }

int cat_write_cache(const char *dirArt) {
  char path[600], tmp[620];
  CacheHeader c;
  FILE *f;
  if (n < 1) return 0;
  pathCache(dirArt, path, sizeof path);
  // Grava num temporario e renomeia: quem le na proxima abertura nunca pega
  // arquivo pela metade se o app for fechado no meio da escrita.
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  f = fopen(tmp, "wb");
  if (!f) return 0;
  c.magic = CACHE_MAGIC; c.version = CACHE_VERSION;
  c.sizeItem = (unsigned)sizeof(CatItem);
  c.sizeRow = (unsigned)sizeof(CatRow);
  c.nItems = n; c.nRows = nFilters;
  if (fwrite(&c, sizeof c, 1, f) != 1 ||
      fwrite(items, sizeof(CatItem), (size_t)n, f) != (size_t)n ||
      (nFilters > 0 &&
       fwrite(filters, sizeof(CatRow), (size_t)nFilters, f) != (size_t)nFilters)) {
    fclose(f); remove(tmp); return 0;
  }
  fclose(f);
  if (rename(tmp, path) != 0) { remove(tmp); return 0; }
  printf("[cat] cache written: %d titles, %d rows\n", n, nFilters);
  fflush(stdout);
  return 1;
}

int cat_read_cache(const char *dirArt) {
  char path[600];
  CacheHeader c;
  FILE *f;
  CatItem *new;
  CatRow readRows[CAT_FILTER_MAX];
  int nRead = 0;
  pathCache(dirArt, path, sizeof path);
  f = fopen(path, "rb");
  if (!f) return 0;
  if (fread(&c, sizeof c, 1, f) != 1) { fclose(f); return 0; }
  // RECUSA em vez de ler torto. Struct diferente = arquivo de outra build.
  if (c.magic != CACHE_MAGIC || c.version != CACHE_VERSION ||
      c.sizeItem != sizeof(CatItem) || c.sizeRow != sizeof(CatRow) ||
      c.nItems < 1 || c.nItems > CAT_MAX ||
      c.nRows < 0 || c.nRows > CAT_FILTER_MAX) {
    fclose(f);
    printf("[cat] cache discarded (format from another build)\n");
    remove(path);
    return 0;
  }
  new = malloc(sizeof(CatItem) * (size_t)c.nItems);
  if (!new) { fclose(f); return 0; }
  if (fread(new, sizeof(CatItem), (size_t)c.nItems, f) != (size_t)c.nItems) {
    free(new); fclose(f); remove(path); return 0;
  }
  if (c.nRows > 0) {
    if (fread(readRows, sizeof(CatRow), (size_t)c.nRows, f)
        != (size_t)c.nRows) {
      free(new); fclose(f); remove(path); return 0;
    }
    nRead = c.nRows;
  }
  fclose(f);
  // Reaproveita o caminho de troca de bloco, que ja e o seguro para o fio de
  // desenho — e o que corta as janelas de fileira pelo tamanho real.
  cat_set_all(new, c.nItems, readRows, nRead);
  cameOfCache = 1;
  free(new);
  printf("[cat] cache read: %d titles, %d rows\n", c.nItems, nRead);
  fflush(stdout);
  return 1;
}

int cat_do_cache(void) { return cameOfCache; }

int cat_n(void) { return n; }

const CatItem *cat_item(int i) {
  if (!items || n <= 0) return NULL;
  return &items[((i % n) + n) % n];
}

// Compara so ate o primeiro ':' — o catalogo guarda "tt123:2:1" em serie com
// progresso, e quem procura tem so o id do titulo.
static int sameTitle(const char *a, const char *b) {
  while (*a && *b && *a != ':' && *b != ':') { if (*a != *b) return 0; a++; b++; }
  return (!*a || *a == ':') && (!*b || *b == ':');
}

void cat_dir_writing(const char *dir) {
  if (dir && *dir) snprintf(dirWriting, sizeof dirWriting, "%s", dir);
}

int cat_index_by_imdb(const char *imdb) {
  int i;
  if (!imdb || !imdb[0]) return -1;
  for (i = 0; i < cat_n(); i++) {
    const CatItem *c = cat_item(i);
    if (c && c->imdb[0] && sameTitle(c->imdb, imdb)) return i;
  }
  return -1;
}

void cat_save_progress(int index_, double posSeg, double durationSeg) {
  cat_save_progress_ep(index_,posSeg,durationSeg,0,0);
}

void cat_save_progress_ep(int index_, double posSeg, double durationSeg, int season, int episode) {
  char path[600], tmp[600], line[256];
  FILE *e, *s;
  const CatItem *it;
  int i;
  if (durationSeg <= 1.0 || !dirWriting[0]) return;
  i = cat_n(); if (i < 1) return;
  index_ = ((index_ % i) + i) % i;
  it = &items[index_];
  if (!it->imdb[0]) return;

  // Reescreve o arquivo inteiro trocando a linha deste titulo. E um arquivo de
  // dezenas de linhas: ler tudo e regravar custa nada e evita duplicata, que
  // um simples append acumularia.
  snprintf(path, sizeof path, "%s/progress.txt", dirWriting);
  snprintf(tmp, sizeof tmp, "%s/progress.tmp", dirWriting);
  s = fopen(tmp, "w");
  if (!s) return;
  e = fopen(path, "r");
  if (e) {
    while (fgets(line, sizeof line, e)) {
      char id[24];
      if (sscanf(line, "%23s", id) == 1 && !strcmp(id, it->imdb)) continue;
      fputs(line, s);
    }
    fclose(e);
  }
  fprintf(s, "%s\t%.0f\t%.0f\t%d\t%d\n", it->imdb, posSeg, durationSeg,season,episode);
  fclose(s);
  // Gravar em temporario e renomear: um corte de energia no meio da escrita
  // deixaria o arquivo pela metade e o app subiria sem progresso nenhum.
  rename(tmp, path);

  items[index_].progress = (int)(100.0 * posSeg / durationSeg);
  items[index_].remainingMin = (int)((durationSeg - posSeg) / 60.0 + 0.5);
  if(season>0 && episode>0) {
    if (items[index_].season != season || items[index_].episode != episode)
      items[index_].nameEpisode[0] = 0;
    items[index_].season=season;
    items[index_].episode=episode;
    for (int e = 0; e < cat_n_episodes(index_); e++) {
      const CatEp *ep = cat_episode(index_, e);
      if (ep && ep->season == season && ep->episode == episode) {
        snprintf(items[index_].nameEpisode, sizeof items[index_].nameEpisode, "%s", ep->name);
        break;
      }
    }
  }
}

int cat_n_episodes(int indexItem) {
  int m = cat_n();
  if (m < 1) return 0;
  indexItem = ((indexItem % m) + m) % m;
  return epCount[indexItem];
}

const CatEp *cat_episode(int indexItem, int i) {
  int m = cat_n();
  if (m < 1) return NULL;
  indexItem = ((indexItem % m) + m) % m;
  if (i < 0 || i >= epCount[indexItem]) return NULL;
  return &eps[epStart[indexItem] + i];
}

int cat_n_rows(void) { return nFilters; }
const CatRow *cat_row(int r) {
  return (r >= 0 && r < nFilters) ? &filters[r] : NULL;
}

// ACRESCENTA UM titulo ao fim do catalogo e devolve o indice dele.
//
// Existe para o titulo que veio de FORA: um credito na filmografia de um ator
// ou um item de "Mais como este" que o catalogo do dono nao tem. Sem isto o
// item ficava apagado e nao abria, o que deixava a filmografia decorativa.
//
// Usa a MESMA troca de bloco de cat_definir_tudo, pelo mesmo motivo (leitor no
// fio de desenho dentro do bloco antigo), com duas diferencas:
//   - acrescenta no FIM, entao as janelas (ini,n) das fileiras continuam
//     valendo e nao precisam ser derrubadas;
//   - `n` NAO e zerado: subir a contagem depois que o bloco novo ja esta
//     publicado e seguro, e zerar faria a home piscar a cada titulo aberto.
void cat_set_in_list(int i, int inList) {
  if (!items || n <= 0 || i < 0 || i >= n) return;
  items[i].inList = inList ? 1 : 0;
}

// Atualiza um espelho de item somente quando o indice ainda pertence ao bloco
// atualmente publicado. A modal pode receber a resposta do worker depois que
// a descoberta trocou o catalogo; nesse caso ignorar e seguro, escrever por um
// indice antigo poderia alterar outro titulo.
void cat_update_item(int i, const CatItem *item) {
  if (!item || !items || n <= 0 || i < 0 || i >= n) return;
  items[i] = *item;
}

// Acrescenta N de UMA VEZ. cat_acrescentar copia o catalogo inteiro a cada
// chamada, e a busca a chamava POR RESULTADO: com 300 titulos no acervo sao
// ~2,3 MB por copia, vezes 40 resultados, no fio de DESENHO, a cada tecla. Era
// o travamento que aparecia como "a busca engasga quando digito".
//
// Uma troca de bloco so, seguindo a mesma ordem de cat_definir: zera `n` antes
// de trocar o ponteiro (o desenho ve catalogo vazio por um quadro em vez de ler
// memoria liberada) e nao libera o bloco velho aqui — um leitor pode estar
// dentro dele; ele morre na proxima troca.
int cat_append_lote(const CatItem *v, int count, int *outputIdx) {
  static CatItem *garbageLote;
  CatItem *new;
  int newN, k;
  if (!v || count < 1 || n < 1) return 0;
  if (n + count > CAT_MAX) count = CAT_MAX - n;
  if (count < 1) return 0;
  newN = n + count;
  new = malloc(sizeof(CatItem) * (size_t)newN);
  if (!new) return 0;
  memcpy(new, items, sizeof(CatItem) * (size_t)n);
  memcpy(&new[n], v, sizeof(CatItem) * (size_t)count);
  if (outputIdx) for (k = 0; k < count; k++) outputIdx[k] = n + k;
  free(garbageLote);
  garbageLote = items;
  items = new;
  nAllocated = newN;
  n = newN;
  ensureTracks(nAllocated);
  return count;
}

int cat_append(const CatItem *item) {
  static CatItem *garbageAccum;
  CatItem *new;
  int newN;
  if (!item || n < 1) return -1;
  if (n >= CAT_MAX) return -1;
  newN = n + 1;
  new = malloc(sizeof(CatItem) * (size_t)newN);
  if (!new) return -1;
  memcpy(new, items, sizeof(CatItem) * (size_t)n);
  memcpy(&new[n], item, sizeof(CatItem));
  free(garbageAccum);
  garbageAccum = items;
  items = new;
  nAllocated = newN;
  n = newN;
  ensureTracks(nAllocated);
  return newN - 1;
}

void cat_set(const CatItem *list, int count) {
  cat_set_all(list, count, NULL, 0);
}

void cat_set_all(const CatItem *list, int count,
                      const CatRow *newFilters, int nNew) {
  int i;
  if (!list || count < 1) return;
  // TROCA DE BLOCO, sem realloc no lugar.
  //
  // cat_definir roda no fio da descoberta enquanto o desenho le itens[] no fio
  // principal. Com realloc, o bloco antigo e LIBERADO e o desenho passa a ler
  // memoria morta — foi assim que o app comecou a morrer em home_desenhar
  // assim que o catalogo cresceu de 40 para 303. Enquanto era vetor estatico o
  // endereco nunca mudava e o problema nao existia.
  //
  // A ordem das tres linhas abaixo e o que torna isto seguro sem trava:
  // zerar `n` primeiro faz o desenho tratar o catalogo como vazio por um
  // quadro (nao desenha nada), e so depois o ponteiro e a contagem sobem. O
  // bloco antigo NAO e liberado aqui: um leitor pode estar dentro dele neste
  // instante. Ele morre na proxima troca, quando ninguem mais o alcanca.
  {
    int newN = count > CAT_MAX ? CAT_MAX : count;
    CatItem *new = malloc(sizeof(CatItem) * (size_t)newN);
    static CatItem *garbage;
    if (!new) return;
    memcpy(new, list, sizeof(CatItem) * (size_t)newN);
    // As fileiras caem JUNTO com `n`. Elas sao janelas (ini,n) no vetor de
    // itens; deixar as antigas de pe por um quadro enquanto o vetor troca faz o
    // desenho ler fora da faixa.
    n = 0;
    nFilters = 0;
    free(garbage);
    garbage = items;
    items = new;
    nAllocated = newN;
    n = newN;
    if (newFilters && nNew > 0) {
      int k, q = nNew > CAT_FILTER_MAX ? CAT_FILTER_MAX : nNew;
      int v = 0;
      for (k = 0; k < q; k++) {
        CatRow f = newFilters[k];
        // Corta a janela pelo que sobrou de verdade. Um catalogo que respondeu
        // menos itens do que o esperado deixaria a fileira apontando para o
        // vizinho.
        if (f.start < 0 || f.start >= n) continue;
        if (f.start + f.n > n) f.n = n - f.start;
        if (f.n < 1) continue;
        filters[v++] = f;
      }
      nFilters = v;
    }
  }
  // Episodios do catalogo anterior nao valem para o novo: os indices mudaram.
  nEps = 0;
  ensureTracks(nAllocated);
  (void)0;
  // O progresso vem de arquivo e e por imdb, entao sobrevive a troca — mas
  // precisa ser reaplicado, porque os itens novos nasceram zerados.
  if (dirWriting[0]) {
    char path[600], line[256];
    FILE *fp;
    snprintf(path, sizeof path, "%s/progress.txt", dirWriting);
    fp = fopen(path, "r");
    if (fp) {
      while (fgets(line, sizeof line, fp)) {
        char id[24]; double pos, duration;
        int season = 0, episode = 0;
        if (sscanf(line, "%23s %lf %lf %d %d", id, &pos, &duration, &season, &episode) < 3 || duration <= 1.0) continue;
        for (i = 0; i < n; i++) {
          size_t L = strlen(id);
          if (!strncmp(items[i].imdb, id, L) &&
              (items[i].imdb[L] == 0 || items[i].imdb[L] == ':')) {
            items[i].progress = (int)(100.0 * pos / duration);
            items[i].remainingMin = (int)((duration - pos) / 60.0 + 0.5);
            if(season>0 && episode>0) {
              if(items[i].season!=season || items[i].episode!=episode)
                items[i].nameEpisode[0]=0;
              items[i].season=season; items[i].episode=episode;
            }
            break;
          }
        }
      }
      fclose(fp);
    }
  }
}

void cat_set_episodes(int indexItem, const CatEp *list, int count) {
  int m = cat_n();
  if (!list || count < 1 || m < 1) return;
  indexItem = ((indexItem % m) + m) % m;
  if (count > CAT_EP_MAX) count = CAT_EP_MAX;
  // Anexa no fim do vetor comum. Trocar de temporada varias vezes acumula, mas
  // o teto de CAT_EP_MAX segura e o custo de compactar nao se paga.
  if (nEps + count > CAT_EP_MAX) {
    nEps = 0;
    // Invalidar os indices antes de reutilizar o armazenamento: senao outra
    // serie passa a exibir os episodios da obra que acabou de ser carregada.
    memset(epCount,0,(size_t)nAllocated*sizeof *epCount);
    memset(epStart,0,(size_t)nAllocated*sizeof *epStart);
  }
  memcpy(&eps[nEps], list, sizeof(CatEp) * (size_t)count);
  epStart[indexItem] = nEps;
  epCount[indexItem] = count;
  nEps += count;
}

// Generos de um item, como uma lista de trechos separados por " · ". O primeiro
// campo e sempre "Filme"/"Programa de TV" e nao conta como genero.
static int sharesGenre(const CatItem *a, const CatItem *b) {
  const char *p = a->genre;
  int first = 1;
  while (p && *p) {
    const char *sep = strstr(p, "\xc2\xb7");
    char term[64];
    size_t n;
    if (!sep) break;
    p = sep + 2;
    while (*p == ' ') p++;
    sep = strstr(p, "\xc2\xb7");
    n = sep ? (size_t)(sep - p) : strlen(p);
    while (n && (p[n - 1] == ' ')) n--;
    if (n && n < sizeof term) {
      memcpy(term, p, n);
      term[n] = 0;
      if (strstr(b->genre, term)) return 1;
    }
    first = 0;
    if (!sep) break;
  }
  (void)first;
  return 0;
}

int cat_similar(int index_, int *output, int max) {
  int m = cat_n(), i, k = 0;
  const CatItem *base;
  if (m < 1 || !output || max < 1) return 0;
  index_ = ((index_ % m) + m) % m;
  base = &items[index_];
  for (i = 0; i < m && k < max; i++) {
    if (i == index_) continue;
    if (base->kind[0] && items[i].kind[0] && strcmp(base->kind, items[i].kind)) continue;
    if (!sharesGenre(base, &items[i])) continue;
    output[k++] = i;
  }
  // Sem nenhum genero em comum a fileira ficaria vazia; ai vale mais mostrar os
  // vizinhos do mesmo tipo que sumir com a secao.
  for (i = 0; i < m && k < max; i++) {
    int j, already = 0;
    if (i == index_) continue;
    for (j = 0; j < k; j++) if (output[j] == i) { already = 1; break; }
    if (already) continue;
    if (base->kind[0] && items[i].kind[0] && strcmp(base->kind, items[i].kind)) continue;
    output[k++] = i;
  }
  // Nota alta primeiro.
  { int a, b, t;
    for (a = 0; a < k; a++)
      for (b = a + 1; b < k; b++)
        if (items[output[b]].score > items[output[a]].score) {
          t = output[a]; output[a] = output[b]; output[b] = t;
        } }
  return k;
}
