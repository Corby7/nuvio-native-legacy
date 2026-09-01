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
// Nao ha arquivo de icone no projeto, entao os quatro icones sao desenhados com
// gfx_cor. Ver o comentario de cada um: o telhado da casa e uma escada de barras
// porque nao existe rotacao no gfx, e o cabo da lupa e um degrau de tres
// quadradinhos pelo mesmo motivo.
#include "menu.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"

// Larguras: a recolhida cabe so o icone; a aberta e a da barra do tvOS, larga o
// bastante para o rotulo mais comprido ("Biblioteca") nao encostar na borda.
#define NV_MENU_W_ICONE   NV_LEGACY_RAIL_W
#define NV_MENU_W_ABERTO  392.0f
#define NV_MENU_LINHA_H   104.0f
#define NV_MENU_ICONE      44.0f
// O centro do icone e o mesmo nas duas larguras: no aparelho o icone NAO anda
// quando a barra abre, so o rotulo entra ao lado dele. Se o icone deslizasse
// junto, a abertura viraria um empurrao lateral em vez de uma revelacao.
#define NV_MENU_ICONE_CX  (NV_MENU_W_ICONE * 0.5f)
#define NV_MENU_ROTULO_X  108.0f
#define NV_MENU_PILL_PAD   12.0f
#define NV_MENU_RAIO_PILL  0.30f
// Quanto o conteudo a direita escurece com a barra aberta. Sem isso o menu
// disputa atencao com a arte do hero, que e clara e ocupa a tela toda.
#define NV_MENU_VEU        0.58f
// Mola da largura, deliberadamente mais frouxa que NV_MOLA_TELA: e a diferenca
// entre as duas que cria o atraso descrito no topo.
#define NV_MENU_MOLA_EXP   6.5f

static const char *ROTULOS[MENU_N] = { "Buscar", "Inicio", "Biblioteca", "Ajustes" };

static int   aberto  = 0;
static int   destino = MENU_INICIO;
static int   linha   = MENU_INICIO;   // destaque; so vira destino ao escolher
static int   mudou   = 0;
static float desliza = 0.0f;
static float expande = 0.0f;
static float animFoco[MENU_N];
static void icone(int d, float cx, float cy, float s, float r, float g, float b, float a);

// O legacy deixa a rail de 144px sempre visível. O menu expandido é uma
// camada adicional; não deslocamos o conteúdo quando ele fecha.
static void desenhaRailFixa(void) {
  GfxRect painel = { 0, 0, NV_LEGACY_RAIL_W, NV_TELA_H };
  gfx_cor(painel, 0.0f, 0.055f, 0.058f, 0.064f, 1.0f);
  float y = (NV_TELA_H - MENU_N * NV_MENU_LINHA_H) * 0.5f;
  for (int i = 0; i < MENU_N; i++, y += NV_MENU_LINHA_H) {
    int atual = (i == destino);
    float lum = atual ? 1.0f : 0.60f;
    if (atual) {
      GfxRect marca = { 18.0f, y + 12.0f, NV_LEGACY_RAIL_W - 36.0f,
                        NV_MENU_LINHA_H - 24.0f };
      gfx_cor(marca, NV_MENU_RAIO_PILL, 0.20f, 0.22f, 0.25f, 0.70f);
    }
    icone(i, NV_MENU_ICONE_CX, y + NV_MENU_LINHA_H * 0.5f,
          NV_MENU_ICONE, lum, lum, lum, 0.95f);
  }
}

int menu_iniciar(void) {
  aberto = 0; destino = MENU_INICIO; linha = MENU_INICIO; mudou = 0;
  desliza = 0.0f; expande = 0.0f;
  for (int i = 0; i < MENU_N; i++) animFoco[i] = 0.0f;
  return 1;
}

void menu_abrir(void) {
  if (aberto) return;
  // O destaque comeca sempre no destino em vigor, nunca onde ficou da ultima
  // vez: a barra e um mapa de onde voce esta, e abrir com o destaque em outro
  // item faria o usuario ler que ja mudou de tela.
  linha = destino;
  aberto = 1;
}
void menu_fechar(void) { aberto = 0; linha = destino; }

int menu_aberto(void)  { return aberto; }
int menu_visivel(void) { return 1; }
int menu_destino(void) { return destino; }
void menu_definir_destino(int d) {
  if (d < 0 || d >= MENU_N) return;
  destino = d;
  if (!aberto) linha = d;
}
int menu_mudou_destino(void) { int m = mudou; mudou = 0; return m; }
const char *menu_rotulo(int d) {
  return (d >= 0 && d < MENU_N) ? ROTULOS[d] : "";
}

// Confirma o destaque e recolhe. DIREITA tambem passa por aqui: no aparelho a
// barra nao "cancela" ao sair pela direita — o item destacado e o que o usuario
// esta olhando, e desfazer a escolha no caminho de volta seria surpresa.
static void escolher(void) {
  if (linha != destino) { destino = linha; mudou = 1; }
  aberto = 0;
}

void menu_evento(const SDL_Event *e) {
  if (!aberto || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;

  // Mesmo conjunto de teclas de "voltar" que o detalhe aceita: no controle e o
  // Back, no teclado cada pessoa alcanca uma diferente.
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { menu_fechar(); return; }

  if (k == SDLK_RIGHT || k == SDLK_RETURN || k == SDLK_KP_ENTER) { escolher(); return; }
  // Sem rotacao nas pontas: a barra e curta e o usuario ve as quatro linhas de
  // uma vez, entao dar a volta no fim da lista le como falha, nao como atalho.
  if (k == SDLK_DOWN && linha < MENU_N - 1) linha++;
  else if (k == SDLK_UP && linha > 0)       linha--;
  // ESQUERDA morre aqui de proposito: a barra ja e a borda da tela.
}

void menu_atualizar(float dt, Uint32 agora) {
  (void)agora;
  // Recolhido e assentado nao custa nada: nem mola, nem laco pelos destinos.
  if (!aberto && desliza < 0.002f) {
    if (desliza != 0.0f) { desliza = 0.0f; expande = 0.0f; }
    return;
  }
  float alvo = aberto ? 1.0f : 0.0f;
  desliza = anim_mola(desliza, alvo, dt, NV_MOLA_TELA);
  expande = anim_mola(expande, alvo, dt, NV_MENU_MOLA_EXP);
  for (int i = 0; i < MENU_N; i++) {
    float a = (aberto && i == linha) ? 1.0f : 0.0f;
    animFoco[i] = anim_mola(animFoco[i], a, dt,
                            a > animFoco[i] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
  }
}

// --- Icones ------------------------------------------------------------------
// Todos recebem o CENTRO e o lado `s`, e todas as medidas sao fracoes de `s`:
// assim o icone acompanha qualquer mudanca de NV_MENU_ICONE sem retoque.

// Lupa: disco cheio mais um cabo. O disco e cheio, e nao um anel, porque anel
// exigiria furar o meio com a cor do painel — e durante o deslize o painel
// ainda esta translucido, entao o furo apareceria com a cor errada.
// O cabo e uma escada de tres quadradinhos sobrepostos: gfx nao tem rotacao, e
// uma barra diagonal de verdade nao existe. Sobrepostos e com canto arredondado
// eles se fundem numa diagonal continua.
static void iconeBuscar(float cx, float cy, float s, float r, float g, float b, float a) {
  GfxRect disco = { cx - 0.34f * s, cy - 0.40f * s, 0.62f * s, 0.62f * s };
  gfx_cor(disco, 0.5f, r, g, b, a);
  for (int i = 0; i < 3; i++) {
    float t = i * 0.085f * s;
    GfxRect p = { cx + 0.14f * s + t, cy + 0.14f * s + t, 0.17f * s, 0.17f * s };
    gfx_cor(p, 0.5f, r, g, b, a);
  }
}

// Casa: corpo quadrado e telhado em escada. Nove degraus de largura crescente,
// com as pontas arredondadas para que a serrilha (menos de 2px por degrau em
// 1080p) se dissolva no anti-aliasing do SDF em vez de virar dente visivel.
static void iconeInicio(float cx, float cy, float s, float r, float g, float b, float a) {
  const int N = 9;
  float topo = cy - 0.44f * s, alt = 0.44f * s / N;
  for (int i = 0; i < N; i++) {
    float w = anim_mistura(0.16f * s, 0.80f * s, (i + 1) / (float)N);
    // +0.6 na altura: sem a sobreposicao aparece uma linha de fundo entre os
    // degraus quando o painel esta translucido.
    GfxRect d = { cx - w * 0.5f, topo + i * alt, w, alt + 0.6f };
    gfx_cor(d, 0.35f, r, g, b, a);
  }
  GfxRect corpo = { cx - 0.30f * s, cy - 0.02f * s, 0.60f * s, 0.44f * s };
  gfx_cor(corpo, 0.12f, r, g, b, a);
}

// Biblioteca: duas barras empilhadas, a de cima mais estreita — a leitura de
// "pilha" vem da diferenca de largura, nao do numero de barras. Com as duas
// iguais o icone vira um sinal de igual.
static void iconeBiblioteca(float cx, float cy, float s, float r, float g, float b, float a) {
  GfxRect topo  = { cx - 0.22f * s, cy - 0.34f * s, 0.44f * s, 0.20f * s };
  GfxRect base  = { cx - 0.36f * s, cy - 0.06f * s, 0.72f * s, 0.44f * s };
  gfx_cor(topo, 0.30f, r, g, b, a * 0.80f);
  gfx_cor(base, 0.22f, r, g, b, a);
}

// Ajustes: engrenagem aproximada por um disco com oito dentes. Os dentes sao
// quadrados sem rotacao — nos 45 graus eles entram no disco por um canto, e no
// tamanho do icone isso le como dente, nao como defeito. Um anel central nao
// entra pelo mesmo motivo da lupa: nao da para furar o painel.
static void iconeAjustes(float cx, float cy, float s, float r, float g, float b, float a) {
  static const float DIR[8][2] = {
    { 1, 0 }, { 0.7071f, 0.7071f }, { 0, 1 }, { -0.7071f, 0.7071f },
    { -1, 0 }, { -0.7071f, -0.7071f }, { 0, -1 }, { 0.7071f, -0.7071f }
  };
  float rd = 0.34f * s, d = 0.19f * s;
  for (int i = 0; i < 8; i++) {
    GfxRect t = { cx + DIR[i][0] * rd - d * 0.5f, cy + DIR[i][1] * rd - d * 0.5f, d, d };
    gfx_cor(t, 0.30f, r, g, b, a);
  }
  GfxRect disco = { cx - 0.30f * s, cy - 0.30f * s, 0.60f * s, 0.60f * s };
  gfx_cor(disco, 0.5f, r, g, b, a);
}

static void icone(int d, float cx, float cy, float s, float r, float g, float b, float a) {
  switch (d) {
    case MENU_BUSCAR:     iconeBuscar(cx, cy, s, r, g, b, a); break;
    case MENU_INICIO:     iconeInicio(cx, cy, s, r, g, b, a); break;
    case MENU_BIBLIOTECA: iconeBiblioteca(cx, cy, s, r, g, b, a); break;
    default:              iconeAjustes(cx, cy, s, r, g, b, a); break;
  }
}

void menu_desenhar(Uint32 agora) {
  (void)agora;
  // Rail fixa sempre presente, como no shell legacy. O overlay expandido só
  // entra em cena quando o menu foi solicitado.
  if (!aberto) desenhaRailFixa();
  if (!aberto && desliza < 0.002f) return;

  float w = anim_mistura(NV_MENU_W_ICONE, NV_MENU_W_ABERTO, expande);
  // A barra desliza a partir da propria largura: em desliza=0 ela esta inteira
  // fora da tela, qualquer que seja o estagio da expansao.
  float px = -w * (1.0f - desliza);

  GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
  gfx_cor(tela, 0.0f, 0, 0, 0, NV_MENU_VEU * desliza);

  // Painel quase opaco e um pouco mais escuro que NV_COR_FUNDO: encostado no
  // fundo da home ele precisa de uma aresta propria, senao a barra parece um
  // pedaco da tela que escureceu sozinho.
  GfxRect painel = { px, 0, w, NV_TELA_H };
  gfx_cor(painel, 0.0f, 0.075f, 0.078f, 0.086f, 0.97f * desliza);

  // Tudo daqui para baixo fica preso ao painel. Sem o recorte, o rotulo — que e
  // desenhado no x fixo do texto — vaza para o conteudo enquanto a barra ainda
  // esta estreita, e ve-se a palavra aparecendo fora dela.
  gfx_recorte(px, 0, w, NV_TELA_H);

  float y = (NV_TELA_H - MENU_N * NV_MENU_LINHA_H) * 0.5f;
  for (int i = 0; i < MENU_N; i++, y += NV_MENU_LINHA_H) {
    float f = animFoco[i];
    float cy = y + NV_MENU_LINHA_H * 0.5f;

    if (f > 0.01f) {
      GfxRect pill = { px + NV_MENU_PILL_PAD, y + 7.0f,
                       w - NV_MENU_PILL_PAD * 2.0f, NV_MENU_LINHA_H - 14.0f };
      gfx_cor(pill, NV_MENU_RAIO_PILL, 0.93f, 0.93f, 0.95f, 0.96f * f * desliza);
    }

    // Tres estados, e os tres precisam existir: em foco (tinta escura sobre a
    // pilula clara), destino em vigor (branco, para o usuario achar onde esta
    // sem mover o foco) e o resto (cinza). Com so dois estados, abrir o menu
    // apaga a indicacao de onde voce estava.
    int atual = (i == destino);
    float lum = anim_mistura(atual ? 1.0f : 0.62f, 0.13f, f);
    float alpha = desliza * anim_mistura(atual ? 1.0f : 0.85f, 1.0f, f);

    icone(i, px + NV_MENU_ICONE_CX, cy, NV_MENU_ICONE, lum, lum, lum, alpha);

    // O rotulo entra com a largura, nao antes dela: `expande` ao quadrado
    // segura a palavra ate a barra ter espaco de verdade, senao ela nasce
    // espremida contra o icone.
    float aRot = expande * expande * desliza;
    if (aRot > 0.01f) {
      int c = (int)(lum * 255.0f + 0.5f);
      TxtLinha l = txt_linha(TXT_HEADLINE, ROTULOS[i], c, c, c, 255);
      txt_desenhar_alpha(l, px + NV_MENU_ROTULO_X, cy - l.h * 0.5f, aRot);
    }
  }

  gfx_sem_recorte();
}
