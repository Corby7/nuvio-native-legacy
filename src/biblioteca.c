// Biblioteca: barra de abas no topo, grade de posteres embaixo.
//
// Duas decisoes vieram de erros ja cometidos em outras telas deste app:
//
//   1. A aba ESCOLHIDA continua marcada quando o foco desce para a grade. Foi o
//      mesmo problema das abas de temporada do detalhe: sem o estado de escolha
//      separado do foco, o usuario perde de vista em que aba esta assim que
//      desce, e a grade vira um monte de posteres sem contexto.
//   2. A rolagem move o MINIMO para a linha focada caber. Alinhar a linha
//      focada ao topo — que foi o que a pagina de detalhe fazia antes — empurra
//      as abas para fora da tela na primeira descida, e o aparelho nao faz
//      isso: ele so rola quando o conteudo nao cabe mais.
#include "biblioteca.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "catalogo.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// 6 colunas e a coluna oficial de 260px da tabela de grid do tvOS:
// 6*260 + 5*40 = 1760, exatamente a area util entre as margens laterais.
#define BIB_COLUNAS       6
#define BIB_ABAS          3
// A fileira 0 do foco e a barra de abas, entao sobram FOCUS_MAX_FILEIRAS-1
// linhas de grade — o teto do catalogo (32 itens) cabe com folga.
#define BIB_MAX_LINHAS    (FOCUS_MAX_FILEIRAS - 1)
// Espaco entre linhas. Precisa engolir o crescimento do foco: um poster de 390
// crescendo 14% invade 27px acima e 27 abaixo, mais o levantamento de 8px. Com
// o gap de 40 das colunas as linhas se tocariam no foco.
#define BIB_LINHA_GAP     64.0f
#define BIB_ABAS_Y        NV_MARGEM_Y
#define BIB_GRADE_TOPO    (NV_MARGEM_Y + NV_ABA_H + 48.0f)
#define BIB_GRADE_BASE    (NV_TELA_H - NV_MARGEM_Y)
// Em quantos px um poster desaparece ao subir por baixo da barra de abas. O
// recorte de tesoura resolveria, mas gfx_recorte assume alvo 1:1 com a tela e
// o Mac em retina entrega o dobro; o esmaecimento e o mesmo recurso que a
// pagina de detalhe usa para o mesmo problema, e nao depende do drawable.
#define BIB_FADE          90.0f

static const char *ROTULOS[BIB_ABAS] = { "Minha Lista", "Comprados", "Gêneros" };
enum { ABA_LISTA, ABA_COMPRADOS, ABA_GENEROS };

static int aba = ABA_LISTA;
static int filtro[CAT_MAX];      // indices do catalogo visiveis na aba atual
static int nFiltro = 0;
static Foco foco;
static float animAba[BIB_ABAS];
static float animFoco[BIB_MAX_LINHAS][BIB_COLUNAS];
static float scrollY = 0.0f;
static int sair = 0, pedido = -1;

// Estado de conta: no aparelho estes dois conjuntos vem do login. Aqui ficam em
// memoria, e comecam semeados de forma deterministica para que a tela nasca com
// conteudo — uma biblioteca vazia na primeira abertura nao mostra nada do que a
// grade faz. Quem esvazia (e chega ao estado vazio de verdade) e o usuario,
// pelo "+" do detalhe.
static char naLista[CAT_MAX];
static char comprado[CAT_MAX];

// O genero principal e o que vem depois do primeiro "·": a linha do catalogo
// abre com "Filme" ou "Programa de TV", que e formato, nao genero. Agrupar por
// ela colocaria metade do acervo num balaio so.
static const char *generoPrincipal(const CatItem *ci) {
  if (!ci || !ci->genero[0]) return "";
  const char *p = strstr(ci->genero, "\xc2\xb7");
  if (!p) return ci->genero;
  p += 2;
  while (*p == ' ') p++;
  return p;
}

static float alturaLinha(void) {
  // So a aba de generos reserva a faixa do rotulo embaixo do poster. Nas outras
  // o rotulo nao existe, e reservar o espaco deixaria buracos entre as linhas.
  return NV_POSTER_H + (aba == ABA_GENEROS ? NV_TOP10_ROTULO : 0.0f);
}
static float passoLinha(void) { return alturaLinha() + BIB_LINHA_GAP; }
static float passoColuna(void) { return NV_POSTER_W + NV_CARD_GAP; }
static int nLinhas(void) { return (nFiltro + BIB_COLUNAS - 1) / BIB_COLUNAS; }

// Refaz a lista visivel e o mapa de foco. Chamada a cada troca de aba porque o
// numero de colunas da ultima linha muda com o filtro, e um foco apontando para
// uma coluna que nao existe mais desenha um retangulo vazio.
static void reconstruir(void) {
  int n = cat_n();
  if (n > CAT_MAX) n = CAT_MAX;
  nFiltro = 0;
  for (int i = 0; i < n; i++) {
    // As marcas vem do TRAKT, pelo item do catalogo. Antes eram dois vetores
    // locais indexados por posicao — e o catalogo agora e reconstruido da rede,
    // entao a posicao 3 de hoje e outro titulo amanha.
    const CatItem *ci = cat_item(i);
    int entra = (aba == ABA_LISTA)     ? (ci && (ci->naLista || naLista[i]))
              : (aba == ABA_COMPRADOS) ? (ci && (ci->naColecao || comprado[i]))
                                       : 1;
    if (entra) filtro[nFiltro++] = i;
  }
  // Na aba de generos a ordem e por genero: sem isso a "aba de generos" e a
  // mesma grade do acervo com outro nome, e a aba nao significa nada. Ordenacao
  // por insercao — sao no maximo 32 itens, uma vez por troca de aba.
  if (aba == ABA_GENEROS) {
    for (int i = 1; i < nFiltro; i++) {
      int v = filtro[i], j = i - 1;
      while (j >= 0 &&
             strcmp(generoPrincipal(cat_item(filtro[j])), generoPrincipal(cat_item(v))) > 0) {
        filtro[j + 1] = filtro[j]; j--;
      }
      filtro[j + 1] = v;
    }
  }

  int linhas = nLinhas();
  if (linhas > BIB_MAX_LINHAS) linhas = BIB_MAX_LINHAS;
  int cols[FOCUS_MAX_FILEIRAS];
  cols[0] = BIB_ABAS;
  for (int r = 0; r < linhas; r++) {
    int resto = nFiltro - r * BIB_COLUNAS;
    cols[r + 1] = resto > BIB_COLUNAS ? BIB_COLUNAS : resto;
  }
  focus_iniciar(&foco, 1 + linhas, cols);
  // O foco volta para a barra de abas, nao para o primeiro poster: quem trocou
  // de aba ainda esta escolhendo aba, e jogar o foco na grade obriga a subir de
  // novo para experimentar a proxima.
  foco.coluna = aba;
  scrollY = 0.0f;
  memset(animFoco, 0, sizeof animFoco);
}

static int iniciado;
int biblioteca_iniciar(void) {
  // Zerado UMA vez por processo, e nao a cada entrada: o botao "Marcar" do
  // detalhe usa esta tabela, e apagar a escolha do usuario a cada visita a
  // faria sumir sem ele entender por que. Antes o padrao aqui era inventado —
  // (i%3)!=2 punha DOIS de cada tres titulos na "minha lista" e (i%4)==1
  // punha um de cada quatro em "Comprados", listas que nunca existiram. A
  // lista de verdade e a do Trakt, que marca ci->naLista/naColecao.
  if (!iniciado) {
    memset(naLista, 0, sizeof naLista);
    memset(comprado, 0, sizeof comprado);
    iniciado = 1;
  }
  aba = ABA_LISTA;
  sair = 0; pedido = -1;
  reconstruir();
  return 1;
}

void biblioteca_encerrar(void) { }

int biblioteca_na_lista(int i) {
  return (i >= 0 && i < CAT_MAX) ? naLista[i] : 0;
}
int biblioteca_comprado(int i) {
  return (i >= 0 && i < CAT_MAX) ? comprado[i] : 0;
}
void biblioteca_alternar_lista(int i) {
  if (i < 0 || i >= CAT_MAX) return;
  naLista[i] = !naLista[i];
  if (aba == ABA_LISTA) reconstruir();
}

int biblioteca_quer_sair(void) { return sair; }

int biblioteca_pediu_abrir(int *indiceCatalogo) {
  if (pedido < 0) return 0;
  if (indiceCatalogo) *indiceCatalogo = pedido;
  pedido = -1;
  return 1;
}

void biblioteca_evento(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  // Varias teclas contam como voltar: no aparelho e o Back do controle, no
  // teclado cada pessoa alcanca uma diferente.
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { sair = 1; return; }

  if (foco.fileira == 0) {
    // Trocar de aba refaz o mapa de foco, e reconstruir() zera o Foco inteiro —
    // por isso a aba muda AQUI e nao por focus_mover: chamar as duas coisas na
    // ordem errada devolvia o foco para a aba 0 a cada movimento.
    if (k == SDLK_RIGHT && aba < BIB_ABAS - 1) { aba++; reconstruir(); return; }
    if (k == SDLK_LEFT  && aba > 0)            { aba--; reconstruir(); return; }
    if (k == SDLK_DOWN) { focus_mover(&foco, 0, 1); return; }
    return;
  }

  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
    int i = (foco.fileira - 1) * BIB_COLUNAS + foco.coluna;
    if (i >= 0 && i < nFiltro) pedido = filtro[i];
    return;
  }
  if (k == SDLK_RIGHT)     focus_mover(&foco, 1, 0);
  else if (k == SDLK_LEFT) focus_mover(&foco, -1, 0);
  else if (k == SDLK_DOWN) focus_mover(&foco, 0, 1);
  else if (k == SDLK_UP)   focus_mover(&foco, 0, -1);
}

void biblioteca_atualizar(float dt, Uint32 agora) {
  (void)agora;
  for (int a = 0; a < BIB_ABAS; a++) {
    float alvo = (foco.fileira == 0 && foco.coluna == a) ? 1.0f : 0.0f;
    animAba[a] = anim_mola(animAba[a], alvo, dt,
                           alvo > animAba[a] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
  }
  for (int r = 0; r < BIB_MAX_LINHAS; r++)
    for (int c = 0; c < BIB_COLUNAS; c++) {
      float alvo = focus_indice(&foco, r + 1, c) ? 1.0f : 0.0f;
      animFoco[r][c] = anim_mola(animFoco[r][c], alvo, dt,
                                 alvo > animFoco[r][c] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
    }

  // Rola o MINIMO para a linha focada caber inteira na area util: primeiro
  // empurra para cima se a base saiu por baixo, depois puxa de volta se o topo
  // entrou por baixo das abas. Com o foco nas abas o alvo e 0 — voltar para o
  // topo faz parte de voltar para a barra.
  float alvo = scrollY;
  if (foco.fileira > 0) {
    float topo = BIB_GRADE_TOPO + (foco.fileira - 1) * passoLinha();
    float base = topo + alturaLinha();
    if (base - alvo > BIB_GRADE_BASE) alvo = base - BIB_GRADE_BASE;
    if (topo - alvo < BIB_GRADE_TOPO) alvo = topo - BIB_GRADE_TOPO;
  } else {
    alvo = 0.0f;
  }
  if (alvo < 0.0f) alvo = 0.0f;
  scrollY = anim_mola(scrollY, alvo, dt, NV_MOLA_SCROLL);
}

// Mesmo tratamento das abas de temporada do detalhe: escolhida sem foco fica
// com pilula translucida, com foco fica clara e o texto escurece. As duas telas
// tem que ler igual — sao a mesma peca de interface.
static void desenhaAba(GfxRect r, int a, float f) {
  int sel = (a == aba);
  float base = sel ? 0.30f : 0.0f;
  float lum = 0.62f + 0.34f * f;
  gfx_cor(r, NV_RAIO_PILL, lum, lum, lum, base + 0.66f * f);
  int cor = f > 0.55f ? 24 : (sel ? 250 : 196);
  TxtLinha l = txt_linha(TXT_BODY, ROTULOS[a], cor, cor, cor, 255);
  txt_desenhar(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f);
}

// Aba sem nenhum titulo. Uma grade em branco parece tela quebrada; o aparelho
// diz o que aconteceu e como sair dali.
static void desenhaVazio(void) {
  const char *linha1 = "Nada por aqui ainda";
  const char *linha2 = (aba == ABA_COMPRADOS)
      ? "Os filmes e programas que você comprar aparecem aqui."
      : "Adicione títulos com o botão + na página do título.";
  TxtLinha t1 = txt_linha(TXT_TITULO2, linha1, 255, 255, 255, 255);
  TxtLinha t2 = txt_linha(TXT_BODY, linha2, 168, 170, 178, 255);
  float y = BIB_GRADE_TOPO + (BIB_GRADE_BASE - BIB_GRADE_TOPO) * 0.32f;
  txt_desenhar_alpha(t1, (NV_TELA_W - t1.w) * 0.5f, y, 0.96f);
  txt_desenhar_alpha(t2, (NV_TELA_W - t2.w) * 0.5f, y + t1.h + 18, 0.85f);
}

void biblioteca_desenhar(Uint32 agora) {
  // Fundo opaco proprio: a biblioteca cobre a tela inteira e nao pode contar
  // com quem desenhou antes dela. Sem isto, abrir a tela por cima da home
  // deixaria os cards da home aparecendo entre os posteres.
  GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
  gfx_cor(tela, 0.0f, NV_COR_FUNDO_R, NV_COR_FUNDO_G, NV_COR_FUNDO_B, 1.0f);

  // Barra de abas centralizada: as tres pilulas somam 2 passos + uma largura.
  float largBarra = (BIB_ABAS - 1) * NV_ABA_PITCH + NV_ABA_W;
  float ax = (NV_TELA_W - largBarra) * 0.5f;
  for (int a = 0; a < BIB_ABAS; a++) {
    float f = animAba[a];
    float esc = 1.0f + NV_FOCO_ESCALA * f;
    float w = NV_ABA_W * esc, h = NV_ABA_H * esc;
    float cx = ax + a * NV_ABA_PITCH + NV_ABA_W * 0.5f;
    GfxRect r = { cx - w * 0.5f, BIB_ABAS_Y + (NV_ABA_H - h) * 0.5f, w, h };
    desenhaAba(r, a, f);
  }

  if (nFiltro == 0) { desenhaVazio(); return; }

  int linhas = nLinhas();
  if (linhas > BIB_MAX_LINHAS) linhas = BIB_MAX_LINHAS;
  float passoC = passoColuna(), passoL = passoLinha();

  // Dois passes como no resto do app: o item focado cresce e precisa ser
  // desenhado por ULTIMO, senao o vizinho da direita corta a borda dele.
  for (int passe = 0; passe < 2; passe++)
    for (int r = 0; r < linhas; r++) {
      float topo = BIB_GRADE_TOPO + r * passoL - scrollY;
      if (topo > NV_TELA_H || topo + alturaLinha() < -80.0f) continue;
      // O que sobe para baixo da barra de abas some antes de cruza-la: sem o
      // esmaecimento, poster e rotulo de aba se leem um sobre o outro.
      float a = anim_clamp((topo - (BIB_ABAS_Y + NV_ABA_H * 0.4f)) / BIB_FADE, 0.0f, 1.0f);
      if (a <= 0.005f) continue;

      for (int c = 0; c < BIB_COLUNAS; c++) {
        int i = r * BIB_COLUNAS + c;
        if (i >= nFiltro) break;
        float f = animFoco[r][c];
        if ((passe == 0) == (f > 0.01f)) continue;

        // Poster 2:3 cresce 14% no foco, e nao os 9% do card 16:9 — numeros das
        // tabelas oficiais de Top Shelf, que publicam os dois tamanhos.
        float esc = 1.0f + NV_FOCO_ESCALA_P * f;
        float w = NV_POSTER_W * esc, h = NV_POSTER_H * esc;
        float cx = NV_LEGACY_CONTENT_X + c * passoC + NV_POSTER_W * 0.5f;
        float cy = topo + NV_POSTER_H * 0.5f - NV_FOCO_LIFT * f;
        GfxRect card = { cx - w * 0.5f, cy - h * 0.5f, w, h };

        const CatItem *ci = cat_item(filtro[i]);
        const char *arte = (ci && ci->poster[0]) ? ci->poster : NULL;
        GLuint tex = arte ? tex_obter(arte) : 0;
        if (tex) {
          // Sem o aspecto a arte estica; com ele o shader recorta (cover).
          gfx_tex_aspect_atual = tex_aspecto(arte);
          float fase = agora / 1000.0f + (r * 3 + c) * 0.6f;
          gfx_rect(card, tex, GFX_CARD, f, sinf(fase) * 0.010f * f,
                   cosf(fase * 0.8f) * 0.006f * f, NV_RAIO_CARD, 0, 0, 0, a);
          gfx_tex_aspect_atual = 0.0f;
        } else {
          // Placeholder na cor do fundo dos cards: o poster ainda esta
          // decodificando em outra thread e a grade nao pode piscar buraco.
          gfx_cor(card, NV_RAIO_CARD, 0.14f, 0.14f, 0.16f, a);
        }

        if (aba == ABA_GENEROS) {
          TxtLinha lg = txt_linha(TXT_CAPTION2, generoPrincipal(ci), 236, 237, 242, 255);
          txt_desenhar_alpha(lg, cx - NV_POSTER_W * 0.5f,
                             topo + NV_POSTER_H + 14.0f, a * 0.92f);
        }
      }
    }
}
