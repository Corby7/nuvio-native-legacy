#include "tex_cache.h"
#include "rede.h"
#include "gfx.h"
#include <sys/stat.h>
#include <stdlib.h>
#include "layout.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_ITENS_ABS 512
#define MAX_FILA 128

typedef enum { VAZIO=0, PENDENTE, DECODIFICADO, PRONTO } Estado;

typedef struct {
  char caminho[512];
  unsigned long hash;  // FNV-1a do caminho, para pular o strcmp na busca
  Estado estado;
  SDL_Surface *sup;   // preenchida pela thread; consumida no bombear
  GLuint tex;
  int w, h;
  // Teto de largura PEDIDO para este item. O hero em tela cheia precisa de 1920;
  // um poster de 212 nao. Um teto unico para todos servia mal aos dois: a 960 o
  // hero era decodificado com metade da resolucao e esticado para 1920 na tela,
  // que e o borrao que o dono viu.
  int limite;
  unsigned long uso;  // contador LRU
} Item;

static Item itens[MAX_ITENS_ABS];
static int nMax = 64;
static unsigned long relogio = 1;

static SDL_mutex *mtx;
static SDL_cond  *cond;
static SDL_Thread *thr;
static int rodando = 0;

// Quanto de memoria as texturas PRONTAS ocupam. Sem esta conta o cache so
// despejava quando FALTAVA SLOT — e com 96 slots de arte 1920x1080 isso da 800
// MB. Com arte de verdade o app chegou a 104 MB e morreu com "double free or
// corruption" dentro do SDL: era falta de memoria, nao bug de ponteiro.
static long bytesUsados = 0;
static long orcamento = 0;

static int fila[MAX_FILA];
static int filaIni = 0, filaFim = 0;

// FNV-1a do caminho. A busca abaixo roda para cada card visivel em cada
// quadro, contra ate 96 slots; comparar um inteiro primeiro reduz o strcmp a
// so os candidatos com o mesmo hash (na pratica, o proprio item).
static unsigned long hashCaminho(const char *s) {
  unsigned long h = 2166136261UL;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619UL; }
  return h;
}

static int acharIndice(const char *caminho, unsigned long h) {
  for (int i = 0; i < nMax; i++)
    if (itens[i].estado != VAZIO && itens[i].hash == h &&
        strcmp(itens[i].caminho, caminho) == 0) return i;
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
    if (itens[i].estado == PENDENTE || itens[i].estado == DECODIFICADO) n++;
  return n;
}

static int slotLivre(void) {
  if (emVoo() >= nMax / 3) return -1;   // pede de novo no proximo quadro
  for (int i = 0; i < nMax; i++) if (itens[i].estado == VAZIO) return i;
  int melhor = -1; unsigned long menor = ~0UL;
  for (int i = 0; i < nMax; i++) {
    if (itens[i].estado != PRONTO) continue;
    if (itens[i].uso < menor) { menor = itens[i].uso; melhor = i; }
  }
  if (melhor >= 0) {
    if (itens[melhor].tex) { gfx_tex_esquecer(itens[melhor].tex); glDeleteTextures(1, &itens[melhor].tex); }
    bytesUsados -= (long)itens[melhor].w * itens[melhor].h * 4;
    if (bytesUsados < 0) bytesUsados = 0;
    memset(&itens[melhor], 0, sizeof(Item));
  }
  return melhor;
}

// Despeja os menos usados ate caber no orcamento. Chamada com o mutex travado.
static void podar(void) {
  while (bytesUsados > orcamento) {
    int melhor = -1; unsigned long menor = ~0UL;
    for (int i = 0; i < nMax; i++) {
      if (itens[i].estado != PRONTO) continue;
      if (itens[i].uso < menor) { menor = itens[i].uso; melhor = i; }
    }
    if (melhor < 0) break;          // so restou o que esta em voo
    if (itens[melhor].tex) { gfx_tex_esquecer(itens[melhor].tex); glDeleteTextures(1, &itens[melhor].tex); }
    bytesUsados -= (long)itens[melhor].w * itens[melhor].h * 4;
    if (bytesUsados < 0) bytesUsados = 0;
    memset(&itens[melhor], 0, sizeof(Item));
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
#define NV_TEX_LARG_MAX 960
#define NV_TEX_HERO_LARG_MAX 1920

static char dirCache[512];

void tex_cache_dir(const char *dir) {
  if (!dir || !*dir) return;
  snprintf(dirCache, sizeof dirCache, "%s", dir);
  mkdir(dirCache, 0777);
}

// Nome de arquivo estavel a partir da URL. Hash simples (FNV-1a) e nao o nome
// da URL porque elas trazem barra, query e caracteres que nao cabem em nome de
// arquivo — e porque duas URLs diferentes precisam de arquivos diferentes.
static void nomeDeCache(const char *url, char *dst, size_t tam) {
  unsigned long h = 2166136261UL;
  const char *p = url, *ponto = strrchr(url, '.');
  char ext[8] = ".jpg";
  for (; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; }
  if (ponto && strlen(ponto) <= 5 && !strchr(ponto, '/'))
    snprintf(ext, sizeof ext, "%s", ponto);
  snprintf(dst, tam, "%s/%08lx%s", dirCache, h, ext);
}

// Baixa a URL para o cache, se ainda nao estiver la. Devolve 1 se ha arquivo
// utilizavel no fim. Roda no fio de decodificacao, entao bloquear aqui nao
// custa quadro nenhum.
static int garantirLocal(const char *url, char *dst, size_t tam) {
  FILE *f;
  char *corpo;
  long n = 0;
  if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
    snprintf(dst, tam, "%s", url);
    return 1;
  }
  if (!dirCache[0]) return 0;
  nomeDeCache(url, dst, tam);
  f = fopen(dst, "rb");
  if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); if (n > 512) return 1; }
  corpo = rede_baixar_bin(url, 25, &n);
  if (!corpo || n <= 512) { free(corpo); return 0; }
  { char tmp[600];
    // Grava em temporario e renomeia: outro fio pode estar lendo o mesmo
    // arquivo, e um arquivo pela metade decodifica como imagem quebrada e fica
    // em cache assim para sempre.
    snprintf(tmp, sizeof tmp, "%s.parcial", dst);
    f = fopen(tmp, "wb");
    if (!f) { free(corpo); return 0; }
    fwrite(corpo, 1, (size_t)n, f);
    fclose(f);
    rename(tmp, dst);
  }
  free(corpo);
  return 1;
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
    while (rodando && filaIni == filaFim) SDL_CondWait(cond, mtx);
    if (!rodando) { SDL_UnlockMutex(mtx); return 0; }
    int idx = fila[filaIni]; filaIni = (filaIni + 1) % MAX_FILA;
    char caminho[512];
    int limite;
    strncpy(caminho, itens[idx].caminho, sizeof caminho - 1);
    caminho[sizeof caminho - 1] = 0;
    // Copiado SOB O MUTEX: o item pode ser promovido a hero enquanto este fio
    // decodifica, e ler o campo depois daria uma leitura sem trava.
    limite = itens[idx].limite > 0 ? itens[idx].limite : NV_TEX_LARG_MAX;
    SDL_UnlockMutex(mtx);

    { char local[600];
      if (garantirLocal(caminho, local, sizeof local))
        snprintf(caminho, sizeof caminho, "%s", local);
    }
    SDL_Surface *bruta = IMG_Load(caminho);
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
    if (conv && conv->w > limite) {
      int lw = limite;
      int lh = conv->h * lw / conv->w;
      SDL_Surface *menor = SDL_CreateRGBSurfaceWithFormat(
          0, lw, lh, 32, SDL_PIXELFORMAT_ABGR8888);
      if (menor) {
        // BlitScaled faz media dos vizinhos; um decimador ingenuo deixaria a
        // arte serrilhada, que foi exatamente o defeito do fundo pixelado.
        SDL_BlitScaled(conv, NULL, menor, NULL);
        SDL_FreeSurface(conv);
        conv = menor;
      }
    }

    SDL_LockMutex(mtx);
    if (itens[idx].estado == PENDENTE) {
      itens[idx].sup = conv;
      itens[idx].estado = conv ? DECODIFICADO : VAZIO;
      if (!conv) itens[idx].caminho[0] = 0;
    } else if (conv) {
      SDL_FreeSurface(conv);  // slot foi reaproveitado no meio do caminho
    }
    SDL_UnlockMutex(mtx);
  }
}

int tex_iniciar(int max_itens) {
  nMax = max_itens > 0 && max_itens <= MAX_ITENS_ABS ? max_itens : 64;
  orcamento = NV_TEX_ORCAMENTO_MB * 1024L * 1024L;
  bytesUsados = 0;
  memset(itens, 0, sizeof itens);
  mtx = SDL_CreateMutex(); cond = SDL_CreateCond();
  rodando = 1;
  thr = SDL_CreateThread(threadDecode, "nv-decode", NULL);
  return thr != NULL;
}

void tex_encerrar(void) {
  SDL_LockMutex(mtx); rodando = 0; SDL_CondBroadcast(cond); SDL_UnlockMutex(mtx);
  if (thr) SDL_WaitThread(thr, NULL);
  for (int i = 0; i < nMax; i++) {
    if (itens[i].tex) glDeleteTextures(1, &itens[i].tex);
    if (itens[i].sup) SDL_FreeSurface(itens[i].sup);
  }
  SDL_DestroyCond(cond); SDL_DestroyMutex(mtx);
}

static GLuint tex_obter_limite(const char *caminho, int limite) {
  if (!caminho || !*caminho) return 0;
  GLuint saida = 0;
  unsigned long h = hashCaminho(caminho);
  SDL_LockMutex(mtx);
  int i = acharIndice(caminho, h);
  if (i >= 0) {
    itens[i].uso = ++relogio;
    // PROMOCAO: a mesma arte pode ser pedida como poster (960) e depois como
    // hero (1920). Se o teto novo e maior e a textura pronta ficou menor que
    // ele, refaz — senao o hero herda para sempre a versao pequena que o card
    // pediu primeiro, e o borrao volta sem explicacao aparente.
    if (limite > itens[i].limite) {
      itens[i].limite = limite;
      if (itens[i].estado == PRONTO && itens[i].w < limite) {
        int prox = (filaFim + 1) % MAX_FILA;
        if (prox != filaIni) {
          itens[i].estado = PENDENTE;
          fila[filaFim] = i; filaFim = prox; SDL_CondSignal(cond);
        }
      }
    }
    saida = itens[i].estado == PRONTO ? itens[i].tex : 0;
  } else {
    int novo = slotLivre();
    if (novo >= 0) {
      strncpy(itens[novo].caminho, caminho, sizeof itens[novo].caminho - 1);
      itens[novo].hash = h;
      itens[novo].limite = limite;
      itens[novo].estado = PENDENTE;
      itens[novo].uso = ++relogio;
      int prox = (filaFim + 1) % MAX_FILA;
      if (prox != filaIni) { fila[filaFim] = novo; filaFim = prox; SDL_CondSignal(cond); }
      else { itens[novo].estado = VAZIO; itens[novo].caminho[0] = 0; } // fila cheia
    }
  }
  SDL_UnlockMutex(mtx);
  return saida;
}

GLuint tex_obter(const char *caminho) {
  return tex_obter_limite(caminho, NV_TEX_LARG_MAX);
}

// Arte que ocupa a tela inteira: hero da home, backdrop do detalhe e a arte do
// player. 1920 e a largura do painel — pedir mais so gastaria memoria, pedir
// menos e ampliar depois.
GLuint tex_obter_hero(const char *caminho) {
  return tex_obter_limite(caminho, NV_TEX_HERO_LARG_MAX);
}

float tex_aspecto(const char *caminho) {
  if (!caminho || !*caminho) return 0.0f;
  float a = 0.0f;
  unsigned long h = hashCaminho(caminho);
  SDL_LockMutex(mtx);
  int i = acharIndice(caminho, h);
  if (i >= 0 && itens[i].estado == PRONTO && itens[i].h > 0)
    a = (float)itens[i].w / (float)itens[i].h;
  SDL_UnlockMutex(mtx);
  return a;
}

int tex_bombear(int max_por_quadro) {
  int subiu = 0;
  for (int passo = 0; passo < max_por_quadro; passo++) {
    SDL_Surface *sup = NULL; int alvo = -1;
    SDL_LockMutex(mtx);
    for (int i = 0; i < nMax; i++) {
      if (itens[i].estado == DECODIFICADO && itens[i].sup) {
        sup = itens[i].sup; itens[i].sup = NULL; alvo = i; break;
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
  glGenerateMipmap(GL_TEXTURE_2D);
  // MIPMAP_NEAREST e nao _LINEAR: o trilinear le DOIS niveis da piramide por
  // amostra, e nesta GPU isso e o dobro do custo de textura em cada pixel de
  // cada card. A diferenca visual e um degrau na transicao entre niveis, que
  // so apareceria numa animacao de zoom continuo — que o app nao faz.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gfx_tex_esquecer(0);  // o bind do upload passou por fora do gfx_rect

    SDL_LockMutex(mtx);
    itens[alvo].tex = t; itens[alvo].w = sup->w; itens[alvo].h = sup->h;
    itens[alvo].estado = PRONTO;
    bytesUsados += (long)sup->w * sup->h * 4;
    podar();
    SDL_UnlockMutex(mtx);
    SDL_FreeSurface(sup);
    subiu++;
  }
  return subiu;
}

void tex_estatisticas(int *nItens, int *nPend, long *bytes) {
  int a=0, p=0; long b=0;
  SDL_LockMutex(mtx);
  for (int i = 0; i < nMax; i++) {
    if (itens[i].estado == PRONTO) { a++; b += (long)itens[i].w * itens[i].h * 4; }
    else if (itens[i].estado != VAZIO) p++;
  }
  SDL_UnlockMutex(mtx);
  if (nItens) *nItens = a;
  if (nPend) *nPend = p;
  if (bytes) *bytes = b;
}
