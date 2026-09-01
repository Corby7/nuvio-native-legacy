// Busca, alinhada com a tela do app web (MEDIDA rodando, perfil do dono).
//
// ------------------------------------------------------------------------
// O QUE MUDOU, E POR QUE
//
// O port tinha um teclado em GRADE a esquerda e uma GRADE de posteres 4x a
// direita. Medida a tela do web, nenhuma das duas coisas confere:
//
//   1. O web nao tem grade de resultados. Tem FILEIRAS horizontais, uma por
//      catalogo de addon, com o nome do catalogo em 48/600 e a origem
//      ("from Xperience") em 20/400 logo abaixo. Card de 248 de largura, poster
//      248x372, nome 28/500 e ano 20/400 embaixo; passo 280 entre cards e 562.4
//      entre fileiras.
//   2. O web nao tem teclado nenhum: tem um <input> largo no topo, e quem
//      levanta o teclado e o SISTEMA da TV.
//
// A (1) foi portada inteira. A (2) NAO da para portar: este app e SDL puro e
// nao existe IME para chamar — sem teclado na tela nao ha como digitar, e uma
// busca em que nao se digita nao e uma busca. O teclado ficou, agora ABAIXO do
// cabecalho e a esquerda, ocupando a faixa onde o web desenha o estado vazio; as
// fileiras de resultado correm a direita dele. E a unica divergencia deliberada
// desta tela, e esta anotada aqui para nao ser confundida com descuido.
//
// DECISAO DE PROJETO — o teclado e em GRADE, nao a linha unica do tvOS.
// A faixa horizontal do tvOS e bonita e cabe em pouca altura, mas custa caro no
// D-pad: sao 38 teclas em UMA dimensao, entao a distancia media entre duas
// letras e ~13 toques e o pior caso passa de 37. A grade 6x7 poe a mesma tecla a
// no maximo 5+6 toques e ~5 em media.
#include "busca.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "ajustes.h"
#include "catalogo.h"
#include <string.h>
#include <stdio.h>

// --- Cabecalho: geometria MEDIDA no web -------------------------------------
// .search-header y=22 h=110, padding lateral 104.
//   .search-discover-btn 110x110 em (104,22)   bg #222, borda 1px #333, raio 22
//   .search-voice-btn    110x110 em (262,22)   -> passo 158 (gap 48)
//   .search-input-field  1396x110 em (420,22)  bg #222, raio 22, 34/500,
//                        padding lateral 32, placeholder "Buscar filmes e séries"
//
// O botao de VOZ nao existe aqui: nao ha captura de audio neste app, e botao que
// aparece e nao faz nada foi exatamente a reclamacao que originou esta rodada.
// O botao de DESCOBRIR aparece conforme `discoverLocation` — e `in_search` que o
// poe nesta tela (searchDiscoverEnabled).
#define BU_HEAD_Y     NV_BUSCA_HEAD_Y
#define BU_HEAD_H     NV_BUSCA_HEAD_H
#define BU_DIR       (NV_TELA_W - NV_CONTENT_PAD)   // 1816

// --- Teclado (divergencia deliberada; ver o topo) ----------------------------
#define BU_TECLA_W     74.0f
#define BU_TECLA_GAP   12.0f
#define BU_KB_COLS      6
#define BU_KB_FILEIRAS  7            // 6 fileiras de A-Z/0-9 + 1 de espaco/apagar
#define BU_KB_PASSO   (BU_TECLA_W + BU_TECLA_GAP)
#define BU_KB_W       (BU_KB_COLS * BU_TECLA_W + (BU_KB_COLS - 1) * BU_TECLA_GAP)
#define BU_KB_Y       NV_BUSCA_VAZIO_Y   // 148: a faixa do estado vazio do web
#define BU_TECLA_LARGA (3 * BU_TECLA_W + 2 * BU_TECLA_GAP)
// Crescimento da tecla em foco. Menor que o do poster de proposito: a tecla e
// pequena e vizinha imediata das outras, e com 14% ela invade o gap de 12px.
#define BU_TECLA_ESCALA 0.10f
#define BU_MAX_CONSULTA 48

// --- Fileiras de resultado (geometria do web) --------------------------------
#define BU_RES_X       (BU_KB_X + BU_KB_W + 64.0f)
#define BU_RES_Y       NV_BUSCA_VAZIO_Y
#define BU_RES_AREA_H  (NV_TELA_H - NV_MARGEM_Y - BU_RES_Y)
#define BU_MAX_FILEIRAS FOCUS_MAX_FILEIRAS
#define BU_MAX_POR_FIL  12

#define BU_KB_X        NV_CONTENT_PAD

// --- Estado ------------------------------------------------------------------
static Foco  focoKb;
static Foco  focoRes;
static int   painel = 0;            // 0 = teclado, 1 = resultados
static char  consulta[BU_MAX_CONSULTA];
static int   nConsulta = 0;
// Resultados agrupados por CATALOGO, como no web: uma fileira por catalogo que
// teve pelo menos um titulo casando. Guardamos indices do catalogo global.
static struct {
  const char *titulo;      // nome do catalogo ("Top 100 Today - Filme")
  const char *origem;      // "from <addon>"; vazio quando nao se sabe
  int itens[BU_MAX_POR_FIL];
  int n;
} fil[BU_MAX_FILEIRAS];
static int nFil = 0;
static int sair = 0;
static int pedido = -1;             // indice de catalogo escolhido, -1 = nenhum
static float animTecla[BU_KB_FILEIRAS][BU_KB_COLS];
static float animRes[BU_MAX_FILEIRAS][BU_MAX_POR_FIL];
static float scrollY = 0.0f, scrollAlvo = 0.0f;
static float scrollX[BU_MAX_FILEIRAS];
static HomeItem itemFoco;
static int   temItemFoco = 0;

static const int KB_COLUNAS[BU_KB_FILEIRAS] = { 6, 6, 6, 6, 6, 6, 2 };
// Minusculas como no aparelho: o campo mostra o que foi digitado, e uma consulta
// em caixa alta le como grito. A comparacao ignora caixa de qualquer forma.
static const char *TECLAS =
  "abcdefghijklmnopqrstuvwxyz0123456789";   // 36 = 6 fileiras x 6 colunas

// --- Normalizacao ------------------------------------------------------------
// Dobra uma letra latina acentuada (segundo byte de uma sequencia UTF-8 iniciada
// por 0xC3) na letra ASCII correspondente. Sem isto, buscar "fundacao" nao acha
// "Fundação" — o caso de uso mais obvio da tela, ja que ninguem digita cedilha
// num teclado de D-pad.
static char dobraLatina(unsigned char segundo) {
  unsigned cp = (unsigned)segundo + 0x40u;
  if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 0x20;
  if (cp >= 0xE0 && cp <= 0xE6) return 'a';
  if (cp == 0xE7)               return 'c';
  if (cp >= 0xE8 && cp <= 0xEB) return 'e';
  if (cp >= 0xEC && cp <= 0xEF) return 'i';
  if (cp == 0xF0)               return 'd';
  if (cp == 0xF1)               return 'n';
  if ((cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8) return 'o';
  if (cp >= 0xF9 && cp <= 0xFC) return 'u';
  if (cp == 0xFD || cp == 0xFF) return 'y';
  return ' ';
}

static void normalizar(const char *s, char *destino, size_t tam) {
  size_t k = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p && k + 1 < tam) {
    unsigned char c = *p++;
    char saida;
    if (c < 0x80) {
      saida = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    } else if (c == 0xC3 && *p) {
      saida = dobraLatina(*p++);
    } else {
      while ((*p & 0xC0) == 0x80) p++;
      saida = ' ';
    }
    destino[k++] = saida;
  }
  destino[k] = 0;
}

// --- Filtro ------------------------------------------------------------------
// Uma fileira por CATALOGO, exatamente como o web monta `.search-results-row`.
// Antes isto era uma lista plana do acervo inteiro, o que perdia a informacao de
// ONDE cada resultado foi achado — e e essa informacao que o subtitulo "from
// <addon>" mostra.
static void refiltrar(void) {
  char alvo[BU_MAX_CONSULTA * 2];
  normalizar(consulta, alvo, sizeof alvo);
  nFil = 0;
  // Menos de 2 caracteres = estado vazio, como o web ("Digite ao menos 2
  // caracteres"). Buscar com uma letra devolve o acervo inteiro e nao ajuda.
  if ((int)strlen(alvo) < 2) { painel = 0; return; }

  int nCat = cat_n_fileiras();
  for (int r = 0; r < nCat && nFil < BU_MAX_FILEIRAS; r++) {
    const CatFileira *cf = cat_fileira(r);
    if (!cf) break;
    int achou = 0;
    for (int i = 0; i < cf->n && achou < BU_MAX_POR_FIL; i++) {
      const CatItem *ci = cat_item(cf->ini + i);
      if (!ci) continue;
      char titulo[320];
      normalizar(ci->titulo, titulo, sizeof titulo);
      if (strstr(titulo, alvo)) fil[nFil].itens[achou++] = cf->ini + i;
    }
    if (!achou) continue;
    fil[nFil].titulo = cf->titulo;
    // `catalogAddonNameEnabled` decide a linha "de <addon>" sob o titulo da
    // fileira. O dado NAO existe deste lado: `CatFileira` guarda chave, titulo,
    // tipo e a janela no vetor — o nome do addon fica em addons.c e a fileira
    // nao o carrega. Ate descoberta.c passar esse campo adiante, a linha nao e
    // desenhada; escrever o TIPO ("movie") no lugar seria pior que a ausencia,
    // porque leria como se fosse a origem.
    fil[nFil].origem = NULL;
    fil[nFil].n = achou;
    nFil++;
  }

  int cols[BU_MAX_FILEIRAS];
  for (int i = 0; i < nFil; i++) cols[i] = fil[i].n;
  focus_iniciar(&focoRes, nFil > 0 ? nFil : 1, nFil > 0 ? cols : (int[]){ 1 });
  if (nFil == 0) painel = 0;
  memset(animRes, 0, sizeof animRes);
  memset(scrollX, 0, sizeof scrollX);
}

// --- Teclas ------------------------------------------------------------------
static void aplicarTecla(void) {
  if (focoKb.fileira < BU_KB_FILEIRAS - 1) {
    int k = focoKb.fileira * BU_KB_COLS + focoKb.coluna;
    if (nConsulta + 1 < BU_MAX_CONSULTA) consulta[nConsulta++] = TECLAS[k];
  } else if (focoKb.coluna == 0) {
    // espaco no comeco nao entra: nao muda o filtro e so acumula lixo no campo
    if (nConsulta > 0 && nConsulta + 1 < BU_MAX_CONSULTA) consulta[nConsulta++] = ' ';
  } else {
    if (nConsulta > 0) nConsulta--;
  }
  consulta[nConsulta] = 0;
  refiltrar();
}

static GfxRect retanguloTecla(int fileira, int coluna) {
  GfxRect r;
  r.y = BU_KB_Y + fileira * (BU_TECLA_W + BU_TECLA_GAP);
  r.h = BU_TECLA_W;
  if (fileira < BU_KB_FILEIRAS - 1) {
    r.x = BU_KB_X + coluna * BU_KB_PASSO;
    r.w = BU_TECLA_W;
  } else {
    r.x = BU_KB_X + coluna * (BU_TECLA_LARGA + BU_TECLA_GAP);
    r.w = BU_TECLA_LARGA;
  }
  return r;
}

// --- Ciclo de vida -----------------------------------------------------------
int busca_iniciar(void) {
  focus_iniciar(&focoKb, BU_KB_FILEIRAS, KB_COLUNAS);
  painel = 0; sair = 0; pedido = -1;
  nConsulta = 0; consulta[0] = 0;
  scrollY = scrollAlvo = 0.0f;
  temItemFoco = 0;
  memset(animTecla, 0, sizeof animTecla);
  memset(animRes, 0, sizeof animRes);
  memset(scrollX, 0, sizeof scrollX);
  refiltrar();
  return 1;
}

void busca_encerrar(void) { temItemFoco = 0; }
int  busca_quer_sair(void) { return sair; }

int busca_pediu_abrir(int *indiceCatalogo) {
  if (pedido < 0) return 0;
  if (indiceCatalogo) *indiceCatalogo = pedido;
  pedido = -1;
  return 1;
}

int busca_item_focado(HomeItem *out) {
  if (!temItemFoco || !out) return 0;
  *out = itemFoco;
  return 1;
}

void busca_evento(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { sair = 1; return; }
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) {
    // Nos resultados, o Back volta ao teclado: e o movimento inverso do que
    // levou ate la. So do teclado ele fecha a tela.
    if (painel == 1) painel = 0; else sair = 1;
    return;
  }

  if (painel == 0) {
    switch (k) {
      case SDLK_LEFT:  focus_mover(&focoKb, -1, 0); break;
      case SDLK_RIGHT:
        // Passar da ULTIMA coluna do teclado entra nos resultados. E a unica
        // ponte entre os dois paineis, e por isso ela nao pode falhar em
        // silencio: sem resultado nenhum, o foco fica onde esta.
        if (focoKb.coluna >= KB_COLUNAS[focoKb.fileira] - 1) {
          if (nFil > 0) painel = 1;
        } else focus_mover(&focoKb, 1, 0);
        break;
      case SDLK_UP:     focus_mover(&focoKb, 0, -1); break;
      case SDLK_DOWN:   focus_mover(&focoKb, 0,  1); break;
      case SDLK_RETURN: case SDLK_KP_ENTER: aplicarTecla(); break;
      default: break;
    }
    return;
  }

  switch (k) {
    case SDLK_LEFT:
      // Voltar da primeira coluna dos resultados devolve o foco ao teclado.
      if (focoRes.coluna == 0) painel = 0;
      else focus_mover(&focoRes, -1, 0);
      break;
    case SDLK_RIGHT:
      // `fastHorizontalNavigationEnabled`: o web pula de 3 em 3 dentro da
      // fileira quando a preferencia esta ligada. Numa fileira de 12 cards, 4
      // toques em vez de 12 para chegar ao fim.
      focus_mover(&focoRes, 1, 0);
      if (ajustes_navegacao_horizontal_rapida()) {
        focus_mover(&focoRes, 1, 0);
        focus_mover(&focoRes, 1, 0);
      }
      break;
    case SDLK_UP:   focus_mover(&focoRes, 0, -1); break;
    case SDLK_DOWN: focus_mover(&focoRes, 0,  1); break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
      if (focoRes.fileira < nFil && focoRes.coluna < fil[focoRes.fileira].n)
        pedido = fil[focoRes.fileira].itens[focoRes.coluna];
      break;
    default: break;
  }
}

void busca_atualizar(float dt, Uint32 agora) {
  (void)agora;
  for (int f = 0; f < BU_KB_FILEIRAS; f++)
    for (int c = 0; c < KB_COLUNAS[f]; c++) {
      float alvo = (painel == 0 && focus_indice(&focoKb, f, c)) ? 1.0f : 0.0f;
      animTecla[f][c] = anim_mola(animTecla[f][c], alvo, dt,
                                  alvo > animTecla[f][c] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
    }
  for (int r = 0; r < BU_MAX_FILEIRAS; r++)
    for (int c = 0; c < BU_MAX_POR_FIL; c++) {
      float alvo = (painel == 1 && focus_indice(&focoRes, r, c)) ? 1.0f : 0.0f;
      animRes[r][c] = anim_mola(animRes[r][c], alvo, dt,
                                alvo > animRes[r][c] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
    }

  // Rola so o necessario para a fileira em foco caber inteira na area util —
  // rolagem proporcional ao indice esconderia a primeira fileira antes de o
  // usuario ter chegado nela.
  if (painel == 1 && nFil > 0) {
    float topo = focoRes.fileira * NV_BUSCA_ROW_PASSO;
    float base = topo + NV_BUSCA_ROW_TRILHO + NV_BUSCA_POSTER_H + 70.0f;
    if (topo - scrollAlvo < 0.0f)             scrollAlvo = topo;
    if (base - scrollAlvo > BU_RES_AREA_H)    scrollAlvo = base - BU_RES_AREA_H;

    // Rolagem horizontal da fileira em foco, mesma regra da home.
    int r = focoRes.fileira;
    float util = BU_DIR - BU_RES_X;
    float esq = focoRes.coluna * NV_BUSCA_CARD_PASSO;
    float dir = esq + NV_BUSCA_CARD_W;
    float alvoX = scrollX[r];
    if (dir - alvoX > util) alvoX = dir - util;
    if (esq - alvoX < 0.0f) alvoX = esq;
    if (alvoX < 0.0f) alvoX = 0.0f;
    scrollX[r] = anim_mola(scrollX[r], alvoX, dt, NV_MOLA_SCROLL);
  } else {
    scrollAlvo = 0.0f;
  }
  if (scrollAlvo < 0.0f) scrollAlvo = 0.0f;
  scrollY = anim_mola(scrollY, scrollAlvo, dt, NV_MOLA_SCROLL);
}

// --- Desenho -----------------------------------------------------------------
// Cabecalho: botao de Descobrir (quando a preferencia o poe aqui) e o campo, com
// a geometria medida no web.
static void desenhaCabecalho(Uint32 agora) {
  float x = NV_CONTENT_PAD;
  float raio = NV_BUSCA_RAIO / (NV_BUSCA_HEAD_H * 0.5f) * 0.5f;  // 22 sobre 110

  if (ajustes_descobrir_na_busca()) {
    GfxRect b = { x, BU_HEAD_Y, NV_BUSCA_BTN, NV_BUSCA_BTN };
    gfx_cor(b, NV_BUSCA_RAIO / NV_BUSCA_BTN, 0.133f, 0.133f, 0.133f, 1.0f);
    // Bussola do "Descobrir", DESENHADA e nao escrita. O glifo que estava aqui
    // ("\xe2\x97\x89") nao existe na Inter embarcada e saia como a caixa de
    // "NO GLYPH" — visivel na captura do aparelho. Nenhuma fonte do pacote tem
    // simbolos geometricos, entao icone tem de ser geometria.
    float lado = NV_BUSCA_BTN_ICONE;
    GfxRect aro = { b.x + (b.w - lado) * 0.5f, b.y + (b.h - lado) * 0.5f, lado, lado };
    gfx_rect(aro, 0, GFX_ANEL, 0.0f, 0.075f, 0.0f, 0.5f, 0.925f, 0.929f, 0.949f, 0.9f);
    float ponto = lado * 0.26f;
    GfxRect miolo = { aro.x + (lado - ponto) * 0.5f, aro.y + (lado - ponto) * 0.5f,
                      ponto, ponto };
    gfx_cor(miolo, 0.5f, 0.925f, 0.929f, 0.949f, 0.9f);
    x += NV_BUSCA_BTN + NV_BUSCA_BTN_GAP;
  }

  GfxRect campo = { x, BU_HEAD_Y, BU_DIR - x, NV_BUSCA_HEAD_H };
  gfx_cor(campo, raio, 0.133f, 0.133f, 0.133f, 1.0f);
  // Foco do campo: borda #f5f5f5 com halo rgba(245,245,245,.22) de 2px. Aqui o
  // campo esta sempre "ativo" — quem digita e o teclado ao lado.
  GfxRect halo = { campo.x - 2.0f, campo.y - 2.0f, campo.w + 4.0f, campo.h + 4.0f };
  gfx_cor(halo, raio, 0.961f, 0.961f, 0.961f, 0.22f);

  float tx = campo.x + NV_BUSCA_CAMPO_PADX;
  if (nConsulta) {
    TxtLinha l = txt_linha(TXT_HEADLINE, consulta, 245, 246, 250, 255);
    txt_desenhar(l, tx, campo.y + (campo.h - l.h) * 0.5f);
    tx += l.w + 6.0f;
  } else {
    // Mesmo texto do placeholder do web.
    TxtLinha l = txt_linha(TXT_HEADLINE, "Buscar filmes e séries", 255, 255, 255, 255);
    txt_desenhar_alpha(l, tx, campo.y + (campo.h - l.h) * 0.5f, 0.40f);
  }
  // O cursor piscando e o unico sinal de que o campo esta ativo.
  if ((agora / 500) % 2 == 0) {
    GfxRect cur = { tx, campo.y + 24.0f, 3.0f, campo.h - 48.0f };
    gfx_cor(cur, 0.0f, 1.0f, 1.0f, 1.0f, 0.85f);
  }
}

static void desenhaTeclado(void) {
  char rotulo[8];
  for (int f = 0; f < BU_KB_FILEIRAS; f++) {
    for (int c = 0; c < KB_COLUNAS[f]; c++) {
      float k = animTecla[f][c];
      GfxRect base = retanguloTecla(f, c);
      float esc = 1.0f + BU_TECLA_ESCALA * k;
      GfxRect t = { base.x - base.w * (esc - 1.0f) * 0.5f,
                    base.y - base.h * (esc - 1.0f) * 0.5f,
                    base.w * esc, base.h * esc };
      // A tecla focada INVERTE (fundo claro, glifo escuro) em vez de so acender:
      // a distancia de sofa, a inversao e o unico contraste que se enxerga de
      // relance numa grade de 38 alvos iguais.
      gfx_cor(t, NV_RAIO_CARD, 1.0f, 1.0f, 1.0f, anim_mistura(0.09f, 1.0f, k));
      const char *s;
      if (f < BU_KB_FILEIRAS - 1) {
        rotulo[0] = TECLAS[f * BU_KB_COLS + c]; rotulo[1] = 0;
        s = rotulo;
      } else s = (c == 0) ? "espa\xc3\xa7o" : "apagar";
      int tom = (int)anim_mistura(236.0f, 26.0f, k);
      TxtEstilo est = (f < BU_KB_FILEIRAS - 1) ? TXT_TITULO3 : TXT_HEADLINE;
      TxtLinha l = txt_linha(est, s, tom, tom, tom, 255);
      txt_desenhar(l, t.x + (t.w - l.w) * 0.5f, t.y + (t.h - l.h) * 0.5f);
    }
  }
}

// Estado vazio do web: titulo 56/600 e apoio 24/400 rgb(179,179,179). Aqui ele
// fica a DIREITA, no lugar das fileiras, porque a faixa central esta com o
// teclado.
static void desenhaVazio(void) {
  const char *t1 = nConsulta ? "Nenhum resultado" : "Comece a pesquisar";
  const char *t2 = nConsulta ? "Tente outro termo"
                             : "Digite ao menos 2 caracteres";
  TxtLinha l1 = txt_linha(TXT_TITULO2, t1, 255, 255, 255, 255);
  TxtLinha l2 = txt_linha(TXT_BODY, t2, 179, 179, 179, 255);
  float cx = BU_RES_X + (BU_DIR - BU_RES_X) * 0.5f;
  float y = BU_RES_Y + 180.0f;
  txt_desenhar_alpha(l1, cx - l1.w * 0.5f, y, 0.96f);
  txt_desenhar_alpha(l2, cx - l2.w * 0.5f, y + l1.h + 18.0f, 0.85f);
}

static void desenhaResultados(Uint32 agora) {
  (void)agora;
  temItemFoco = 0;
  if (nFil == 0) { desenhaVazio(); return; }

  gfx_recorte(BU_RES_X - 8.0f, BU_RES_Y - 30.0f,
              (BU_DIR - BU_RES_X) + 16.0f, BU_RES_AREA_H + 30.0f);

  for (int r = 0; r < nFil; r++) {
    float ry = BU_RES_Y + r * NV_BUSCA_ROW_PASSO - scrollY;
    if (ry > NV_TELA_H + 100.0f || ry + NV_BUSCA_ROW_PASSO < -100.0f) continue;

    // Titulo do catalogo 48/600 e a origem 20/400 logo abaixo (margin-top 4).
    TxtLinha tt = txt_linha_corta(TXT_TITULO3, fil[r].titulo, 255, 255, 255, 255,
                                  BU_DIR - BU_RES_X);
    txt_desenhar(tt, BU_RES_X, ry);
    if (fil[r].origem) {
      char org[96];
      snprintf(org, sizeof org, "de %s", fil[r].origem);
      TxtLinha ts = txt_linha(TXT_CAPTION2, org, 179, 179, 179, 255);
      txt_desenhar_alpha(ts, BU_RES_X, ry + NV_BUSCA_ROW_SUB, 0.95f);
    }

    float cardY = ry + NV_BUSCA_ROW_TRILHO;
    // Dois passes: o item em foco tem de ficar POR CIMA dos vizinhos, senao a
    // borda do poster ao lado corta o anel de foco.
    for (int passe = 0; passe < 2; passe++)
      for (int c = 0; c < fil[r].n; c++) {
        float f = animRes[r][c];
        if ((passe == 1) != (f > 0.01f)) continue;
        const CatItem *ci = cat_item(fil[r].itens[c]);
        if (!ci) continue;

        float px = BU_RES_X + c * NV_BUSCA_CARD_PASSO - scrollX[r];
        if (px > BU_DIR || px + NV_BUSCA_CARD_W < BU_RES_X - NV_BUSCA_CARD_W) continue;
        GfxRect poster = { px, cardY, NV_BUSCA_CARD_W, NV_BUSCA_POSTER_H };
        // O card do web NAO escala no foco: marca por borda de 2px, como a home.
        float raio = NV_BUSCA_RAIO / NV_BUSCA_CARD_W;
        if (f > 0.01f) {
          GfxRect b = { poster.x - 2.0f, poster.y - 2.0f,
                        poster.w + 4.0f, poster.h + 4.0f };
          gfx_cor(b, raio, 0.961f, 0.961f, 0.961f, f);
        }

        const char *arte = ci->poster[0] ? ci->poster
                         : (ci->backdrop[0] ? ci->backdrop : NULL);
        GLuint tex = arte ? tex_obter(arte) : 0;
        if (tex) {
          // Sem o aspecto a arte 2:3 estica; e o poster e justamente onde isso
          // salta aos olhos, porque todos ficam lado a lado.
          gfx_tex_aspect_atual = tex_aspecto(arte);
          gfx_rect(poster, tex, GFX_CARD, f, 0.0f, 0.0f, raio, 0, 0, 0, 1);
          gfx_tex_aspect_atual = 0.0f;
        } else {
          gfx_cor(poster, raio, 0.133f, 0.133f, 0.133f, 1.0f);
        }

        // Nome 28/500 branco a 8 do poster; ano 20/400 rgb(179) a 4 do nome.
        TxtLinha tn = txt_linha_corta(TXT_CALLOUT, ci->titulo, 255, 255, 255, 255,
                                      NV_BUSCA_CARD_W);
        float ny = poster.y + poster.h + NV_BUSCA_NOME_GAP;
        txt_desenhar_alpha(tn, poster.x, ny, anim_mistura(0.82f, 1.0f, f));
        if (ci->meta[0]) {
          TxtLinha td = txt_linha_corta(TXT_CAPTION2, ci->meta, 179, 179, 179, 255,
                                        NV_BUSCA_CARD_W);
          txt_desenhar_alpha(td, poster.x, ny + tn.h + NV_BUSCA_DATA_GAP, 0.92f);
        }

        if (painel == 1 && focus_indice(&focoRes, r, c)) {
          itemFoco.indice = fil[r].itens[c];
          itemFoco.rect   = poster;
          itemFoco.arte   = ci->backdrop[0] ? ci->backdrop : ci->poster;
          itemFoco.titulo = ci->titulo;
          itemFoco.genero = ci->genero;
          itemFoco.meta   = ci->meta;
          temItemFoco = 1;
        }
      }
  }
  gfx_sem_recorte();
}

void busca_desenhar(Uint32 agora) {
  // Fundo #0d0d0d, medido no .search-screen-shell do web — mais escuro que o
  // cinza da home, e o web usa o mesmo tom nas duas.
  GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
  gfx_cor(tela, 0.0f, NV_COR_FUNDO_R, NV_COR_FUNDO_G, NV_COR_FUNDO_B, 1.0f);
  desenhaCabecalho(agora);
  desenhaTeclado();
  desenhaResultados(agora);
}
