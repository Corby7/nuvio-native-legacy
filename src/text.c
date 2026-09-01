#include "text.h"
#include "gfx.h"
#include "layout.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string.h>
#include <stdio.h>

#define MAX_LINHAS 256

typedef struct {
  char chave[288];
  unsigned long hash;   // FNV-1a da chave, para pular o strcmp
  TxtLinha linha;
  unsigned long uso;
  int ocupado;
} Entrada;

// Fator entre o pixel do BUFFER e o pixel de layout. As fontes sao abertas em
// `corpo * escala` e a linha cacheada guarda a medida DIVIDIDA por ele, entao
// todo o resto do app continua medindo em 1920x1080 enquanto o glifo tem a
// resolucao real da tela.
//
// Sem isto o texto era rasterizado a 1080p e ampliado ao dobro na TV 4K — que
// e exatamente o borrao que o dono viu comparando com o app web, onde o
// navegador rasteriza no devicePixelRatio.
static float escalaTxt = 1.0f;
static TTF_Font *fontes[TXT_NFONTES];

static Entrada cache[MAX_LINHAS];

// Quantas linhas NOVAS podem ser rasterizadas por quadro.
//
// Medido no aparelho: entrar na pagina de detalhe rasteriza 12 linhas de uma
// vez e custa 12 ms — um quadro inteiro, e o tranco aparece exatamente na
// transicao que se quer suave. Rasterizar em conta-gotas faz o texto assentar
// um ou dois quadros depois, o que ninguem ve; o tranco, todo mundo ve.
// 2 e nao 4: com 4 o pior quadro media 6 ms so de texto, e o objetivo aqui e
// que NENHUMA parte sozinha coma mais que um terco do quadro.
#define TXT_POR_QUADRO 2
static int rastNesteQuadro;

void txt_novo_quadro(void) { rastNesteQuadro = 0; }
static unsigned long relogio = 1;
int    txt_rasterizadas = 0;
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
enum { PESO_REGULAR, PESO_MEDIUM, PESO_BOLD };
static const struct { int corpo, peso; } ESTILOS[TXT_NFONTES] = {
  { NV_FT_TITULO1,  PESO_BOLD   },   // titulo do filme na tela de detalhe
  // ERA PESO_REGULAR, pelo cabecalho espacado da pagina de titulo do app da
  // Apple. Esse cabecalho NAO EXISTE MAIS: a tela de detalhe do web e um
  // documento rolavel sem cabecalho fixo, e o do app da Apple saiu do port.
  // Hoje TXT_TITULO2 e usado so por "Biblioteca" (.library-page-title 56/600) e
  // pelos titulos de estado vazio da busca e da biblioteca — os TRES em peso
  // 600 no web. Regular errava 200 para baixo em todos.
  { NV_FT_TITULO2,  PESO_BOLD    },  // .library-page-title e estados vazios
  { NV_FT_TITULO3,  PESO_BOLD   },   // nome dentro do card destaque
  { NV_FT_HEADLINE, PESO_MEDIUM },   // cabecalho de fileira
  { NV_FT_BODY,     PESO_MEDIUM },   // rotulo de botao, titulo de episodio
  { NV_FT_CALLOUT,  PESO_MEDIUM },   // linha de genero
  { NV_FT_CAPTION,  PESO_REGULAR },  // sinopse, texto corrido
  { NV_FT_CAPTION2, PESO_REGULAR },  // creditos, datas, rotulos
  // Abaixo do minimo de 23px que o tvOS estabelece para TEXTO — mas isto nao e
  // texto para ler, e um selo de classificacao indicativa, que no aparelho tem
  // mesmo o tamanho de um icone.
  { NV_FT_MINI,     PESO_BOLD    },  // badge de classificacao
  // Player, do app web: titulo em 700 e o corpo em 400 (.player-title tem
  // font-weight 700; .player-subtitle e .player-time-label nao declaram peso e
  // herdam o normal).
  { NV_FT_PLR_TITULO, PESO_BOLD    },
  { NV_FT_PLR_CORPO,  PESO_REGULAR },
  { NV_FT_ROW_TITULO, PESO_BOLD    },  // .home-row-title (600)
  // Linha secundaria do hero em tela cheia. 600 sobre fundo escuro: Bold,
  // pela mesma regra optica ja escrita acima.
  { NV_FT_HERO_SEC,   PESO_BOLD    },
  // Tela de detalhe, medidos no app web. O peso 600 do rotulo do botao nao
  // existe no pacote da Inter embarcada (so Regular, Medium e Bold): fica em
  // MEDIUM, que erra 100 para baixo, e nao em Bold, que erraria 100 para cima e
  // engorda visivelmente numa pilula clara de 96px de altura.
  { NV_FT_DET_BOTAO, PESO_MEDIUM  },
  { NV_FT_DET_META,  PESO_REGULAR },
  { NV_FT_DET_SIN,   PESO_REGULAR },
  { NV_FT_DET_META2, PESO_REGULAR },
  { NV_FT_HERO_META, PESO_MEDIUM  },   // .home-modern-hero-meta-line (21/500)
  { NV_FT_HERO_SIN,  PESO_REGULAR },   // .home-hero-description (22/400)
  { NV_FT_PG_RELOGIO, PESO_MEDIUM  },  // .player-clock (26/600)
  { NV_FT_PG_FIM,     PESO_REGULAR },  // .player-ends-at (20/400)
  { NV_FT_PG_ROTULO,  PESO_MEDIUM  },  // .player-parental-label (22/600)
  { NV_FT_PG_GRAV,    PESO_REGULAR },  // .player-parental-severity (22/400)
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
static TTF_Font *reservas[TXT_NFONTES];
static char caminhoReserva[512];

// Primeiro codepoint FORA do ASCII, ou 0. Decodifica UTF-8 na mao porque e o
// unico ponto do app que precisa disso e puxar uma biblioteca por causa de tres
// linhas nao se paga.
static Uint32 primeiroNaoAscii(const char *s) {
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

// Fonte com que a linha `s` deve ser desenhada. Devolve a principal quando ela
// da conta — que e o caso da esmagadora maioria das linhas.
static TTF_Font *fonteDe(TxtEstilo estilo, const char *s) {
  Uint32 cp = primeiroNaoAscii(s);
  if (!cp || cp >= 0x10000) return fontes[estilo];
  // Acentos do portugues e do espanhol estao na Inter; so cai na reserva o que
  // ela realmente nao tem.
  if (TTF_GlyphIsProvided(fontes[estilo], (Uint16)cp)) return fontes[estilo];
  if (!caminhoReserva[0]) return fontes[estilo];
  if (!reservas[estilo])
    reservas[estilo] = TTF_OpenFont(caminhoReserva,
                                    (int)(ESTILOS[estilo].corpo * escalaTxt + 0.5f));
  return reservas[estilo] ? reservas[estilo] : fontes[estilo];
}

int txt_iniciar(const char *dirRecursos, float escala) {
  if (escala < 0.5f) escala = 1.0f;
  escalaTxt = escala;
  if (TTF_Init() != 0) { printf("TTF_Init: %s\n", TTF_GetError()); return 0; }
  // A fonte da propria LG e a que a interface da TV usa; DroidSans e a reserva.
  // A Inter vai EMBARCADA no pacote. A TV so tem as fontes da LG e as do app da
  // Netflix — nada proximo da SF Pro do tvOS. A Inter foi desenhada como
  // alternativa livre com metricas parecidas, e e o que aproxima o desenho das
  // letras do original. As fontes da LG ficam de reserva: se o pacote for
  // instalado sem a pasta fonts/, o app continua legivel em vez de morrer.
  char base[512] = "";
  if (dirRecursos && *dirRecursos) {
    snprintf(base, sizeof base, "%s/", dirRecursos);
  } else {
    char *bp = SDL_GetBasePath();
    if (bp) { snprintf(base, sizeof base, "%s", bp); SDL_free(bp); }
  }

  char inter[3][512];
  snprintf(inter[PESO_REGULAR], 512, "%sfonts/InterDisplay-Regular.ttf", base);
  snprintf(inter[PESO_MEDIUM],  512, "%sfonts/InterDisplay-Medium.ttf",  base);
  snprintf(inter[PESO_BOLD],    512, "%sfonts/InterDisplay-Bold.ttf",    base);

  const char *lg[3] = { "/usr/share/fonts/LG_Display-Light.ttf",
                        "/usr/share/fonts/LG_Display-Regular.ttf",
                        "/usr/share/fonts/LG_Display-Regular.ttf" };
  const char *droid[3] = { "/usr/share/fonts/DroidSans.ttf",
                           "/usr/share/fonts/DroidSans.ttf",
                           "/usr/share/fonts/DroidSans.ttf" };

  const char *familias[3][3] = {
    { inter[0], inter[1], inter[2] },
    { lg[0], lg[1], lg[2] },
    { droid[0], droid[1], droid[2] },
  };
  const char *nomes[3] = { "Inter (embarcada)", "LG Display", "DroidSans" };

  // Caminho da reserva CJK. Na TV e a DroidSansFallback; no Mac, a fonte do
  // sistema que cobre CJK — ali isto e so para a previa nao mentir.
  { const char *cand[] = {
      "/usr/share/fonts/DroidSansFallback.ttf",
      "/System/Library/Fonts/Hiragino Sans GB.ttc",
      "/System/Library/Fonts/PingFang.ttc",
      NULL };
    for (int i = 0; cand[i]; i++) {
      FILE *fr = fopen(cand[i], "rb");
      if (fr) { fclose(fr); snprintf(caminhoReserva, sizeof caminhoReserva, "%s", cand[i]); break; }
    }
    printf("reserva CJK: %s\n", caminhoReserva[0] ? caminhoReserva : "nenhuma"); }

  for (int c = 0; c < 3; c++) {
    int todas = 1;
    for (int i = 0; i < TXT_NFONTES; i++) {
      fontes[i] = TTF_OpenFont(familias[c][ESTILOS[i].peso],
                               (int)(ESTILOS[i].corpo * escalaTxt + 0.5f));
      if (!fontes[i]) { todas = 0; break; }
      // negrito sintetico so na reserva, que nao tem arquivo Bold proprio
      if (c > 0 && ESTILOS[i].peso == PESO_BOLD) TTF_SetFontStyle(fontes[i], TTF_STYLE_BOLD);
    }
    if (todas) { printf("fonte: %s\n", nomes[c]); return 1; }
    for (int i = 0; i < TXT_NFONTES; i++) { if (fontes[i]) TTF_CloseFont(fontes[i]); fontes[i] = NULL; }
  }
  printf("txt: nenhuma fonte carregou\n");
  return 0;
}

void txt_encerrar(void) {
  for (int i = 0; i < MAX_LINHAS; i++)
    if (cache[i].ocupado && cache[i].linha.tex) glDeleteTextures(1, &cache[i].linha.tex);
  for (int i = 0; i < TXT_NFONTES; i++) {
    if (fontes[i]) TTF_CloseFont(fontes[i]);
    if (reservas[i]) TTF_CloseFont(reservas[i]);
  }
  TTF_Quit();
}

TxtLinha txt_linha(TxtEstilo estilo, const char *s, int r, int g, int b, int a) {
  TxtLinha vazia = {0, 0, 0};
  if (!s || !*s || estilo < 0 || estilo >= TXT_NFONTES || !fontes[estilo]) return vazia;

  char chave[288];
  snprintf(chave, sizeof chave, "%d|%02x%02x%02x|%.240s", (int)estilo, r & 255, g & 255, b & 255, s);

  // Hash da chave para evitar o strcmp em quase todas as entradas: a busca
  // roda para CADA linha de CADA quadro, e comparar 288 bytes centenas de
  // vezes por quadro custa mais que o desenho.
  unsigned long h = 2166136261UL;
  { const char *p = chave;
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
  int livre = -1;
  for (int k = 0; k < MAX_LINHAS; k++) {
    int i = (int)((h + (unsigned long)k) % MAX_LINHAS);
    if (!cache[i].ocupado) { livre = i; break; }
    if (cache[i].hash == h && strcmp(cache[i].chave, chave) == 0) {
      cache[i].uso = ++relogio; return cache[i].linha;
    }
  }

  // Orcamento estourado: devolve vazio e tenta de novo no proximo quadro. A
  // linha aparece com um quadro de atraso em vez de travar o atual.
  if (rastNesteQuadro >= TXT_POR_QUADRO) return vazia;
  rastNesteQuadro++;
  int slot = livre;
  if (slot < 0) {
    // Tabela cheia: so agora vale a varredura completa atras do LRU. Isso
    // acontece no maximo TXT_POR_QUADRO vezes por quadro, nao por linha.
    unsigned long menor = ~0UL;
    for (int i = 0; i < MAX_LINHAS; i++)
      if (cache[i].uso < menor) { menor = cache[i].uso; slot = i; }
  }
  if (slot < 0) return vazia;
  if (cache[slot].ocupado && cache[slot].linha.tex) {
    // avisa o gfx: o nome pode ser reutilizado pelo glGenTextures logo abaixo
    gfx_tex_esquecer(cache[slot].linha.tex);
    glDeleteTextures(1, &cache[slot].linha.tex);
  }

  Uint64 t0 = SDL_GetPerformanceCounter();
  SDL_Color cor = { (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a };
  SDL_Surface *sf = TTF_RenderUTF8_Blended(fonteDe(estilo, s), s, cor);
  if (!sf) return vazia;
  SDL_Surface *cv = SDL_ConvertSurfaceFormat(sf, SDL_PIXELFORMAT_ABGR8888, 0);
  SDL_FreeSurface(sf);
  if (!cv) return vazia;

  GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cv->w, cv->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, cv->pixels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gfx_tex_esquecer(0);  // o bind do upload passou por fora do gfx_rect

  cache[slot].ocupado = 1;
  cache[slot].hash = h;
  strncpy(cache[slot].chave, chave, sizeof cache[slot].chave - 1);
  // Medida em unidades de LAYOUT, nao em pixeis do buffer.
  cache[slot].linha.tex = t;
  cache[slot].linha.w = (int)(cv->w / escalaTxt + 0.5f);
  cache[slot].linha.h = (int)(cv->h / escalaTxt + 0.5f);
  txt_rasterizadas++;
  txt_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
  cache[slot].uso = ++relogio;
  SDL_FreeSurface(cv);
  return cache[slot].linha;
}

void txt_desenhar(TxtLinha l, float x, float y) { txt_desenhar_alpha(l, x, y, 1.0f); }

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
static float encaixa(float v) {
  float e = escalaTxt;
  return (float)((int)(v * e + (v < 0.0f ? -0.5f : 0.5f))) / e;
}

void txt_desenhar_alpha(TxtLinha l, float x, float y, float alpha) {
  if (!l.tex) return;
  GfxRect r = { encaixa(x), encaixa(y), (float)l.w, (float)l.h };
  gfx_rect(r, l.tex, GFX_TEXTO, 0, 0, 0, 0.0f, 1, 1, 1, alpha);
}

float txt_tracking(TxtEstilo estilo, const char *s, int r, int g, int b,
                   float x, float y, float alpha, float tracking) {
  if (!s || !*s) return 0.0f;
  float larg = 0.0f;
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

    TxtLinha l = txt_linha(estilo, c, r, g, b, 255);
    if (x >= 0.0f && l.w) txt_desenhar_alpha(l, x + larg, y, alpha);
    larg += l.w + tracking;
  }
  return larg > 0.0f ? larg - tracking : 0.0f;
}

// Declarada em text.h desde o inicio e NUNCA implementada. Ninguem chamava,
// entao o link passava; a primeira chamada derrubou o build ARM com
// "undefined reference". No Mac isso NAO aparece: `cc -fsyntax-only` num
// arquivo solto nao linka nada.
TxtLinha txt_linha_corta(TxtEstilo estilo, const char *s, int r, int g, int b,
                         int a, float maxW) {
  TxtLinha l = txt_linha(estilo, s, r, g, b, a);
  if (!s || !*s || (float)l.w <= maxW) return l;
  char buf[512];
  size_t n = strlen(s);
  if (n >= sizeof buf - 4) n = sizeof buf - 4;
  memcpy(buf, s, n); buf[n] = 0;
  // Corta por PALAVRA enquanto houver espaco; so quando sobra uma palavra so e
  // que se corta no meio dela. Cortar sempre por caractere deixa meia palavra
  // antes das reticencias, e isso se le como texto corrompido, nao como corte.
  while (n > 0) {
    size_t corte = n;
    while (corte > 0 && buf[corte - 1] != ' ') corte--;
    if (corte > 1) n = corte - 1; else n--;
    // nunca parar no meio de um caractere UTF-8: meio caractere vira tofu
    while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
    buf[n] = 0;
    if (!n) break;
    char t[520];
    snprintf(t, sizeof t, "%s\xe2\x80\xa6", buf);
    l = txt_linha(estilo, t, r, g, b, a);
    if ((float)l.w <= maxW) return l;
  }
  return txt_linha(estilo, "\xe2\x80\xa6", r, g, b, a);
}

float txt_bloco(TxtEstilo estilo, const char *s, int r, int g, int b,
                float x, float y, float larg, float leading, float alpha, int maxLinhas) {
  if (!s || !*s) return 0.0f;
  char linha[512]; linha[0] = 0;
  float usado = 0.0f;
  int nLinhas = 0;
  const char *p = s;
  while (*p && (maxLinhas <= 0 || nLinhas < maxLinhas)) {
    // pega a proxima palavra
    const char *ini = p;
    while (*p && *p != ' ') p++;
    size_t np = (size_t)(p - ini);
    while (*p == ' ') p++;

    char tentativa[512];
    size_t nl = strlen(linha);
    if (nl + np + 2 >= sizeof tentativa) break;
    memcpy(tentativa, linha, nl);
    if (nl) tentativa[nl++] = ' ';
    memcpy(tentativa + nl, ini, np);
    tentativa[nl + np] = 0;

    TxtLinha m = txt_linha(estilo, tentativa, r, g, b, 255);
    if (m.w > larg && linha[0]) {
      // nao coube: fecha a linha atual e recomeca com a palavra
      TxtLinha l = txt_linha(estilo, linha, r, g, b, 255);
      txt_desenhar_alpha(l, x, y + usado, alpha);
      usado += leading; nLinhas++;
      if (maxLinhas > 0 && nLinhas >= maxLinhas) return usado;
      memcpy(linha, ini, np); linha[np] = 0;
    } else {
      memcpy(linha, tentativa, nl + np + 1);
    }
  }
  if (linha[0] && (maxLinhas <= 0 || nLinhas < maxLinhas)) {
    TxtLinha l = txt_linha(estilo, linha, r, g, b, 255);
    txt_desenhar_alpha(l, x, y + usado, alpha);
    usado += leading;
  }
  return usado;
}

// Quebra igual a txt_bloco, mas posiciona cada linha pela BORDA DIREITA. A
// duplicacao com txt_bloco e pequena e proposital: unificar as duas exigiria um
// parametro de alinhamento em todas as chamadas, e so este caso precisa.
float txt_bloco_dir(TxtEstilo estilo, const char *s, int r, int g, int b,
                    float xDir, float y, float larg, float leading,
                    float alpha, int maxLinhas) {
  if (!s || !*s) return 0.0f;
  char linha[512]; linha[0] = 0;
  float usado = 0.0f;
  int nLinhas = 0;
  const char *p = s;
  while (*p && (maxLinhas <= 0 || nLinhas < maxLinhas)) {
    const char *ini = p;
    while (*p && *p != ' ') p++;
    size_t np = (size_t)(p - ini);
    while (*p == ' ') p++;

    char tentativa[512];
    size_t nl = strlen(linha);
    if (nl + np + 2 >= sizeof tentativa) break;
    memcpy(tentativa, linha, nl);
    if (nl) tentativa[nl++] = ' ';
    memcpy(tentativa + nl, ini, np);
    tentativa[nl + np] = 0;

    TxtLinha m = txt_linha(estilo, tentativa, r, g, b, 255);
    if (m.w > larg && linha[0]) {
      TxtLinha l = txt_linha(estilo, linha, r, g, b, 255);
      if (xDir >= 0.0f) txt_desenhar_alpha(l, xDir - l.w, y + usado, alpha);
      usado += leading; nLinhas++;
      if (maxLinhas > 0 && nLinhas >= maxLinhas) return usado;
      memcpy(linha, ini, np); linha[np] = 0;
    } else {
      memcpy(linha, tentativa, nl + np + 1);
    }
  }
  if (linha[0] && (maxLinhas <= 0 || nLinhas < maxLinhas)) {
    TxtLinha l = txt_linha(estilo, linha, r, g, b, 255);
    if (xDir >= 0.0f) txt_desenhar_alpha(l, xDir - l.w, y + usado, alpha);
    usado += leading;
  }
  return usado;
}
