#include "tex_cache.h"
#include "net.h"
#include "gfx.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include "layout.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_ITEMS_ABS 512
#define MAX_QUEUE 128
#define NV_TEX_STALE_FRAMES 8
#define NV_TEX_STALE_MS 200
#define NV_TEX_UPLOAD_BUDGET_MS 4.0

// FALHOU e um estado de verdade, nao a ausencia de um. Sem ele, um caminho que
// nao decodifica volta a VAZIO, o desenho pede de novo no quadro seguinte e o
// ciclo nao termina nunca: o item ocupa uma vaga em voo e um turno na fila a
// frente das imagens boas, para falhar de novo. Com recuo, a segunda tentativa
// so acontece daqui a 2 s, a terceira a 10 s, e depois desiste na sessao.
typedef enum { EMPTY=0, PENDING, DECODED, READY, FAILED } State;

typedef struct {
  char path[512];
  unsigned long hash;  // FNV-1a do caminho, para pular o strcmp na busca
  State state;
  SDL_Surface *sup;   // preenchida pela thread; consumida no bombear
  GLuint tex;
  int w, h;
  // Teto de largura PEDIDO para este item. O hero em tela cheia precisa de 1920;
  // um poster de 212 nao. Um teto unico para todos servia mal aos dois: a 960 o
  // hero era decodificado com metade da resolucao e esticado para 1920 na tela,
  // que e o borrao que o dono viu.
  int limit;
  unsigned long uso;  // contador LRU
  // Luminancia media dos pixels OPACOS, 0..255; -1 enquanto nao se sabe.
  // Medida uma vez, na thread de decode. Serve ao logo do titulo: o TMDB nao
  // marca claro/escuro em lugar nenhum (o ranking do proprio app web e so
  // idioma + vote_average), entao a unica forma de saber se um logo e preto e
  // OLHAR os pixels.
  int luma;
  // Quando tentar de novo (ticks) e quantas vezes ja falhou. Ver o enum Estado.
  Uint32 tryIn;
  int    failures;
  unsigned long lastFrame;
  Uint32 lastRequest;
  // Croma medio: max(R,G,B) - min(R,G,B) dos mesmos pixels opacos. Separa logo
  // PRETO (acromatico, variante errada do TMDB) de logo de MARCA escuro mas
  // colorido (vermelho, vinho), que deve passar intacto.
  int chroma;
} Item;

#define NV_TEX_THREADS 2
#define NV_TEX_THREADS_NET 4
static SDL_Thread *thrs[NV_TEX_THREADS];
static SDL_Thread *thrsNet[NV_TEX_THREADS_NET];
static Item items[MAX_ITEMS_ABS];
static int nMax = 64;
static unsigned long lruClock = 1;

static SDL_mutex *mtx;
static SDL_cond  *cond;
static SDL_Thread *thr;
static int running = 0;

// Quanto de memoria as texturas PRONTAS ocupam. Sem esta conta o cache so
// despejava quando FALTAVA SLOT — e com 96 slots de arte 1920x1080 isso da 800
// MB. Com arte de verdade o app chegou a 104 MB e morreu com "double free or
// corruption" dentro do SDL: era falta de memoria, nao bug de ponteiro.
static long bytesUsed = 0;
static long budget = 0;
static unsigned long frameCurrent = 1;

// O driver tambem aloca a piramide de mipmaps. Contar apenas o nivel base
// deixava o cache ultrapassar o teto real em cerca de 33% nas artes de card.
static long bytesTexture(int w, int h) {
  long total = (long)w * h * 4;
  int mw = w, mh = h;
  if (w >= 1024) return total;
  while (mw > 1 || mh > 1) {
    mw = (mw + 1) / 2;
    mh = (mh + 1) / 2;
    total += (long)mw * mh * 4;
  }
  return total;
}

// DUAS FILAS, e a separacao e o conserto.
//
// `fila` era a unica, e o download acontecia DENTRO do fio de decodificacao
// (garantirLocal, chamado de threadDecode). Com dois fios de decode em
// prioridade baixa, cada um ficava BLOQUEADO NA REDE por ate 8 s em vez de
// decodificar — com cache frio a arte entrava a conta-gotas mesmo com a rede
// sobrando, que foi exatamente o relato: "a internet ta rapida e as artes
// demoram".
//
// Agora `fila` e a fila de REDE (baixar para o cache de disco) e `filaDec` a de
// DECODIFICACAO. Rede e espera de I/O, nao trabalho de CPU: da para ter varios
// fios sem roubar quadro do desenho. Decodificar continua com dois, em
// prioridade baixa, pelo motivo ja medido.
static int queue[MAX_QUEUE];
static int queueStart = 0, queueEnd = 0;
static int queueDec[MAX_QUEUE];
static int decStart = 0, decEnd = 0;
static SDL_cond *condDec;

// Sinalizado pelos fios de decode quando LIBERAM um lugar na fila.
static SDL_cond *condFree;

// Enfileira para DECODIFICAR. Chamado com o mutex tomado, e ESPERA quando a
// fila esta cheia.
//
// Descartar em silencio, que era o que estava aqui, deixava o item PENDENTE
// para sempre: ninguem o decodificava e nada o reenfileirava. MEDIDO com cache
// frio: as texturas subiam ate 24 e PARAVAM — quatro fios de rede enchem a fila
// mais rapido do que dois de decode a esvaziam, entao o descarte virava a regra
// e nao a excecao. Esperar aqui e o que faz a rede andar no passo do decode em
// vez de atropela-lo.
static void paraDecode(int idx) {
  for (;;) {
    int next = (decEnd + 1) % MAX_QUEUE;
    if (next != decStart) {
      queueDec[decEnd] = idx; decEnd = next;
      SDL_CondSignal(condDec);
      return;
    }
    if (!running) return;
    SDL_CondWait(condFree, mtx);
  }
}

// FNV-1a do caminho. A busca abaixo roda para cada card visivel em cada
// quadro, contra ate 96 slots; comparar um inteiro primeiro reduz o strcmp a
// so os candidatos com o mesmo hash (na pratica, o proprio item).
static unsigned long hashPath(const char *s) {
  unsigned long h = 2166136261UL;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619UL; }
  return h;
}

int    tex_n_search = 0;
double tex_ms_search = 0.0;
static double texFreqMs = 0.0;
static int requestStale(const Item *it) {
  Uint32 now;
  if (!it->lastRequest || !it->lastFrame) return 0;
  now = SDL_GetTicks();
  return frameCurrent > it->lastFrame + NV_TEX_STALE_FRAMES &&
         now - it->lastRequest >= NV_TEX_STALE_MS;
}

void tex_new_frame(void) {
  tex_n_search = 0;
  tex_ms_search = 0.0;
  (void)texFreqMs;
  if (!mtx) return;
  SDL_LockMutex(mtx);
  frameCurrent++;
  // Um decode concluido mas nunca mais desenhado nao deve ocupar memoria nem
  // bloquear a arte que entrou na tela. Pedidos PENDENTES sao cancelados pelo
  // consumidor da fila, para que o indice do slot nao seja reutilizado antes
  // de a fila o retirar.
  for (int i = 0; i < nMax; i++) {
    if (items[i].state == DECODED && requestStale(&items[i])) {
      SDL_FreeSurface(items[i].sup);
      items[i].sup = NULL;
      items[i].state = EMPTY;
      items[i].path[0] = 0;
    }
  }
  SDL_UnlockMutex(mtx);
}

// Envolve TRAVA + busca com relogio de CPU. So os chamadores da THREAD DE
// DESENHO passam por aqui; a thread de decode usa acharIndice cru, senao os
// dois fios somariam no mesmo contador e o numero deixaria de descrever o
// quadro.
//
// A TRAVA ENTRA NA CONTA, e nao so a busca. Os fios de decode rodam em
// prioridade BAIXA e pegam este mesmo mutex: um fio de decode preemptado
// SEGURANDO o mutex faz o fio de desenho esperar por ele — inversao de
// prioridade classica. Medir so o acharIndice esconderia exatamente esse custo,
// que e o unico caminho pelo qual o decode pode roubar o quadro.
//
// RESULTADO, para nao refazer a conta: com 192 slots e ~70 buscas por quadro na
// home, trava + busca linear somaram 0,04 a 0,12 ms POR QUADRO — menos de 1% de
// um quadro de 20ms. A busca linear e a inversao de prioridade estavam na lista
// de suspeitos do quadro de 22ms e as duas foram DESCARTADAS POR MEDIDA; nao
// vale trocar isto por tabela de hash. O relogio fica atras de NV_PERF_FINO
// porque custava duas leituras por consulta.
#ifdef NV_PERF_FINO
#define SEARCH_MEASURE(idx, cam, h) do { \
  if (texFreqMs == 0.0) texFreqMs = 1000.0 / (double)SDL_GetPerformanceFrequency(); \
  Uint64 t0_ = SDL_GetPerformanceCounter(); \
  SDL_LockMutex(mtx); \
  (idx) = findIndex((cam), (h)); \
  tex_ms_search += (double)(SDL_GetPerformanceCounter() - t0_) * texFreqMs; \
  tex_n_search++; \
} while (0)
#else
#define SEARCH_MEASURE(idx, cam, h) do { \
  SDL_LockMutex(mtx); \
  (idx) = findIndex((cam), (h)); \
  tex_n_search++; \
} while (0)
#endif

static int findIndex(const char *path, unsigned long h) {
  for (int i = 0; i < nMax; i++)
    if (items[i].state != EMPTY && items[i].hash == h &&
        strcmp(items[i].path, path) == 0) return i;
  return -1;
}

// Escolhe vitima LRU entre os PRONTOS. Nunca descarta o que esta em voo, senao
// a thread escreveria em cima de um slot reaproveitado.
// Quantos pedidos podem estar EM VOO ao mesmo tempo.
//
// Sem teto, uma tela cheia de arte nova pede ~90 imagens de uma vez, os slots
// enchem de PENDENTE e o slotLivre passa a descartar textura PRONTA para dar
// lugar a mais pendente — a tela FICA EM BRANCO e nao volta, porque o que
// termina e jogado fora antes de aparecer.
//
// So aparece com o cache de disco FRIO (primeira execucao, ou logo depois de
// reinstalar o app, que apaga art/cache junto). Foi assim que apareceu aqui: eu
// tinha acabado de apagar o cache a mao e o dono viu a home sem poster nenhum.
//
// Um terco dos slots mantem o fio de decodificacao ocupado e ainda deixa dois
// tercos para o que ja esta na tela.
static int emVoo(void) {
  int n = 0;
  for (int i = 0; i < nMax; i++)
    if (items[i].state == PENDING || items[i].state == DECODED) n++;
  return n;
}

static int slotFree(void) {
  if (emVoo() >= nMax / 3) return -1;   // pede de novo no proximo quadro
  for (int i = 0; i < nMax; i++) if (items[i].state == EMPTY) return i;
  // Slot que ja desistiu vale mais como vaga que um PRONTO em uso: reaproveita
  // antes de despejar arte que esta na tela.
  for (int i = 0; i < nMax; i++)
    if (items[i].state == FAILED && items[i].failures >= 3) {
      memset(&items[i], 0, sizeof(Item));
      items[i].luma = -1;
      return i;
    }
  int best = -1; unsigned long smaller = ~0UL;
  for (int i = 0; i < nMax; i++) {
    if (items[i].state != READY) continue;
    if (items[i].uso < smaller) { smaller = items[i].uso; best = i; }
  }
  if (best >= 0) {
    if (items[best].tex) { gfx_tex_forget(items[best].tex); glDeleteTextures(1, &items[best].tex); }
    bytesUsed -= bytesTexture(items[best].w, items[best].h);
    if (bytesUsed < 0) bytesUsed = 0;
    memset(&items[best], 0, sizeof(Item));
    items[best].luma = -1;   // 0 seria "preto"; o desconhecido e -1
  }
  return best;
}

// Despeja os menos usados ate caber no orcamento. Chamada com o mutex travado.
static void prune(void) {
  while (bytesUsed > budget) {
    int best = -1; unsigned long smaller = ~0UL;
    for (int i = 0; i < nMax; i++) {
      if (items[i].state != READY) continue;
      if (items[i].uso < smaller) { smaller = items[i].uso; best = i; }
    }
    if (best < 0) break;          // so restou o que esta em voo
    if (items[best].tex) { gfx_tex_forget(items[best].tex); glDeleteTextures(1, &items[best].tex); }
    bytesUsed -= bytesTexture(items[best].w, items[best].h);
    if (bytesUsed < 0) bytesUsed = 0;
    memset(&items[best], 0, sizeof(Item));
    items[best].luma = -1;   // 0 seria "preto"; o desconhecido e -1
  }
}

// Diretorio onde as imagens baixadas ficam. Uma vez baixada, a imagem vale
// para sempre: arte de filme nao muda. Sem isto cada volta a home refaria
// dezenas de downloads.
// Teto de largura da textura.
//
// 960 e nao 1280. As contas: o card de arte mede 410px e o cartao grande do
// detalhe 684; so o hero e a arte em tela cheia passam disso, e nesses dois a
// imagem ja aparece desfocada ou coberta de texto. A 1280 o cache vivia
// ENCOSTADO no teto de 72 MB (medido: 71,4 MB com 29 texturas), despejando e
// rebaixando sem parar — o que aparece como 30fps com jank em todo quadro
// depois de alguns minutos de uso. A 960 a mesma cena cabe com folga.
// Teto de decodificacao das artes de CARD.
//
// Era 960 para tudo, e a maior arte de card que a tela desenha e a miniatura de
// episodio, com 640 (NV_DETP_EP_W). Um poster de 212 de largura era decodificado
// a 960x1440 e custava 5 MB de textura — vinte deles ja passam do orcamento
// inteiro de 96 MB.
//
// Foi o que o dono viu: mexendo nas fileiras, e principalmente ao ABRIR UM
// FILME (que pede backdrop de 1920 mais miniaturas, posteres de relacionados e
// fotos de elenco de uma vez), o total estourava e o podar despejava tudo que
// estava na tela — ficava cinza e nao voltava.
//
// 640 cobre a maior arte de card sem sobra e divide o custo por 2,25: o mesmo
// poster passa a custar 2,2 MB. O hero continua com teto proprio de 1920, pela
// promocao.
#define NV_TEX_WIDTH_MAX 640
#define NV_TEX_HERO_WIDTH_MAX 1920

// TETO POR USO — o 640 acima e o padrao, e ele e GRANDE DEMAIS para a maioria
// das artes. Ele foi dimensionado pela MAIOR arte de card (a miniatura de
// episodio, 640), mas se aplica a todas: um poster desenhado com 212 de largura
// era decodificado a 640x960 e custava 2,4 MB. Com o orcamento de 96 MB isso da
// ~40 texturas — MEDIDO no aparelho: com a home rolando o log mostrava
// `texturas=40 pend=32 92.3MB`, ou seja, a fila entupida e o cache ja
// despejando o que ainda estava na tela para caber o que entrava.
//
// Esse e o "carrega as coisas enquanto passa": nao e rede nem decode lento, e o
// cache batendo no teto e re-decodificando o que acabou de despejar.
//
// Aqui cada chamador pede pela LARGURA COM QUE DESENHA, e a conta vira
// largura * escala do buffer * folga. Na TV a escala e 1 (drawable=1920x1080,
// medido) e um poster passa a custar ~420 KB em vez de 2,4 MB — quase seis
// vezes mais arte no mesmo orcamento. No Mac retina a escala e 2 e a previa
// continua nitida.
//
// A FOLGA de 1,25 cobre o card que cresce ao receber foco (escala ~1,08) e
// evita reamostrar no limite exato, que serrilha.
#define NV_TEX_SLACK 1.25f
static float scaleBuf = 1.0f;

void tex_scale(float e) {
  if (e > 0.1f && e < 8.0f) scaleBuf = e;
}

static char dirCache[512];

void tex_cache_dir(const char *dir) {
  if (!dir || !*dir) return;
  snprintf(dirCache, sizeof dirCache, "%s", dir);
  mkdir(dirCache, 0777);
  // A PASTA PODE EXISTIR E NAO SER GRAVAVEL, e isso ja aconteceu: o tools/arm.sh
  // manda art/ num tar feito no Mac, e o tar extraido como root na TV carimba
  // o dono com o uid do Mac (13888160) e modo 755. O app roda como uid 5152,
  // entao depois de CADA deploy a pasta ficava so-leitura.
  //
  // O efeito era invisivel: garantirLocal baixava a imagem, o fopen do
  // temporario falhava, ela devolvia 0 sem dizer nada, e o unico sintoma era
  // "card sem arte". Medido: 91 "decode falhou" numa navegacao, com ZERO erro
  // de rede — as imagens chegavam e eram jogadas fora. Cada uma volta a ser
  // pedida ate o terceiro recuo, entao os dois fios de decode ficam ocupados
  // baixando o que nunca vai poder ser guardado.
  //
  // O app nao consegue consertar (chmod de quem nao e dono falha), mas TEM de
  // dizer. Sem esta linha o defeito nao tem como ser atribuido a causa.
  if (access(dirCache, W_OK) != 0)
    printf("[tex] CACHE FOLDER NOT WRITABLE: %s — every downloaded image will be"
           " discarded (check the owner; the deploy stamps the Mac uid)\n",
           dirCache);
  fflush(stdout);
}

// Nome de arquivo estavel a partir da URL. Hash simples (FNV-1a) e nao o nome
// da URL porque elas trazem barra, query e caracteres que nao cabem em nome de
// arquivo — e porque duas URLs diferentes precisam de arquivos diferentes.
static void nameOfCache(const char *url, char *dst, size_t size) {
  unsigned long h = 2166136261UL;
  const char *p = url, *dot = strrchr(url, '.');
  char ext[8] = ".jpg";
  for (; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; }
  if (dot && strlen(dot) <= 5 && !strchr(dot, '/'))
    snprintf(ext, sizeof ext, "%s", dot);
  snprintf(dst, size, "%s/%08lx%s", dirCache, h, ext);
}

// Baixa a URL para o cache, se ainda nao estiver la. Devolve 1 se ha arquivo
// utilizavel no fim. Roda no fio de decodificacao, entao bloquear aqui nao
// custa quadro nenhum.
static int ensureLocal(const char *url, char *dst, size_t size) {
  FILE *f;
  char *body;
  long n = 0;
  if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
    snprintf(dst, size, "%s", url);
    return 1;
  }
  if (!dirCache[0]) return 0;
  nameOfCache(url, dst, size);
  f = fopen(dst, "rb");
  if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); if (n > 512) return 1; }
  // 8 s e nao 25: isto e uma IMAGEM. Com 25 s, duas URLs mortas seguravam os
  // dois fios de decode por quase um minuto e a tela inteira parava de receber
  // arte — repetidamente, porque nada guarda a falha.
  body = net_download_bin(url, 8, &n);
  // ESTE RAMO ERA MUDO. Medido numa navegacao da home: 93 "decode falhou" com
  // ZERO "[rede] falha" no log — todas as falhas passavam por aqui, com o curl
  // dizendo sucesso e um corpo curto demais para ser imagem. Sem a linha nao
  // havia como distinguir "servidor recusou" de "cache sem permissao de
  // escrita" de "resposta vazia". Nao repete o caso do HTTP >= 400, que agora
  // o rede.c nomeia sozinho.
  if (!body || n <= 512) {
    if (body) { printf("[tex] body too short (%ld B): %.70s\n", n, url); fflush(stdout); }
    free(body);
    return 0;
  }
  // ASSINATURA DE IMAGEM. rede.c nao confere status HTTP, entao um 404 com
  // pagina de erro de mais de 512 bytes era gravado como "imagem" e ficava no
  // cache de disco PARA SEMPRE — o item nunca mais teria arte, nem depois de o
  // servidor voltar. Aceita JPEG (FF D8), PNG (89 50 4E 47), GIF e RIFF/WEBP.
  { const unsigned char *b0 = (const unsigned char *)body;
    int ok = (n > 4) && (
       (b0[0] == 0xFF && b0[1] == 0xD8) ||
       (b0[0] == 0x89 && b0[1] == 0x50 && b0[2] == 0x4E && b0[3] == 0x47) ||
       (b0[0] == 'G'  && b0[1] == 'I'  && b0[2] == 'F') ||
       (b0[0] == 'R'  && b0[1] == 'I'  && b0[2] == 'F'  && b0[3] == 'F'));
    if (!ok) {
      printf("[tex] response is not an image (%ld B): %.70s\n", n, url);
      fflush(stdout);
      free(body);
      return 0;
    } }
  { char tmp[600];
    // Grava em temporario e renomeia: outro fio pode estar lendo o mesmo
    // arquivo, e um arquivo pela metade decodifica como imagem quebrada e fica
    // em cache assim para sempre.
    snprintf(tmp, sizeof tmp, "%s.partial", dst);
    f = fopen(tmp, "wb");
    // Falhava em SILENCIO. Ver a nota em tex_cache_dir: pasta sem permissao de
    // escrita joga fora toda imagem baixada e o unico sintoma era card cinza.
    if (!f) { printf("[tex] could not write %.80s\n", tmp); fflush(stdout);
              free(body); return 0; }
    fwrite(body, 1, (size_t)n, f);
    fclose(f);
    rename(tmp, dst);
  }
  free(body);
  return 1;
}

// FIO DE REDE: tira da fila, garante o arquivo no cache de disco e passa para a
// decodificacao. Nao toca em pixel nenhum, entao pode rodar em prioridade
// normal e em varios — o que ele faz e ESPERAR.
static int threadNet(void *arg) {
  (void)arg;
  for (;;) {
    int idx;
    char path[512], local[600];
    SDL_LockMutex(mtx);
    while (running && queueStart == queueEnd) SDL_CondWait(cond, mtx);
    if (!running) { SDL_UnlockMutex(mtx); return 0; }
    idx = queue[queueStart]; queueStart = (queueStart + 1) % MAX_QUEUE;
    if (items[idx].state != PENDING || requestStale(&items[idx])) {
      if (items[idx].state == PENDING) {
        items[idx].state = EMPTY;
        items[idx].path[0] = 0;
      }
      SDL_UnlockMutex(mtx);
      continue;
    }
    strncpy(path, items[idx].path, sizeof path - 1);
    path[sizeof path - 1] = 0;
    SDL_UnlockMutex(mtx);

    // Caminho local devolve na hora; so URL sai para a rede.
    SDL_LockMutex(mtx);
    if (items[idx].state != PENDING || requestStale(&items[idx])) {
      if (items[idx].state == PENDING) {
        items[idx].state = EMPTY;
        items[idx].path[0] = 0;
      }
      SDL_UnlockMutex(mtx);
      continue;
    }
    SDL_UnlockMutex(mtx);

    if (!ensureLocal(path, local, sizeof local)) {
      // Falhou o download. Marca como falha AQUI para o recuo valer — antes o
      // decode e que marcava, e ate la o item ficava PENDENTE ocupando slot.
      SDL_LockMutex(mtx);
      if (items[idx].state != PENDING || requestStale(&items[idx])) {
        if (items[idx].state == PENDING) {
          items[idx].state = EMPTY;
          items[idx].path[0] = 0;
        }
        SDL_UnlockMutex(mtx);
        continue;
      }
      items[idx].state = FAILED;
      items[idx].failures++;
      items[idx].tryIn = SDL_GetTicks() +
          (items[idx].failures == 1 ? 2000 : (items[idx].failures == 2 ? 10000 : 60000));
      SDL_UnlockMutex(mtx);
      continue;
    }
    SDL_LockMutex(mtx);
    if (items[idx].state != PENDING || requestStale(&items[idx])) {
      if (items[idx].state == PENDING) {
        items[idx].state = EMPTY;
        items[idx].path[0] = 0;
      }
      SDL_UnlockMutex(mtx);
      continue;
    }
    paraDecode(idx);
    SDL_UnlockMutex(mtx);
  }
}

static int threadDecode(void *arg) {
  (void)arg;
  // PRIORIDADE BAIXA, e isto nao e detalhe.
  //
  // Medido: durante a navegacao o pior quadro cravava em 42 ms e os janks
  // batiam EXATAMENTE com `pend>0` — ou seja, com este fio trabalhando. Ele
  // decodifica JPEG e reduz a imagem com SDL_BlitScaled, tudo em CPU, e nesta
  // TV sao quatro nucleos fracos: com prioridade igual, ele rouba o quadro do
  // desenho. Arte que aparece um instante depois ninguem nota; o tranco, sim.
  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
  for (;;) {
    SDL_LockMutex(mtx);
    while (running && decStart == decEnd) SDL_CondWait(condDec, mtx);
    if (!running) { SDL_UnlockMutex(mtx); return 0; }
    int idx = queueDec[decStart]; decStart = (decStart + 1) % MAX_QUEUE;
    SDL_CondSignal(condFree);   // abriu lugar: solta um fio de rede que espera
    if (items[idx].state != PENDING || requestStale(&items[idx])) {
      if (items[idx].state == PENDING) {
        items[idx].state = EMPTY;
        items[idx].path[0] = 0;
      }
      SDL_UnlockMutex(mtx);
      continue;
    }
    char path[512];
    int limit;
    strncpy(path, items[idx].path, sizeof path - 1);
    path[sizeof path - 1] = 0;
    // Copiado SOB O MUTEX: o item pode ser promovido a hero enquanto este fio
    // decodifica, e ler o campo depois daria uma leitura sem trava.
    limit = items[idx].limit > 0 ? items[idx].limit : NV_TEX_WIDTH_MAX;
    SDL_UnlockMutex(mtx);

    // O download JA ACONTECEU no fio de rede; aqui garantirLocal so traduz a
    // URL para o caminho do cache, sem tocar a rede.
    { char local[600];
      if (ensureLocal(path, local, sizeof local))
        snprintf(path, sizeof path, "%s", local);
    }
    SDL_Surface *bruta = IMG_Load(path);
    SDL_Surface *conv = NULL;
    if (bruta) {
      conv = SDL_ConvertSurfaceFormat(bruta, SDL_PIXELFORMAT_ABGR8888, 0);
      SDL_FreeSurface(bruta);
    }
    // TETO DE LARGURA. Antes a arte vinha do pacote ja reduzida; agora vem da
    // rede no tamanho que o servidor tiver, e um backdrop de 1920 custa 8 MB
    // DECODIFICADO — meia duzia deles estoura o orcamento e o cache passa a
    // despejar e rebaixar em circulo, o que aparece como queda de fps e quadros
    // de 100 ms. Reduzir aqui vale para qualquer fonte, presente ou futura.
    if (conv && conv->w > limit) {
      int lw = limit;
      int lh = conv->h * lw / conv->w;
      SDL_Surface *smaller = SDL_CreateRGBSurfaceWithFormat(
          0, lw, lh, 32, SDL_PIXELFORMAT_ABGR8888);
      if (smaller) {
        // BlitScaled faz media dos vizinhos; um decimador ingenuo deixaria a
        // arte serrilhada, que foi exatamente o defeito do fundo pixelado.
        SDL_BlitScaled(conv, NULL, smaller, NULL);
        SDL_FreeSurface(conv);
        conv = smaller;
      }
    }

    // MEDIDA DE LUMINANCIA, aqui e nao no desenho: esta thread ja tem os pixels
    // na mao e roda em prioridade baixa. Amostra de 4 em 4 nos dois eixos —
    // 1/16 dos pixels bastam para dizer se uma arte e escura, e a conta inteira
    // num logo de 700x271 seria trabalho sem retorno.
    int lumaMedia = -1, chromaMedia = 0;
    if (conv && conv->format->BytesPerPixel == 4) {
      const unsigned char *px = (const unsigned char *)conv->pixels;
      long sum = 0, sumC = 0, n = 0;
      int yy, xx;
      for (yy = 0; yy < conv->h; yy += 4) {
        const unsigned char *ln = px + (size_t)yy * conv->pitch;
        for (xx = 0; xx < conv->w; xx += 4) {
          const unsigned char *q = ln + (size_t)xx * 4;   // ABGR8888: R,G,B,A
          if (q[3] < 200) continue;                       // so o que e opaco
          sum += (q[0] * 299 + q[1] * 587 + q[2] * 114) / 1000;
          { int mx = q[0] > q[1] ? q[0] : q[1]; if (q[2] > mx) mx = q[2];
            int mn = q[0] < q[1] ? q[0] : q[1]; if (q[2] < mn) mn = q[2];
            sumC += mx - mn; }
          n++;
        }
      }
      if (n > 0) { lumaMedia = (int)(sum / n); chromaMedia = (int)(sumC / n); }
    }

    // A FALHA PRECISA APARECER. Sem log, uma imagem que nunca decodifica vira
    // um laco silencioso: o desenho pede todo quadro, o fio tenta todo quadro,
    // e o unico sintoma e "esse card nao tem arte". Foi assim que o WEBP do
    // metahub passou despercebido.
    int failed = 0;
    SDL_LockMutex(mtx);
    if (items[idx].state == PENDING && requestStale(&items[idx])) {
      // A imagem terminou depois de o card sair da tela. Nao a publique e nao
      // a transforme em falha: outro card pode reutilizar o slot frio.
      items[idx].state = EMPTY;
      items[idx].path[0] = 0;
      if (conv) { SDL_FreeSurface(conv); conv = NULL; }
    } else if (items[idx].state == PENDING) {
      items[idx].luma = lumaMedia;
      items[idx].chroma = chromaMedia;
      items[idx].sup = conv;
      if (conv) {
        items[idx].state = DECODED;
        items[idx].failures = 0;
      } else {
        // MANTEM o caminho: e ele que identifica o slot na proxima consulta e
        // permite responder "ainda nao, tente depois" em vez de reenfileirar.
        // Zerar o caminho, como estava, apagava a memoria da falha junto.
        static const Uint32 INSET[3] = { 2000, 10000, 60000 };
        int k = items[idx].failures;
        items[idx].state = FAILED;
        items[idx].failures = k + 1;
        items[idx].tryIn = SDL_GetTicks() + INSET[k < 3 ? k : 2];
        failed = 1;
      }
    } else if (conv) {
      SDL_FreeSurface(conv);  // slot foi reaproveitado no meio do caminho
    }
    SDL_UnlockMutex(mtx);

    if (failed) {
      printf("[tex] decode failed (%s): %.70s\n", IMG_GetError(), path);
      fflush(stdout);
      // Arquivo LOCAL que nao decodifica esta envenenado: garantirLocal o
      // aceita para sempre por ter mais de 512 bytes, entao sem apagar aqui o
      // item nunca mais teria arte. Só apaga o que esta no NOSSO cache.
      if (dirCache[0] && !strncmp(path, dirCache, strlen(dirCache)))
        remove(path);
    }
  }
}

int tex_start(int max_items) {
  nMax = max_items > 0 && max_items <= MAX_ITEMS_ABS ? max_items : 64;
  // ORCAMENTO ESCALA COM O BUFFER. Os 96 MB sao a conta da TV, onde a escala e
  // 1. No Mac retina a escala e 2, e a MESMA cena precisa de 4x os pixels — com
  // o teto fixo a previa vivia encostada no limite, despejando arte visivel e
  // mostrando um defeito que o aparelho nao tem. Uma previa que mente e pior
  // que nao ter previa.
  //
  // Na TV o fator e 1 e nada muda; e exatamente por isso que a conta pode ser
  // esta e nao um numero maior cravado.
  { float e = scaleBuf > 0.1f ? scaleBuf : 1.0f;
    budget = (long)(NV_TEX_BUDGET_MB * e * e) * 1024L * 1024L; }
  bytesUsed = 0;
  memset(items, 0, sizeof items);
  mtx = SDL_CreateMutex(); cond = SDL_CreateCond();
  condDec = SDL_CreateCond(); condFree = SDL_CreateCond();
  running = 1;
  // DOIS fios de decode, nao um. A fila e retirada sob o mutex e cada fio leva
  // um indice proprio, entao mais consumidores e seguro sem outra mudanca.
  //
  // Um fio so era o limite real do "carrega enquanto passa": com 32 itens em
  // voo (o teto de slotLivre) e ~30 ms por imagem nesta TV, a fila levava um
  // segundo para escoar. Dois fios cortam isso pela metade.
  //
  // Nao mais que dois: sao quatro nucleos fracos, e os dois rodam em prioridade
  // BAIXA justamente para nao roubar o quadro do desenho — a nota acima, no
  // threadDecode, registra que com prioridade igual o tranco batia exatamente
  // com `pend>0`. Quatro fios competiriam com o desenho mesmo em prioridade
  // baixa.
  { int k;
    for (k = 0; k < NV_TEX_THREADS; k++)
      thrs[k] = SDL_CreateThread(threadDecode, "nv-decode", NULL);
    // QUATRO fios de REDE, e eles NAO sao como os de decode: nao tocam pixel,
    // so esperam I/O. Podem rodar em prioridade normal e em maior numero sem
    // competir com o desenho — o custo de um fio parado num socket e zero de
    // CPU. Quatro cobre os quatro cartazes que entram na tela de uma vez.
    for (k = 0; k < NV_TEX_THREADS_NET; k++)
      thrsNet[k] = SDL_CreateThread(threadNet, "nv-net", NULL);
    thr = thrs[0]; }
  return thr != NULL;
}

void tex_shutdown(void) {
  SDL_LockMutex(mtx); running = 0;
  SDL_CondBroadcast(cond); SDL_CondBroadcast(condDec);
  SDL_CondBroadcast(condFree);
  SDL_UnlockMutex(mtx);
  // Espera os DOIS fios. Esperar so o primeiro deixava o outro decodificando
  // para dentro de itens[] enquanto o laco abaixo ja liberava as superficies.
  { int k;
    for (k = 0; k < NV_TEX_THREADS; k++)
      if (thrs[k]) { SDL_WaitThread(thrs[k], NULL); thrs[k] = NULL; }
    for (k = 0; k < NV_TEX_THREADS_NET; k++)
      if (thrsNet[k]) { SDL_WaitThread(thrsNet[k], NULL); thrsNet[k] = NULL; }
    thr = NULL; }
  for (int i = 0; i < nMax; i++) {
    if (items[i].tex) glDeleteTextures(1, &items[i].tex);
    if (items[i].sup) SDL_FreeSurface(items[i].sup);
  }
  SDL_DestroyCond(cond); SDL_DestroyMutex(mtx);
}

static GLuint tex_get_limit(const char *path, int limit) {
  if (!path || !*path) return 0;
  GLuint output = 0;
  unsigned long h = hashPath(path);
  int i; SEARCH_MEASURE(i, path, h);
  if (i >= 0 && items[i].state == FAILED) {
    items[i].lastFrame = frameCurrent;
    items[i].lastRequest = SDL_GetTicks();
    // Ja falhou: so volta para a fila quando o recuo vencer, e nunca depois da
    // terceira tentativa. Sem isto o pedido voltava a cada quadro.
    if (items[i].failures < 3 && SDL_GetTicks() >= items[i].tryIn) {
      int next = (queueEnd + 1) % MAX_QUEUE;
      if (next != queueStart) {
        items[i].state = PENDING;
        items[i].uso = ++lruClock;
        queue[queueEnd] = i; queueEnd = next; SDL_CondSignal(cond);
      }
    }
    SDL_UnlockMutex(mtx);
    return 0;
  }
  if (i >= 0) {
    items[i].lastFrame = frameCurrent;
    items[i].lastRequest = SDL_GetTicks();
    items[i].uso = ++lruClock;
    // PROMOCAO: a mesma arte pode ser pedida como poster (960) e depois como
    // hero (1920). Se o teto novo e maior e a textura pronta ficou menor que
    // ele, refaz — senao o hero herda para sempre a versao pequena que o card
    // pediu primeiro, e o borrao volta sem explicacao aparente.
    if (limit > items[i].limit) {
      int smallerFont = (items[i].state == READY && items[i].w < items[i].limit);
      items[i].limit = limit;
      // `w < limite` NAO basta: um poster da Cinemeta tem 250px de origem, e
      // pedi-lo a 320 refaz o decode para devolver os mesmos 250 — trabalho
      // puro, mais o cinza enquanto refaz. Se a textura pronta ja e MENOR que o
      // teto que ela mesma tinha, a fonte acabou; nao ha o que ganhar.
      if (items[i].state == READY && items[i].w < limit && !smallerFont) {
        int next = (queueEnd + 1) % MAX_QUEUE;
        if (next != queueStart) {
          items[i].state = PENDING;
          queue[queueEnd] = i; queueEnd = next; SDL_CondSignal(cond);
        }
      }
    }
    output = items[i].state == READY ? items[i].tex : 0;
  } else {
    int new = slotFree();
    if (new >= 0) {
      strncpy(items[new].path, path, sizeof items[new].path - 1);
      items[new].hash = h;
      items[new].limit = limit;
      items[new].state = PENDING;
      items[new].uso = ++lruClock;
      items[new].lastFrame = frameCurrent;
      items[new].lastRequest = SDL_GetTicks();
      int next = (queueEnd + 1) % MAX_QUEUE;
      if (next != queueStart) { queue[queueEnd] = new; queueEnd = next; SDL_CondSignal(cond); }
      else { items[new].state = EMPTY; items[new].path[0] = 0; } // fila cheia
    }
  }
  SDL_UnlockMutex(mtx);
  return output;
}

GLuint tex_get(const char *path) {
  return tex_get_limit(path, NV_TEX_WIDTH_MAX);
}

// Arte que ocupa a tela inteira: hero da home, backdrop do detalhe e a arte do
// player. 1920 e a largura do painel — pedir mais so gastaria memoria, pedir
// menos e ampliar depois.
GLuint tex_get_width(const char *path, float widthLayout) {
  int cap;
  if (widthLayout <= 1.0f) return tex_get(path);
  cap = (int)(widthLayout * scaleBuf * NV_TEX_SLACK + 0.5f);
  // Arredonda para multiplo de 32: sem isso cada largura de desenho vira um
  // teto proprio, e a mesma arte pedida por dois lugares com poucos pixels de
  // diferenca era promovida e RE-DECODIFICADA sem ganho visivel.
  cap = ((cap + 31) / 32) * 32;
  if (cap < 128) cap = 128;
  if (cap > NV_TEX_HERO_WIDTH_MAX) cap = NV_TEX_HERO_WIDTH_MAX;
  return tex_get_limit(path, cap);
}

GLuint tex_get_hero(const char *path) {
  return tex_get_limit(path, NV_TEX_HERO_WIDTH_MAX);
}

float tex_aspect(const char *path) {
  if (!path || !*path) return 0.0f;
  float a = 0.0f;
  unsigned long h = hashPath(path);
  int i; SEARCH_MEASURE(i, path, h);
  if (i >= 0 && items[i].state == READY && items[i].h > 0)
    a = (float)items[i].w / (float)items[i].h;
  SDL_UnlockMutex(mtx);
  return a;
}

int tex_brand_dark(const char *path) {
  int r = 0;
  unsigned long h;
  int i;
  if (!path || !*path) return 0;
  h = hashPath(path);
  SEARCH_MEASURE(i, path, h);
  // lum < 0 e "ainda nao medi": responde NAO, para nao tingir arte que ainda
  // vai chegar. Errar para o lado de nao mexer.
  if (i >= 0 && items[i].state == READY && items[i].luma >= 0)
    r = (items[i].luma < NV_LOGO_LUMA_MIN && items[i].chroma < NV_LOGO_CHROMA_MAX);
  SDL_UnlockMutex(mtx);
  return r;
}

int tex_pump(int max_por_frame) {
  int rose = 0;
  Uint64 start = SDL_GetPerformanceCounter();
  double freq = (double)SDL_GetPerformanceFrequency();
  for (int step = 0; step < max_por_frame; step++) {
    if (rose > 0 &&
        (double)(SDL_GetPerformanceCounter() - start) * 1000.0 / freq >=
            NV_TEX_UPLOAD_BUDGET_MS)
      break;
    SDL_Surface *sup = NULL; int target = -1;
    SDL_LockMutex(mtx);
    for (int i = 0; i < nMax; i++) {
      if (items[i].state == DECODED && items[i].sup) {
        sup = items[i].sup; items[i].sup = NULL; target = i; break;
      }
    }
    SDL_UnlockMutex(mtx);
    if (!sup) break;

    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sup->w, sup->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, sup->pixels);
    // Mipmaps servem a duas coisas: reduzir o serrilhado quando a arte aparece
  // menor que o original, e — a razao de terem entrado agora — permitir o fundo
  // desfocado da pagina de detalhe. Em GLES2 nao ha blur barato; amostrar um
  // nivel pequeno da piramide com bias e o blur.
  // MIPMAP SO NO QUE ENCOLHE NA TELA. A piramide custa +33% de memoria de GPU
  // sobre a textura, e `bytesUsados` NAO a conta — com o orcamento em 96 MB, o
  // uso real chegava perto de 128 MB e o driver e que decidia o que despejar.
  //
  // Arte de tela cheia (heroi, backdrop) e desenhada 1:1 ou AMPLIADA: nivel
  // menor da piramide nunca e amostrado, entao ali a piramide e custo puro. Os
  // cards, sim, aparecem menores que o decodificado e precisam dela.
  //
  // Sem mipmap o filtro TEM de ser GL_LINEAR: com MIPMAP_NEAREST numa textura
  // sem piramide a amostragem e indefinida e a textura sai PRETA.
  int comMip = (sup->w < 1024);
  if (comMip) glGenerateMipmap(GL_TEXTURE_2D);
  // MIPMAP_NEAREST e nao _LINEAR: o trilinear le DOIS niveis da piramide por
  // amostra, e nesta GPU isso e o dobro do custo de textura em cada pixel de
  // cada card. A diferenca visual e um degrau na transicao entre niveis, que
  // so apareceria numa animacao de zoom continuo — que o app nao faz.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  comMip ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gfx_tex_forget(0);  // o bind do upload passou por fora do gfx_rect

    SDL_LockMutex(mtx);
    // PROMOCAO VAZAVA. Quando a mesma arte e pedida com um teto maior (poster a
    // 288 e depois heroi a 1920), o item volta a PENDENTE e passa por aqui de
    // novo — e esta linha sobrescrevia `tex` sem apagar a textura antiga e
    // SOMAVA os bytes novos sem subtrair os velhos.
    //
    // Duas consequencias, as duas silenciosas: uma textura orfa ficava na GPU a
    // cada promocao, e `bytesUsados` so crescia com bytes-fantasma. Com o
    // orcamento inflado, podar() passava a despejar cada vez mais cedo — ate
    // despejar arte que estava na tela, um quadro depois de ela subir. Era mais
    // uma fonte de "poster que some".
    if (items[target].tex) {
      gfx_tex_forget(items[target].tex);
      glDeleteTextures(1, &items[target].tex);
    bytesUsed -= bytesTexture(items[target].w, items[target].h);
      if (bytesUsed < 0) bytesUsed = 0;
    }
    items[target].tex = t; items[target].w = sup->w; items[target].h = sup->h;
    items[target].state = READY;
    bytesUsed += bytesTexture(sup->w, sup->h);
    prune();
    SDL_UnlockMutex(mtx);
    SDL_FreeSurface(sup);
    rose++;
  }
  return rose;
}

void tex_stats(int *nItems, int *nPending, long *bytes) {
  int a=0, p=0; long b=0;
  SDL_LockMutex(mtx);
  for (int i = 0; i < nMax; i++) {
    if (items[i].state == READY) { a++; b += bytesTexture(items[i].w, items[i].h); }
    else if (items[i].state != EMPTY) p++;
  }
  SDL_UnlockMutex(mtx);
  if (nItems) *nItems = a;
  if (nPending) *nPending = p;
  if (bytes) *bytes = b;
}
