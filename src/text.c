#include "text.h"
#include "gfx.h"
#include "layout.h"
#include "mark.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Quantas linhas de texto ficam guardadas ao mesmo tempo.
//
// 256 chegou ao limite quando a pagina de titulo passou a ter a secao de
// comentarios: cada cartao sao ~7 linhas (nome, cinco de texto e o rodape) e
// eles convivem com episodios, elenco, abas, sinopse e as duas linhas de meta.
// Passando do teto, o LRU despeja linhas que a PROPRIA TELA ainda vai desenhar
// no mesmo quadro; elas voltam pela fila de TXT_POR_QUADRO, duas por vez, e
// sao despejadas de novo. E esse laco que aparece como o texto "piscando".
//
// 512 nao muda o custo de busca (a sondagem parte do hash e para no primeiro
// buraco) nem o de rasterizacao. Custa memoria de textura para linhas que nao
// estao na tela — o preco de nao rerasterizar as que estao.
#define MAX_LINES 512

typedef struct {
  char key[288];
  unsigned long hash;   // FNV-1a da chave, para pular o strcmp
  TxtLine line;
  unsigned long uso;
  unsigned long frameUso;
  int busy;
} Entry;

// Fator entre o pixel do BUFFER e o pixel de layout. As fontes sao abertas em
// `corpo * escala` e a linha cacheada guarda a medida DIVIDIDA por ele, entao
// todo o resto do app continua medindo em 1920x1080 enquanto o glifo tem a
// resolucao real da tela.
//
// Sem isto o texto era rasterizado a 1080p e ampliado ao dobro na TV 4K — que
// e exatamente o borrao que o dono viu comparando com o app web, onde o
// navegador rasteriza no devicePixelRatio.
static float scaleTxt = 1.0f;
static TTF_Font *fonts[TXT_NFONTS];

// Os arquivos TTF dos tres pesos, LIDOS UMA VEZ e mantidos vivos enquanto o app
// vive: as faces do FreeType leem deles sob demanda, entao liberar aqui e
// leitura de memoria liberada no primeiro glifo novo. Sao ~900 KB no total.
// `donoPeso` marca quais ponteiros sao proprios: pesos que apontam para o mesmo
// arquivo compartilham o buffer e so um deles libera.
static unsigned char *bytesWeight[3];
static size_t         sizeWeight[3];
static int            ownerWeight[3];
// O RWops de cada estilo. Guardado porque abrimos com freesrc=0 (o buffer e
// compartilhado, a fonte nao pode fecha-lo) e alguem tem de fechar em
// txt_encerrar.
static SDL_RWops     *rwSource[TXT_NFONTS];

// Le o arquivo inteiro para um buffer novo. NULL se nao abrir.
static unsigned char *readAll(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  unsigned char *b;
  long n;
  *size = 0;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  n = ftell(f);
  if (n <= 0) { fclose(f); return NULL; }
  rewind(f);
  b = malloc((size_t)n);
  if (!b) { fclose(f); return NULL; }
  if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  fclose(f);
  *size = (size_t)n;
  return b;
}
// As alternativas so sao abertas para os 16 estilos de legenda e sob demanda.
// Abrir a matriz inteira (todas as familias x todos os estilos do app) gastaria
// memoria numa TV fraca por uma preferencia que afeta no maximo quatro linhas.
#define TXT_SUB_N (TXT_SUB_200 - TXT_SUB_50 + 1)
static TTF_Font *subFontsHeight[TXT_FAMILY_N][TXT_SUB_N];
static unsigned char subFontTried[TXT_FAMILY_N][TXT_SUB_N];
static int warningFallback[TXT_FAMILY_N];
const char *const TXT_FAMILIES_PT[TXT_FAMILY_N] = {
  "Inter", "LG Display", "Droid Sans"
};

static Entry cache[MAX_LINES];

// Quantas linhas NOVAS podem ser rasterizadas por quadro.
//
// Medido no aparelho: entrar na pagina de detalhe rasteriza 12 linhas de uma
// vez e custa 12 ms — um quadro inteiro, e o tranco aparece exatamente na
// transicao que se quer suave. Rasterizar em conta-gotas faz o texto assentar
// um ou dois quadros depois, o que ninguem ve; o tranco, todo mundo ve.
// 2 e nao 4: com 4 o pior quadro media 6 ms so de texto, e o objetivo aqui e
// que NENHUMA parte sozinha coma mais que um terco do quadro.
#define TXT_POR_FRAME 2
static int traceThisFrame;
static unsigned long frameTxt = 1;

void txt_new_frame(void) { traceThisFrame = 0; frameTxt++; }
static unsigned long lruClock = 1;
int    txt_rasterized = 0;
// Quantas linhas foram DESPEJADAS para dar lugar a outras. Zero e o estado
// saudavel. Se voltar a subir com a tela parada, a tabela encheu de novo e o
// texto vai piscar — e melhor ler isso num contador do que descobrir pela
// reclamacao de quem esta olhando a tela.
int    txt_evictions = 0;
double txt_ms = 0.0;

// Peso por estilo. Cada peso e um ARQUIVO de verdade da Inter Display
// (Regular 400, Medium 500, Bold 700) — nao ha passada repetida nem
// deslocamento sub-pixel para simular peso. O negrito sintetico
// (TTF_SetFontStyle) so entra nas familias de RESERVA (LG, Droid), que nao tem
// arquivo Bold proprio; ver o `if (c > 0 && ...)` em txt_iniciar. Isso importa
// porque negrito sintetico engorda os tracos sem redesenhar nada, e ao lado de
// um Bold de verdade a diferenca aparece logo nos titulos grandes.
//
// COMO O PESO 600 DO WEB E RESOLVIDO, e por que nao ha um valor unico.
// A Inter embarcada nao tem SemiBold, e acrescentar o arquivo esta fora de
// questao (o ipk ja tem 166 MB). Sobra escolher entre Medium (erra 100 para
// baixo) e Bold (erra 100 para cima), e a escolha e OPTICA, nao aritmetica:
//
//   texto CLARO sobre fundo escuro parece mais fino do que e  -> Bold
//   texto ESCURO sobre pilula clara parece mais grosso do que e -> Medium
//
// Por isso `.home-row-title` (600, branco no escuro) fica em Bold e
// `.series-primary-btn` (600, preto na pilula branca de 96px) fica em Medium.
// Sao dois destinos diferentes para o mesmo 600 de propósito, e nao um
// descuido — sem a regra escrita aqui, a proxima pessoa "conserta" um dos dois
// e desalinha a tela.
enum { WEIGHT_REGULAR, WEIGHT_MEDIUM, WEIGHT_BOLD };
static const struct { int body, weight; } STYLES[TXT_NFONTS] = {
  { NV_FT_TITLE1,  WEIGHT_BOLD   },   // titulo do filme na tela de detalhe
  // ERA PESO_REGULAR, pelo cabecalho espacado da pagina de titulo do app da
  // Apple. Esse cabecalho NAO EXISTE MAIS: a tela de detalhe do web e um
  // documento rolavel sem cabecalho fixo, e o do app da Apple saiu do port.
  // Hoje TXT_TITULO2 e usado so por "Biblioteca" (.library-page-title 56/600) e
  // pelos titulos de estado vazio da busca e da biblioteca — os TRES em peso
  // 600 no web. Regular errava 200 para baixo em todos.
  { NV_FT_TITLE2,  WEIGHT_BOLD    },  // .library-page-title e estados vazios
  { NV_FT_TITLE3,  WEIGHT_BOLD   },   // nome dentro do card destaque
  { NV_FT_HEADLINE, WEIGHT_MEDIUM },   // cabecalho de fileira
  { NV_FT_BODY,     WEIGHT_MEDIUM },   // rotulo de botao, titulo de episodio
  { NV_FT_CALLOUT,  WEIGHT_MEDIUM },   // linha de genero
  { NV_FT_CAPTION,  WEIGHT_REGULAR },  // sinopse, texto corrido
  { NV_FT_CAPTION2, WEIGHT_REGULAR },  // creditos, datas, rotulos
  // Abaixo do minimo de 23px que o tvOS estabelece para TEXTO — mas isto nao e
  // texto para ler, e um selo de classificacao indicativa, que no aparelho tem
  // mesmo o tamanho de um icone.
  { NV_FT_MINI,     WEIGHT_BOLD    },  // badge de classificacao
  // Player, do app web: titulo em 700 e o corpo em 400 (.player-title tem
  // font-weight 700; .player-subtitle e .player-time-label nao declaram peso e
  // herdam o normal).
  { NV_FT_PLR_TITLE, WEIGHT_BOLD    },
  { NV_FT_PLR_BODY,  WEIGHT_REGULAR },
  { NV_FT_ROW_TITLE, WEIGHT_BOLD    },  // .home-row-title (600)
  // Linha secundaria do hero em tela cheia. 600 sobre fundo escuro: Bold,
  // pela mesma regra optica ja escrita acima.
  { NV_FT_HERO_SEC,   WEIGHT_BOLD    },
  // Tela de detalhe, medidos no app web. O peso 600 do rotulo do botao nao
  // existe no pacote da Inter embarcada (so Regular, Medium e Bold): fica em
  // MEDIUM, que erra 100 para baixo, e nao em Bold, que erraria 100 para cima e
  // engorda visivelmente numa pilula clara de 96px de altura.
  { NV_FT_DET_BUTTON, WEIGHT_MEDIUM  },
  { NV_FT_DET_META,  WEIGHT_REGULAR },
  { NV_FT_DET_SIN,   WEIGHT_REGULAR },
  { NV_FT_DET_META2, WEIGHT_REGULAR },
  { NV_FT_HERO_META, WEIGHT_MEDIUM  },   // .home-modern-hero-meta-line (21/500)
  { NV_FT_HERO_SIN,  WEIGHT_REGULAR },   // .home-hero-description (22/400)
  { NV_FT_PG_CLOCK, WEIGHT_MEDIUM  },  // .player-clock (26/600)
  { NV_FT_PG_END,     WEIGHT_REGULAR },  // .player-ends-at (20/400)
  { NV_FT_PG_LABEL,  WEIGHT_MEDIUM  },  // .player-parental-label (22/600)
  { NV_FT_PG_GRAV,    WEIGHT_REGULAR },  // .player-parental-severity (22/400)
  { 36, WEIGHT_REGULAR },             // cabecalhos dos paineis do player oficial
  { 24, WEIGHT_BOLD },                // episodio/fonte dentro da lista
  { 28, WEIGHT_MEDIUM },              // titulo no card Continuar assistindo
  { 23, WEIGHT_REGULAR },             // temporada e nome do episodio
  { 20, WEIGHT_MEDIUM },              // tempo restante no badge do card
  { 110, WEIGHT_BOLD },               // posição real no ranking
  { 20, WEIGHT_REGULAR }, { 24, WEIGHT_REGULAR }, { 28, WEIGHT_REGULAR },
  { 32, WEIGHT_REGULAR }, { 36, WEIGHT_REGULAR }, { 40, WEIGHT_REGULAR },
  { 44, WEIGHT_REGULAR }, { 48, WEIGHT_REGULAR }, { 52, WEIGHT_REGULAR },
  { 56, WEIGHT_REGULAR }, { 60, WEIGHT_REGULAR }, { 64, WEIGHT_REGULAR },
  { 68, WEIGHT_REGULAR }, { 72, WEIGHT_REGULAR }, { 76, WEIGHT_REGULAR },
  { 80, WEIGHT_REGULAR },
};

// RESERVA PARA O QUE A INTER NAO TEM.
//
// A Inter cobre latim, e so. Um titulo japones da filmografia de um ator (a
// tela nova de pessoa mostra varios) saia como fileira de quadradinhos — a
// fonte nao tem o glifo e o SDL_ttf desenha .notdef sem reclamar. A TV traz
// /usr/share/fonts/DroidSansFallback.ttf, que cobre CJK; abrimos ela SOB
// DEMANDA, no mesmo corpo do estilo, e so para as linhas que precisam.
//
// Nao e fallback por glifo (isso exigiria compor a linha caractere a caractere
// e perder o kerning): a linha INTEIRA vai para a reserva quando o primeiro
// caractere fora do ASCII nao existir na fonte principal. Titulo misto
// "Deadpool & ウルヴァリン" sairia todo na reserva, o que e feio mas legivel —
// e o caso raro; o comum e a linha ser toda de uma escrita so.
// Uma reserva POR ESCRITA. A primeira versao tinha um arquivo so, escolhido
// como "o CJK", e o nome de uma atriz iraniana continuava em quadradinhos: a
// DroidSansFallback nao tem arabe. A TV traz arquivo separado para cada
// familia de escrita, e e por isso que a escolha e por faixa de codepoint.
typedef enum { ESC_CJK, ESC_ARABIC, ESC_CYRILLIC_ETC, ESC_N } Write;
static TTF_Font *fallbacks[ESC_N][TXT_NFONTS];
static char pathFallback[ESC_N][512];

// Primeiro codepoint FORA do ASCII, ou 0. Decodifica UTF-8 na mao porque e o
// unico ponto do app que precisa disso e puxar uma biblioteca por causa de tres
// linhas nao se paga.
static Uint32 firstNotAscii(const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  for (; *p; p++) {
    if (*p < 0x80) continue;
    if ((*p & 0xE0) == 0xC0 && p[1])
      return (Uint32)((*p & 0x1F) << 6 | (p[1] & 0x3F));
    if ((*p & 0xF0) == 0xE0 && p[1] && p[2])
      return (Uint32)((*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F));
    if ((*p & 0xF8) == 0xF0) return 0x10000;   // fora do BMP: nao tratamos
    return 0;
  }
  return 0;
}

// Qual reserva cobre este codepoint. As faixas sao as usuais do Unicode; o que
// nao for arabe/hebraico nem CJK cai na terceira, que e a DroidSansFallback (ela
// cobre cirilico, grego, tailandes e mais).
static Write writeOf(Uint32 cp) {
  if (cp >= 0x0590 && cp <= 0x07FF) return ESC_ARABIC;       // hebraico + arabe
  if (cp >= 0xFB50 && cp <= 0xFEFF) return ESC_ARABIC;       // formas de apresentacao
  if (cp >= 0x2E80 && cp <= 0x9FFF) return ESC_CJK;
  if (cp >= 0xAC00 && cp <= 0xD7AF) return ESC_CJK;         // hangul
  if (cp >= 0xF900 && cp <= 0xFAFF) return ESC_CJK;
  return ESC_CYRILLIC_ETC;
}

// Fonte com que a linha `s` deve ser desenhada. Devolve a principal quando ela
// da conta — que e o caso da esmagadora maioria das linhas.
static TTF_Font *fontOf(TxtStyle style, const char *s) {
  Uint32 cp = firstNotAscii(s);
  Write e;
  if (!cp || cp >= 0x10000) return fonts[style];
  // Acentos do portugues e do espanhol estao na Inter; so cai na reserva o que
  // ela realmente nao tem.
  if (TTF_GlyphIsProvided(fonts[style], (Uint16)cp)) return fonts[style];
  e = writeOf(cp);
  if (!pathFallback[e][0]) return fonts[style];
  if (!fallbacks[e][style])
    fallbacks[e][style] = TTF_OpenFont(pathFallback[e],
                                       (int)(STYLES[style].body * scaleTxt + 0.5f));
  return fallbacks[e][style] ? fallbacks[e][style] : fonts[style];
}

static TTF_Font *subtitleFontOf(TxtStyle style, const char *s,
                                TxtFamily family) {
  int i;
  const char *path;
  if (family <= TXT_FAMILY_INTER || family >= TXT_FAMILY_N ||
      style < TXT_SUB_50 || style > TXT_SUB_200)
    return fontOf(style, s);
  i = style - TXT_SUB_50;
  if (subFontsHeight[family][i]) return subFontsHeight[family][i];
  if (subFontTried[family][i]) return fontOf(style, s);
  subFontTried[family][i] = 1;
  path = family == TXT_FAMILY_LG
          ? "/usr/share/fonts/LG_Display-Regular.ttf"
          : "/usr/share/fonts/DroidSans.ttf";
  subFontsHeight[family][i] = TTF_OpenFont(
      path, (int)(STYLES[style].body * scaleTxt + 0.5f));
  if (!subFontsHeight[family][i]) {
    if (!warningFallback[family]) {
      printf("subtitle font %s unavailable; using Inter\n",
             TXT_FAMILIES_PT[family]);
      warningFallback[family] = 1;
    }
    return fontOf(style, s);
  }
  return subFontsHeight[family][i];
}

int txt_start(const char *dirAssets, float scale) {
  if (scale < 0.5f) scale = 1.0f;
  scaleTxt = scale;
  if (TTF_Init() != 0) { printf("TTF_Init: %s\n", TTF_GetError()); return 0; }
  // A fonte da propria LG e a que a interface da TV usa; DroidSans e a reserva.
  // A Inter vai EMBARCADA no pacote. A TV so tem as fontes da LG e as do app da
  // Netflix — nada proximo da SF Pro do tvOS. A Inter foi desenhada como
  // alternativa livre com metricas parecidas, e e o que aproxima o desenho das
  // letras do original. As fontes da LG ficam de reserva: se o pacote for
  // instalado sem a pasta fonts/, o app continua legivel em vez de morrer.
  char base[512] = "";
  if (dirAssets && *dirAssets) {
    snprintf(base, sizeof base, "%s/", dirAssets);
  } else {
    char *bp = SDL_GetBasePath();
    if (bp) { snprintf(base, sizeof base, "%s", bp); SDL_free(bp); }
  }

  char inter[3][512];
  snprintf(inter[WEIGHT_REGULAR], 512, "%sfonts/InterDisplay-Regular.ttf", base);
  snprintf(inter[WEIGHT_MEDIUM],  512, "%sfonts/InterDisplay-Medium.ttf",  base);
  snprintf(inter[WEIGHT_BOLD],    512, "%sfonts/InterDisplay-Bold.ttf",    base);

  const char *lg[3] = { "/usr/share/fonts/LG_Display-Light.ttf",
                        "/usr/share/fonts/LG_Display-Regular.ttf",
                        "/usr/share/fonts/LG_Display-Regular.ttf" };
  const char *droid[3] = { "/usr/share/fonts/DroidSans.ttf",
                           "/usr/share/fonts/DroidSans.ttf",
                           "/usr/share/fonts/DroidSans.ttf" };

  const char *families[3][3] = {
    { inter[0], inter[1], inter[2] },
    { lg[0], lg[1], lg[2] },
    { droid[0], droid[1], droid[2] },
  };
  const char *names[3] = { "Inter (embarcada)", "LG Display", "DroidSans" };

  // Caminho da reserva CJK. Na TV e a DroidSansFallback; no Mac, a fonte do
  // sistema que cobre CJK — ali isto e so para a previa nao mentir.
  // Largura 5, nao 4: a linha do arabe tem quatro candidatos mais o NULL, e o
  // laco abaixo para no NULL. Com [4] o terminador era descartado em silencio e
  // a busca do arabe seguia lendo a linha do cirilico.
  { const char *cand[ESC_N][5] = {
      /* ESC_CJK          */ { "/usr/share/fonts/LG_Display_JP.ttf",
                               "/usr/share/fonts/DroidSansFallback.ttf",
                               "/System/Library/Fonts/Hiragino Sans GB.ttc", NULL },
      /* ESC_ARABE        */ { "/usr/share/fonts/DroidNaskh-Regular.ttf",
                               "/usr/share/fonts/LG_Display_Urdu.ttf",
                               "/System/Library/Fonts/Supplemental/GeezaPro.ttc",
                               "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", NULL },
      /* ESC_CIRILICO_ETC */ { "/usr/share/fonts/DroidSansFallback.ttf",
                               "/usr/share/fonts/DroidSans.ttf",
                               "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", NULL },
    };
    const char *nameEsc[ESC_N] = { "CJK", "arabic", "rest" };
    for (int e = 0; e < ESC_N; e++) {
      for (int i = 0; cand[e][i]; i++) {
        FILE *fr = fopen(cand[e][i], "rb");
        if (fr) { fclose(fr);
                  snprintf(pathFallback[e], sizeof pathFallback[e], "%s", cand[e][i]);
                  break; }
      }
      printf("fallback %s: %s\n", nameEsc[e],
             pathFallback[e][0] ? pathFallback[e] : "none");
    } }

  mark("fonts: start");
  for (int c = 0; c < 3; c++) {
    int all = 1;
    // UM ARQUIVO, UMA LEITURA.
    //
    // MEDIDO: 1035 ms na TV contra 12 ms no Mac para o MESMO txt_iniciar. Nao e
    // o FreeType que custa — e o armazenamento do aparelho. TTF_OpenFont abre e
    // LE O ARQUIVO INTEIRO a cada chamada, e sao TXT_NFONTES chamadas sobre
    // apenas TRES arquivos distintos (Regular, Medium, Bold): a mesma dezena de
    // leituras da mesma dezena de megabytes, num disco que entrega ~1 MB/s de
    // arquivo pequeno.
    //
    // Aqui os tres arquivos sao lidos UMA vez para a memoria e cada estilo abre
    // sobre esses bytes com TTF_OpenFontRW. Nao ha fio nenhum de proposito: o
    // gargalo era I/O REPETIDO, e paralelizar leituras redundantes no mesmo
    // armazenamento lento nao as torna menos redundantes — nao fazer as
    // leituras torna. Serial e mais previsivel, e nada disso encosta na
    // thread-safety duvidosa do FreeType.
    for (int p = 0; p < 3; p++) {
      int j;
      // A LG repete Regular em dois pesos e a Droid nos tres: nao ler de novo.
      for (j = 0; j < p; j++)
        if (!strcmp(families[c][p], families[c][j])) break;
      if (j < p) { bytesWeight[p] = bytesWeight[j]; sizeWeight[p] = sizeWeight[j]; ownerWeight[p] = 0; continue; }
      bytesWeight[p] = readAll(families[c][p], &sizeWeight[p]);
      ownerWeight[p] = bytesWeight[p] ? 1 : 0;
      if (!bytesWeight[p]) { all = 0; break; }
    }
    if (all)
      for (int i = 0; i < TXT_NFONTS; i++) {
        int weight = STYLES[i].weight;
        // Um RWops POR fonte: o FreeType le pelo stream durante toda a vida da
        // face, entao dois estilos nao podem dividir a mesma posicao de leitura.
        // Sao bytes em memoria — criar o RWops nao custa I/O.
        SDL_RWops *rw = SDL_RWFromConstMem(bytesWeight[weight], (int)sizeWeight[weight]);
        // freesrc=0: quem libera o RWops e o TTF_CloseFont em txt_encerrar? Nao
        // — passamos 0 e guardamos o ponteiro, porque o buffer e compartilhado
        // entre estilos e nao pode ser liberado pela primeira fonte a fechar.
        fonts[i] = rw ? TTF_OpenFontRW(rw, 0, (int)(STYLES[i].body * scaleTxt + 0.5f)) : NULL;
        rwSource[i] = rw;
        // A LG usa SDL 2.0.4: SDL_RWclose so existe nas versoes novas do SDL.
        // SDL_FreeRW e a ABI disponivel no webOS 4 e libera corretamente o
        // stream criado por SDL_RWFromConstMem.
        if (!fonts[i]) { if (rw) SDL_FreeRW(rw); rwSource[i] = NULL; all = 0; break; }
        // negrito sintetico so na reserva, que nao tem arquivo Bold proprio
        if (c > 0 && STYLES[i].weight == WEIGHT_BOLD) TTF_SetFontStyle(fonts[i], TTF_STYLE_BOLD);
      }
    if (all) {
      printf("font: %s (%d styles, 3 reads)\n", names[c], TXT_NFONTS);
      mark("fonts: ready");
      return 1;
    }
    for (int i = 0; i < TXT_NFONTS; i++) {
      if (fonts[i]) TTF_CloseFont(fonts[i]);
      fonts[i] = NULL;
      if (rwSource[i]) SDL_FreeRW(rwSource[i]);
      rwSource[i] = NULL;
    }
    for (int p = 0; p < 3; p++) {
      if (ownerWeight[p]) free(bytesWeight[p]);
      bytesWeight[p] = NULL; sizeWeight[p] = 0; ownerWeight[p] = 0;
    }
  }
  printf("txt: no font loaded\n");
  mark("fonts: none loaded");
  return 0;
}

void txt_shutdown(void) {
  for (int i = 0; i < MAX_LINES; i++)
    if (cache[i].busy && cache[i].line.tex) glDeleteTextures(1, &cache[i].line.tex);
  // ORDEM: a fonte primeiro, o RWops depois, o buffer por ultimo. A face do
  // FreeType ainda referencia o stream, e o stream, os bytes.
  for (int i = 0; i < TXT_NFONTS; i++) {
    if (fonts[i]) TTF_CloseFont(fonts[i]);
    fonts[i] = NULL;
    if (rwSource[i]) SDL_FreeRW(rwSource[i]);
    rwSource[i] = NULL;
    for (int e = 0; e < ESC_N; e++)
      if (fallbacks[e][i]) TTF_CloseFont(fallbacks[e][i]);
  }
  for (int p = 0; p < 3; p++) {
    if (ownerWeight[p]) free(bytesWeight[p]);
    bytesWeight[p] = NULL; sizeWeight[p] = 0; ownerWeight[p] = 0;
  }
  for (int f = 1; f < TXT_FAMILY_N; f++)
    for (int i = 0; i < TXT_SUB_N; i++)
      if (subFontsHeight[f][i]) TTF_CloseFont(subFontsHeight[f][i]);
  TTF_Quit();
}

static TxtLine lineFamily(TxtStyle style, const char *s, int r, int g,
                             int b, int a, TxtFamily family) {
  TxtLine empty = {0, 0, 0};
  if (!s || !*s || style < 0 || style >= TXT_NFONTS || !fonts[style]) return empty;

  if (family < TXT_FAMILY_INTER || family >= TXT_FAMILY_N)
    family = TXT_FAMILY_INTER;

  char key[288];
  snprintf(key, sizeof key, "%d:%d|%02x%02x%02x|%.236s", (int)family,
           (int)style, r & 255, g & 255, b & 255, s);

  // Hash da chave para evitar o strcmp em quase todas as entradas: a busca
  // roda para CADA linha de CADA quadro, e comparar 288 bytes centenas de
  // vezes por quadro custa mais que o desenho.
  unsigned long h = 2166136261UL;
  { const char *p = key;
    for (; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; } }

  // Sondagem a partir de h % MAX_LINHAS, e nao varredura das 256 entradas.
  // Esta busca roda para CADA linha de CADA quadro; a varredura completa
  // custava em media 128 comparacoes por acerto. Sondando do ponto do hash o
  // acerto sai nas primeiras casas, e a busca PARA no primeiro slot vazio:
  // quem foi inserido por esta mesma regra nunca esta depois de um buraco.
  //
  // O despejo LRU pode abrir um buraco no meio de uma corrente antiga; o
  // efeito e no maximo uma rerasterizacao daquela linha (que entra de novo
  // mais perto do hash), nunca resultado errado — a chave e conferida por
  // strcmp de qualquer forma.
  int free_ = -1;
  for (int k = 0; k < MAX_LINES; k++) {
    int i = (int)((h + (unsigned long)k) % MAX_LINES);
    if (!cache[i].busy) { free_ = i; break; }
    if (cache[i].hash == h && strcmp(cache[i].key, key) == 0) {
      cache[i].uso = ++lruClock;
      cache[i].frameUso = frameTxt;
      return cache[i].line;
    }
  }

  // Orcamento estourado: devolve vazio e tenta de novo no proximo quadro. A
  // linha aparece com um quadro de atraso em vez de travar o atual.
  if (traceThisFrame >= TXT_POR_FRAME) return empty;
  traceThisFrame++;
  int slot = free_;
  if (slot < 0) {
    // Tabela cheia: so agora vale a varredura completa atras do LRU. Isso
    // acontece no maximo TXT_POR_QUADRO vezes por quadro, nao por linha.
    unsigned long smaller = ~0UL;
    for (int i = 0; i < MAX_LINES; i++)
      if (cache[i].busy && cache[i].frameUso != frameTxt &&
          cache[i].uso < smaller) {
        smaller = cache[i].uso;
        slot = i;
      }
  }
  if (slot < 0) return empty;
  if (cache[slot].busy) txt_evictions++;
  if (cache[slot].busy && cache[slot].line.tex) {
    // avisa o gfx: o nome pode ser reutilizado pelo glGenTextures logo abaixo
    gfx_tex_forget(cache[slot].line.tex);
    glDeleteTextures(1, &cache[slot].line.tex);
  }

  Uint64 t0 = SDL_GetPerformanceCounter();
  SDL_Color color = { (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a };
  SDL_Surface *sf = TTF_RenderUTF8_Blended(
      subtitleFontOf(style, s, family), s, color);
  if (!sf) return empty;
  SDL_Surface *cv = SDL_ConvertSurfaceFormat(sf, SDL_PIXELFORMAT_ABGR8888, 0);
  SDL_FreeSurface(sf);
  if (!cv) return empty;

  GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cv->w, cv->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, cv->pixels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gfx_tex_forget(0);  // o bind do upload passou por fora do gfx_rect

  cache[slot].busy = 1;
  cache[slot].hash = h;
  strncpy(cache[slot].key, key, sizeof cache[slot].key - 1);
  // Medida em unidades de LAYOUT, nao em pixeis do buffer.
  cache[slot].line.tex = t;
  cache[slot].line.w = (int)(cv->w / scaleTxt + 0.5f);
  cache[slot].line.h = (int)(cv->h / scaleTxt + 0.5f);
  txt_rasterized++;
  txt_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
  cache[slot].uso = ++lruClock;
  cache[slot].frameUso = frameTxt;
  SDL_FreeSurface(cv);
  return cache[slot].line;
}

TxtLine txt_line(TxtStyle style, const char *s, int r, int g, int b, int a) {
  return lineFamily(style, s, r, g, b, a, TXT_FAMILY_INTER);
}

TxtLine txt_line_family(TxtStyle style, const char *s, int r, int g,
                           int b, int a, TxtFamily family) {
  return lineFamily(style, s, r, g, b, a, family);
}

void txt_draw(TxtLine l, float x, float y) { txt_draw_alpha(l, x, y, 1.0f); }

// ENCAIXE NO PIXEL DA TELA.
//
// A textura do glifo tem exatamente a resolucao em que vai ser desenhada, mas
// o CANTO caia em coordenada fracionaria o tempo todo: centralizacao
// (`(r.h - l.h) * 0.5f`), pilhas ancoradas na base, molas de rolagem. Com o
// canto em 478.4 o GL_LINEAR amostra ENTRE dois texels e cada letra sai
// espalhada por duas colunas de pixel — o texto inteiro fica meio pixel fora
// de foco, em toda a tela, o tempo todo.
//
// Era isso que restava do "borrao" depois de o 4K se provar impossivel: nao
// falta resolucao, falta o texto cair em cima do pixel. O web nao tem esse
// problema porque o navegador ja posiciona glifo na grade do dispositivo.
//
// O arredondamento e feito na grade do DRAWABLE e nao na de layout: no Mac
// retina meio pixel de layout e um pixel de tela inteiro, e arredondar na
// grade errada jogaria o texto fora do lugar em vez de assenta-lo.
//
// So o TEXTO encaixa. Encaixar cartao e arte transformaria as molas em degraus
// visiveis; o glifo nao sofre disso porque a letra em si nao se deforma, ela
// so anda de um pixel para o outro.
static float fits(float v) {
  float e = scaleTxt;
  return (float)((int)(v * e + (v < 0.0f ? -0.5f : 0.5f))) / e;
}

void txt_draw_alpha(TxtLine l, float x, float y, float alpha) {
  if (!l.tex) return;
  GfxRect r = { fits(x), fits(y), (float)l.w, (float)l.h };
  gfx_rect(r, l.tex, GFX_TEXT, 0, 0, 0, 0.0f, 1, 1, 1, alpha);
}

float txt_tracking(TxtStyle style, const char *s, int r, int g, int b,
                   float x, float y, float alpha, float tracking) {
  if (!s || !*s) return 0.0f;
  float width = 0.0f;
  // Percorre por CARACTERE UTF-8, nao por byte: cortar no meio de um acento
  // produz um glifo invalido, e a fonte da LG devolve um retangulo vazio.
  for (const unsigned char *p = (const unsigned char *)s; *p; ) {
    int n = 1;
    if      ((*p & 0xF8) == 0xF0) n = 4;
    else if ((*p & 0xF0) == 0xE0) n = 3;
    else if ((*p & 0xE0) == 0xC0) n = 2;
    char c[5]; int k = 0;
    while (k < n && p[k]) { c[k] = (char)p[k]; k++; }
    c[k] = 0; p += k ? k : 1;

    TxtLine l = txt_line(style, c, r, g, b, 255);
    if (x >= 0.0f && l.w) txt_draw_alpha(l, x + width, y, alpha);
    width += l.w + tracking;
  }
  return width > 0.0f ? width - tracking : 0.0f;
}

// Declarada em text.h desde o inicio e NUNCA implementada. Ninguem chamava,
// entao o link passava; a primeira chamada derrubou o build ARM com
// "undefined reference". No Mac isso NAO aparece: `cc -fsyntax-only` num
// arquivo solto nao linka nada.
TxtLine txt_line_trim(TxtStyle style, const char *s, int r, int g, int b,
                         int a, float maxW) {
  return txt_line_trim_family(style, s, r, g, b, a, maxW,
                                 TXT_FAMILY_INTER);
}

TxtLine txt_line_trim_family(TxtStyle style, const char *s, int r, int g,
                                 int b, int a, float maxW,
                                 TxtFamily family) {
  TxtLine l = txt_line_family(style, s, r, g, b, a, family);
  if (!s || !*s || (float)l.w <= maxW) return l;
  char buf[512];
  size_t n = strlen(s);
  if (n >= sizeof buf - 4) n = sizeof buf - 4;
  memcpy(buf, s, n); buf[n] = 0;
  // Corta por PALAVRA enquanto houver espaco; so quando sobra uma palavra so e
  // que se corta no meio dela. Cortar sempre por caractere deixa meia palavra
  // antes das reticencias, e isso se le como texto corrompido, nao como corte.
  while (n > 0) {
    size_t cut = n;
    while (cut > 0 && buf[cut - 1] != ' ') cut--;
    if (cut > 1) n = cut - 1; else n--;
    // nunca parar no meio de um caractere UTF-8: meio caractere vira tofu
    while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
    buf[n] = 0;
    if (!n) break;
    char t[520];
    snprintf(t, sizeof t, "%s\xe2\x80\xa6", buf);
    l = txt_line_family(style, t, r, g, b, a, family);
    if ((float)l.w <= maxW) return l;
  }
  return txt_line_family(style, "\xe2\x80\xa6", r, g, b, a, family);
}

float txt_block(TxtStyle style, const char *s, int r, int g, int b,
                float x, float y, float width, float leading, float alpha, int maxLines) {
  if (!s || !*s) return 0.0f;
  char line[512]; line[0] = 0;
  float used = 0.0f;
  int nLines = 0;
  const char *p = s;
  while (*p && (maxLines <= 0 || nLines < maxLines)) {
    // pega a proxima palavra
    const char *start = p;
    while (*p && *p != ' ') p++;
    size_t np = (size_t)(p - start);
    while (*p == ' ') p++;

    char attempt[512];
    size_t nl = strlen(line);
    if (nl + np + 2 >= sizeof attempt) break;
    memcpy(attempt, line, nl);
    if (nl) attempt[nl++] = ' ';
    memcpy(attempt + nl, start, np);
    attempt[nl + np] = 0;

    TxtLine m = txt_line(style, attempt, r, g, b, 255);
    if (m.w > width && line[0]) {
      // nao coube: fecha a linha atual e recomeca com a palavra
      TxtLine l = txt_line(style, line, r, g, b, 255);
      txt_draw_alpha(l, x, y + used, alpha);
      used += leading; nLines++;
      if (maxLines > 0 && nLines >= maxLines) return used;
      memcpy(line, start, np); line[np] = 0;
    } else {
      memcpy(line, attempt, nl + np + 1);
    }
  }
  if (line[0] && (maxLines <= 0 || nLines < maxLines)) {
    TxtLine l = txt_line(style, line, r, g, b, 255);
    txt_draw_alpha(l, x, y + used, alpha);
    used += leading;
  }
  return used;
}

// Quebra igual a txt_bloco, mas posiciona cada linha pela BORDA DIREITA. A
// duplicacao com txt_bloco e pequena e proposital: unificar as duas exigiria um
// parametro de alinhamento em todas as chamadas, e so este caso precisa.
float txt_block_dir(TxtStyle style, const char *s, int r, int g, int b,
                    float xDir, float y, float width, float leading,
                    float alpha, int maxLines) {
  if (!s || !*s) return 0.0f;
  char line[512]; line[0] = 0;
  float used = 0.0f;
  int nLines = 0;
  const char *p = s;
  while (*p && (maxLines <= 0 || nLines < maxLines)) {
    const char *start = p;
    while (*p && *p != ' ') p++;
    size_t np = (size_t)(p - start);
    while (*p == ' ') p++;

    char attempt[512];
    size_t nl = strlen(line);
    if (nl + np + 2 >= sizeof attempt) break;
    memcpy(attempt, line, nl);
    if (nl) attempt[nl++] = ' ';
    memcpy(attempt + nl, start, np);
    attempt[nl + np] = 0;

    TxtLine m = txt_line(style, attempt, r, g, b, 255);
    if (m.w > width && line[0]) {
      TxtLine l = txt_line(style, line, r, g, b, 255);
      if (xDir >= 0.0f) txt_draw_alpha(l, xDir - l.w, y + used, alpha);
      used += leading; nLines++;
      if (maxLines > 0 && nLines >= maxLines) return used;
      memcpy(line, start, np); line[np] = 0;
    } else {
      memcpy(line, attempt, nl + np + 1);
    }
  }
  if (line[0] && (maxLines <= 0 || nLines < maxLines)) {
    TxtLine l = txt_line(style, line, r, g, b, 255);
    if (xDir >= 0.0f) txt_draw_alpha(l, xDir - l.w, y + used, alpha);
    used += leading;
  }
  return used;
}
