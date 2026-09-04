// Rail lateral fixa do Nuvio 1.0.1 legacy, com overlay expansível para
// navegação por D-pad.
//
// Duas animacoes independentes, e a separacao e o que da o movimento certo:
//   - `desliza` tira a barra da borda esquerda (posicao);
//   - `expande` troca a largura de "so icone" para "icone + rotulo".
// No tvOS a barra recolhida mostra apenas os icones e so alarga quando ganha o
// foco. Aqui ela nasce fora da tela, entao os dois acontecem quase juntos — mas
// com molas de rigidez diferente, de forma que a largura ATRASA em relacao a
// entrada. E esse atraso que produz a leitura "entrou e entao se abriu"; com uma
// mola so, a barra aparece ja no tamanho final e o efeito some.
//
// Icones derivados dos SVGs originais do sidebar, com alpha e recortes reais.
#include "menu.h"
#include "profiles.h"
#include "tex_cache.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"

// Larguras: a recolhida cabe so o icone; a aberta e a da barra do tvOS, larga o
// bastante para o rotulo mais comprido ("Biblioteca") nao encostar na borda.
#define NV_MENU_W_ICON   NV_LEGACY_RAIL_W
#define NV_MENU_W_IS_OPEN  392.0f
#define NV_MENU_LINE_H    96.0f
#define NV_MENU_ICON      38.0f
// O centro do icone e o mesmo nas duas larguras: no aparelho o icone NAO anda
// quando a barra abre, so o rotulo entra ao lado dele. Se o icone deslizasse
// junto, a abertura viraria um empurrao lateral em vez de uma revelacao.
#define NV_MENU_ICON_CX  (NV_MENU_W_ICON * 0.5f)
#define NV_MENU_LABEL_X  112.0f
#define NV_MENU_PILL_DFLT   20.0f
#define NV_MENU_RADIUS_PILL  0.20f
// Quanto o conteudo a direita escurece com a barra aberta. Sem isso o menu
// disputa atencao com a arte do hero, que e clara e ocupa a tela toda.
#define NV_MENU_VEIL        0.58f
// TEMPO DE ABRIR E DE FECHAR, com relogio proprio.
//
// A referencia nao tem barra lateral nenhuma na home — LEFT e UP a partir do
// primeiro card sobem para os botoes do hero e param ali; nao ha rail para
// medir. O unico overlay comparavel que consegui abrir la foi a folha de
// contexto da tecla MENU, e ela da o tempo e a FORMA do veu:
//
//   fechar (medida limpa, 10 quadros seguidos, sem perda):
//     16ms 0,00 | 33 0,09 | 50 0,19 | 66 0,29 | 83 0,38 | 100 0,49
//     117 0,60 | 133 0,73 | 151 0,84 | 166 0,98
//   ou seja RAMPA RETA, ~0,10 a cada 17 ms, terminando em ~150 ms de percurso.
//   abrir: mesma rampa reta, ~0,0044/ms, o que da ~230 ms de percurso.
//
// Dois achados que a mola nao reproduzia: o veu e LINEAR (mola nenhuma e), e
// FECHAR e bem mais rapido que ABRIR. NV_MOLA_TELA (9,0) dava 333 ms simetricos
// e com a partida mais veloz do percurso, que e o oposto de uma rampa.
#define NV_MENU_OPEN_MS  230.0f
#define NV_MENU_CLOSE_MS 150.0f
// A largura continua ATRASADA em relacao a entrada — e o efeito "entrou e
// entao se abriu" descrito no topo do arquivo. Nao ha medida da referencia para
// ele (la nao existe esta barra); o que mudou foi so o tempo total, agora
// amarrado ao mesmo relogio em vez de uma mola de rigidez solta.
#define NV_MENU_EXP_LENTO  1.6f

// "Inicio" sem acento era erro de portugues NA TELA. E "Busca", nao "Buscar":
// os outros tres sao substantivos (Biblioteca, Ajustes) e o verbo destoava.
// Rotulos e ordem conferidos na referencia.
static const char *LABELS[MENU_N] = { "Home", "Search", "Library", "Profile and Stats", "Settings" };

// RODAPE: quem esta usando o app, e a porta para trocar. Ele e um item de
// FOCO a mais, no indice MENU_N — nao entrou no enum de proposito, porque
// trocar de perfil nao e uma aba do app e ninguem deve poder "navegar" para
// ela como destino.
#define NV_MENU_FOOTER_H   112.0f
#define NV_MENU_AVATAR      56.0f
#define NV_MENU_FOCOS      (MENU_N + 1)
#define MENU_FOOTER         MENU_N

static int   requestedSwap = 0;
static int   is_open  = 0;
static int   destination = MENU_START;
static int   line   = MENU_START;   // destaque; so vira destino ao escolher
static int   changed   = 0;
static float slides = 0.0f;
static float expands = 0.0f;
static float animFocus[NV_MENU_FOCOS];
static void icon(int d, float cx, float cy, float s, float r, float g, float b, float a);
static void drawFooter(float px, float w, float alpha, float focus);

// O legacy deixa a rail de 144px sempre visível. O menu expandido é uma
// camada adicional; não deslocamos o conteúdo quando ele fecha.
static void drawRailFixed(void) {
  GfxRect panel = { 0, 0, NV_LEGACY_RAIL_W, NV_TELA_H };
  gfx_color(panel, 0.0f, 0.055f, 0.058f, 0.064f, 1.0f);
  float y = (NV_TELA_H - MENU_N * NV_MENU_LINE_H) * 0.5f;
  for (int i = 0; i < MENU_N; i++, y += NV_MENU_LINE_H) {
    int current = (i == destination);
    float luma = current ? 1.0f : 0.60f;
    if (current) {
      GfxRect brand = { 18.0f, y + 12.0f, NV_LEGACY_RAIL_W - 36.0f,
                        NV_MENU_LINE_H - 24.0f };
      gfx_color(brand, NV_MENU_RADIUS_PILL, 0.20f, 0.22f, 0.25f, 0.70f);
    }
    icon(i, NV_MENU_ICON_CX, y + NV_MENU_LINE_H * 0.5f,
          NV_MENU_ICON, luma, luma, luma, 0.95f);
  }
  drawFooter(0.0f, NV_LEGACY_RAIL_W, 0.95f, 0.0f);
}

int menu_start(void) {
  is_open = 0; destination = MENU_START; line = MENU_START; changed = 0;
  slides = 0.0f; expands = 0.0f;
  for (int i = 0; i < MENU_N; i++) animFocus[i] = 0.0f;
  return 1;
}

void menu_open(void) {
  if (is_open) return;
  // O destaque comeca sempre no destino em vigor, nunca onde ficou da ultima
  // vez: a barra e um mapa de onde voce esta, e abrir com o destaque em outro
  // item faria o usuario ler que ja mudou de tela.
  line = destination;
  is_open = 1;
}
void menu_close(void) { is_open = 0; line = destination; }

int menu_is_open(void)  { return is_open; }
int menu_visible(void) { return 1; }
int menu_destination(void) { return destination; }
void menu_set_destination(int d) {
  if (d < 0 || d >= MENU_N) return;
  destination = d;
  if (!is_open) line = d;
}
int menu_changed_destination(void) { int m = changed; changed = 0; return m; }
const char *menu_label(int d) {
  return (d >= 0 && d < MENU_N) ? LABELS[d] : "";
}

// Confirma o destaque e recolhe. DIREITA tambem passa por aqui: no aparelho a
// barra nao "cancela" ao sair pela direita — o item destacado e o que o usuario
// esta olhando, e desfazer a escolha no caminho de volta seria surpresa.
static void choose(void) {
  if (line == MENU_FOOTER) {
    // O rodape nao troca de destino: ele pede a tela de escolha de perfil.
    requestedSwap = 1;
    is_open = 0;
    line = destination;
    return;
  }
  if (line != destination) { destination = line; changed = 1; }
  is_open = 0;
}

int menu_requested_swap(void) { int p = requestedSwap; requestedSwap = 0; return p; }

void menu_event(const SDL_Event *e) {
  if (!is_open || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  // Mesmo conjunto de teclas de "voltar" que o detalhe aceita: no controle e o
  // Back, no teclado cada pessoa alcanca uma diferente.
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { menu_close(); return; }

  if (k == SDLK_RIGHT || k == SDLK_RETURN || k == SDLK_KP_ENTER) { choose(); return; }
  // Sem rotacao nas pontas: a barra e curta e o usuario ve as quatro linhas de
  // uma vez, entao dar a volta no fim da lista le como falha, nao como atalho.
  if (k == SDLK_DOWN && line < NV_MENU_FOCOS - 1) line++;
  else if (k == SDLK_UP && line > 0)       line--;
  // ESQUERDA morre aqui de proposito: a barra ja e a borda da tela.
}

void menu_update(float dt, Uint32 now) {
  (void)now;
  // Recolhido e assentado nao custa nada: nem mola, nem laco pelos destinos.
  if (!is_open && slides < 0.002f) {
    if (slides != 0.0f) { slides = 0.0f; expands = 0.0f; }
    return;
  }
  float target = is_open ? 1.0f : 0.0f;
  float ms   = is_open ? NV_MENU_OPEN_MS : NV_MENU_CLOSE_MS;
  slides = anim_ramp(slides, target, dt, ms);
  expands = anim_ramp(expands, target, dt, ms * NV_MENU_EXP_LENTO);
  for (int i = 0; i < NV_MENU_FOCOS; i++) {
    float a = (is_open && i == line) ? 1.0f : 0.0f;
    animFocus[i] = anim_mola(animFocus[i], a, dt,
                            a > animFocus[i] ? NV_MOLA_FOCUS : NV_MOLA_DESFOCO);
  }
}

// Mesmos vetores do sidebar oficial, rasterizados no build e tintados pelo shader.
static void icon(int d, float cx, float cy, float s, float r, float g, float b, float a) {
  static const char *names[MENU_N] = {"menu_home", "menu_search", "menu_library", "menu_profile", "menu_settings"};
  if (d < 0 || d >= MENU_N) return;
  gfx_icon((GfxRect){cx-s*.5f, cy-s*.5f, s, s}, names[d], r, g, b, a);
}


// Cor do avatar a partir do "#RRGGBB" que a conta guarda. Sem cor legivel, o
// azul do padrao do web.
static void colorAvatar(const char *hex, float *r, float *g, float *b) {
  unsigned v = 0;
  *r = 0.12f; *g = 0.53f; *b = 0.90f;
  if (!hex || hex[0] != '#' || strlen(hex) < 7) return;
  if (sscanf(hex + 1, "%6x", &v) != 1) return;
  *r = ((v >> 16) & 255) / 255.0f;
  *g = ((v >> 8) & 255) / 255.0f;
  *b = (v & 255) / 255.0f;
}

// A INICIAL do nome, respeitando UTF-8: um nome comecado por acento tem dois
// bytes, e cortar no primeiro desenha lixo.
static void initialOf(const char *name, char *dst, size_t size) {
  if (size < 3) { if (size) dst[0] = 0; return; }
  dst[0] = (name && name[0]) ? name[0] : '?';
  dst[1] = 0;
  if (name && (unsigned char)name[0] >= 0xC0 && name[1]) { dst[1] = name[1]; dst[2] = 0; }
}

// Rodape: quem esta usando, e a porta para trocar. Desenha nas DUAS larguras —
// recolhida mostra so o avatar (e a unica coisa que cabe em 144px), aberta
// mostra nome e a acao.
static void drawFooter(float px, float w, float alpha, float focus) {
  const AccountProfile *p = profiles_item_active();
  // Acima da area segura, nao colado na base: numa TV os ultimos 60px podem
  // estar fora do painel (overscan), e o nome do usuario e justamente o que
  // some primeiro.
  float y = NV_TELA_H - NV_MARGIN_Y - NV_MENU_FOOTER_H;
  float cx = px + NV_MENU_ICON_CX;
  float cy = y + NV_MENU_FOOTER_H * 0.5f;
  float cr, cg, cb;
  char start[4];
  GfxRect av;

  if (alpha <= 0.01f) return;

  if (focus > 0.01f) {
    GfxRect pill = { px + NV_MENU_PILL_DFLT, y + 8.0f,
                     w - NV_MENU_PILL_DFLT * 2.0f, NV_MENU_FOOTER_H - 16.0f };
    GfxRect ring = { pill.x - NV_RING_FOCUS, pill.y - NV_RING_FOCUS,
                     pill.w + NV_RING_FOCUS * 2, pill.h + NV_RING_FOCUS * 2 };
    float radius = (pill.h * NV_MENU_RADIUS_PILL + NV_RING_FOCUS) / ring.h;
    gfx_color(ring, radius, 1, 1, 1, focus * alpha);
    gfx_color(pill, NV_MENU_RADIUS_PILL, NV_COLOR_FOCUS_R, NV_COLOR_FOCUS_G,
            NV_COLOR_FOCUS_B, focus * alpha);
  }

  av.x = cx - NV_MENU_AVATAR * 0.5f;
  av.y = cy - NV_MENU_AVATAR * 0.5f;
  av.w = av.h = NV_MENU_AVATAR;

  // FOTO quando a conta tem uma; senao o circulo com a inicial, que e o mesmo
  // que o app web mostra quando `avatar_url` e nulo — e nesta conta ele e.
  { GLuint tex = (p && p->avatarUrl[0]) ? tex_get(p->avatarUrl) : 0;
    if (tex) {
      gfx_tex_aspect_current = 1.0f;
      gfx_rect(av, tex, GFX_CARD, 0, 0, 0, 0.5f, 0, 0, 0, alpha);
    } else {
      colorAvatar(p ? p->colorHex : NULL, &cr, &cg, &cb);
      gfx_color(av, 0.5f, cr, cg, cb, alpha);
      initialOf(p ? p->name : NULL, start, sizeof start);
      { TxtLine l = txt_line(TXT_HEADLINE, start, 255, 255, 255, 255);
        txt_draw_alpha(l, av.x + (av.w - l.w) * 0.5f,
                           av.y + (av.h - l.h) * 0.5f, alpha); } } }

  // Nome e acao so aparecem com a barra aberta: em 144px nao cabe texto, e
  // espremer o nome ali seria pior que nao mostrar.
  { float aText = expands * expands * alpha;
    if (aText > 0.01f) {
      int c = (int)(anim_blend(0.72f, 1.0f, focus) * 255.0f + 0.5f);
      TxtLine name = txt_line_trim(TXT_BODY, p ? p->name : "Your account",
                                      c, c, c, 255,
                                      NV_MENU_W_IS_OPEN - NV_MENU_LABEL_X - 28.0f);
      TxtLine action = txt_line(TXT_CAPTION, "Switch user", 150, 152, 160, 255);
      txt_draw_alpha(name, px + NV_MENU_LABEL_X, cy - name.h - 2.0f, aText);
      txt_draw_alpha(action, px + NV_MENU_LABEL_X, cy + 4.0f, aText);
    } }
}

void menu_draw(Uint32 now) {
  (void)now;
  // Rail fixa sempre presente, como no shell legacy. O overlay expandido só
  // entra em cena quando o menu foi solicitado.
  // `collapseSidebar`: com a barra RECOLHIDA o web nao desenha rail nenhuma —
  // `.home-nav-list` fica com largura 0 e nao ocupa fluxo; ela so aparece como
  // camada quando ganha foco. O port ja movia o conteudo para 104 nesse caso
  // (ajustes_conteudo_x), mas continuava pintando os 144px da rail por baixo
  // dele: uma faixa escura sob o primeiro card, sem nada em cima.
  if (!is_open && slides < .002f && !settings_rail_collapsed()) drawRailFixed();
  if (!is_open && slides < 0.002f) return;

  float w = anim_blend(NV_MENU_W_ICON, NV_MENU_W_IS_OPEN, anim_smooth(expands));
  // O VEU usa a rampa CRUA: a medida da referencia e uma reta (ver
  // NV_MENU_ABRIR_MS). A POSICAO do painel usa a mesma rampa suavizada — um
  // bloco desse tamanho parando de vez no fim do percurso le como corte, e a
  // referencia comeca devagar em tudo que desliza (ver anim_mola2 em anim.h).
  float entry = anim_smooth(slides);
  float px = -w * (1.0f - entry);

  GfxRect screen = { 0, 0, NV_TELA_W, NV_TELA_H };
  gfx_color(screen, 0.0f, 0, 0, 0, NV_MENU_VEIL * slides);

  // Painel quase opaco e um pouco mais escuro que NV_COR_FUNDO: encostado no
  // fundo da home ele precisa de uma aresta propria, senao a barra parece um
  // pedaco da tela que escureceu sozinho.
  GfxRect panel = { px, 0, w, NV_TELA_H };
  gfx_color(panel, 0.0f, 0.075f, 0.078f, 0.086f, 0.97f * entry);

  // Tudo daqui para baixo fica preso ao painel. Sem o recorte, o rotulo — que e
  // desenhado no x fixo do texto — vaza para o conteudo enquanto a barra ainda
  // esta estreita, e ve-se a palavra aparecendo fora dela.
  gfx_crop(px, 0, w, NV_TELA_H);

  float y = (NV_TELA_H - MENU_N * NV_MENU_LINE_H) * 0.5f;
  for (int i = 0; i < MENU_N; i++, y += NV_MENU_LINE_H) {
    float f = animFocus[i];
    float cy = y + NV_MENU_LINE_H * 0.5f;

    if (i == destination && f < .99f) {
      GfxRect current = {px + NV_MENU_PILL_DFLT, y + 9, w - NV_MENU_PILL_DFLT*2, NV_MENU_LINE_H - 18};
      gfx_color(current, NV_MENU_RADIUS_PILL, .16f, .17f, .19f, .6f * (1-f) * slides);
    }
    if (f > 0.01f) {
      GfxRect pill = { px + NV_MENU_PILL_DFLT, y + 7.0f,
                       w - NV_MENU_PILL_DFLT * 2.0f, NV_MENU_LINE_H - 14.0f };
      // PILULA ESCURA com anel branco, nao pilula clara com texto escuro.
      // MEDIDO na referencia: item focado tem fundo #303030 e texto #FFFFFF —
      // o token --focus-bg, que o CSS do web tambem declara. O nosso invertia
      // (fundo #E4E4E9, texto escuro), e #E4E4E9 nao era cor de sistema
      // nenhuma: nem branco, nem o #F5F5F5 de --secondary-color.
      { GfxRect ring = { pill.x - NV_RING_FOCUS, pill.y - NV_RING_FOCUS,
                         pill.w + NV_RING_FOCUS * 2, pill.h + NV_RING_FOCUS * 2 };
        float radius = (pill.h * NV_MENU_RADIUS_PILL + NV_RING_FOCUS) / ring.h;
        gfx_color(ring, radius, 1, 1, 1, f * slides); }
      gfx_color(pill, NV_MENU_RADIUS_PILL, NV_COLOR_FOCUS_R, NV_COLOR_FOCUS_G,
              NV_COLOR_FOCUS_B, f * slides);
    }

    // Tres estados, e os tres precisam existir: em foco (texto BRANCO sobre a
    // pilula escura), destino em vigor (branco, para o usuario achar onde esta
    // sem mover o foco) e o resto (cinza). Com so dois estados, abrir o menu
    // apaga a indicacao de onde voce estava.
    int current = (i == destination);
    float luma = anim_blend(current ? 1.0f : 0.62f, 1.0f, f);
    float alpha = slides * anim_blend(current ? 1.0f : 0.85f, 1.0f, f);

    icon(i, px + NV_MENU_ICON_CX, cy, NV_MENU_ICON, luma, luma, luma, alpha);

    // O rotulo entra com a largura, nao antes dela: `expande` ao quadrado
    // segura a palavra ate a barra ter espaco de verdade, senao ela nasce
    // espremida contra o icone.
    float aRot = expands * expands * entry;
    if (aRot > 0.01f) {
      int c = (int)(luma * 255.0f + 0.5f);
      TxtLine l = txt_line_trim(TXT_BODY, LABELS[i], c, c, c, 255,
                                   NV_MENU_W_IS_OPEN - NV_MENU_LABEL_X - 28);
      txt_draw_alpha(l, px + NV_MENU_LABEL_X, cy - l.h * 0.5f, aRot);
    }
  }

  drawFooter(px, w, entry, animFocus[MENU_FOOTER]);

  gfx_sem_crop();
}
