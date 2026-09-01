// Home nativa compatível com a interface moderna do Nuvio 1.0.1 legacy:
// hero no topo, rail fixa à esquerda e fileiras horizontais de posters. A
// infraestrutura nativa cuida de cache assíncrono, foco e transições.
#include "home.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "catalogo.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <math.h>

#define MAX_ARTE   64
#define MAX_FIL    6
#define MAX_CARDS  12

typedef struct {
  const char *titulo;
  TipoFileira tipo;
  int n;
} Fileira;

static char bd[MAX_ARTE][512];    int nBd = 0;    // backdrops 16:9
static char pst[MAX_ARTE][512];   int nPst = 0;   // posters 2:3

static Fileira fileiras[MAX_FIL] = {
  { "Continue Assistindo", FILEIRA_CONTINUE, 8 },
  { "Popular - Movie",     FILEIRA_NORMAL,   8 },
  { "Popular - Series",    FILEIRA_NORMAL,   8 },
  { "Em alta",             FILEIRA_NORMAL,   8 },
};
static int nFileiras = 4;

static const char *TITULOS_DEMO[] = {
  "Ruptura", "Silo", "Ted Lasso", "Foundation", "For All Mankind",
  "Servant", "Invasao", "Teera", "Shrinking", "Presumed Innocent"
};
static const char *GENEROS_DEMO[] = {
  "Programa de TV  \xc2\xb7  Drama", "Programa de TV  \xc2\xb7  Suspense",
  "Programa de TV  \xc2\xb7  Ficcao cientifica", "Filme  \xc2\xb7  Acao",
  "Programa de TV  \xc2\xb7  Comedia", "Filme  \xc2\xb7  Misterio"
};
static const char *META_DEMO[] = {
  "T2, E10 \xc2\xb7 1 h 24 min", "T1, E1 \xc2\xb7 58 min", "T3, E4 \xc2\xb7 32 min",
  "T2, E6 \xc2\xb7 51 min", "T4, E2 \xc2\xb7 1 h 5 min", "T1, E4 \xc2\xb7 31 min"
};

static Foco foco;
static HomeItem itemFoco;      // preenchido durante o desenho, lido pela transicao
static int  temItemFoco = 0;
static float animFoco[MAX_FIL][MAX_CARDS];
static float scrollX[MAX_FIL];
static float scrollY = 0.0f;
static int sair = 0, pedidoAbrir = 0, pedidoMenu = 0;

// --- hero-carrossel ---
static int heroAtual = 0, heroAnterior = 0;
static Uint32 heroTrocaEm = 0;
static float heroFade = 1.0f;   // 1 = so o atual; <1 = mistura com o anterior

static void carregaDir(const char *dir, char destino[][512], int *n, const char *sub) {
  char caminho[512];
  if (sub) snprintf(caminho, sizeof caminho, "%s/%s", dir, sub);
  else snprintf(caminho, sizeof caminho, "%s", dir);
  DIR *d = opendir(caminho);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) && *n < MAX_ARTE) {
    if (!strstr(e->d_name, ".jpg") && !strstr(e->d_name, ".png")) continue;
    snprintf(destino[*n], 512, "%s/%s", caminho, e->d_name);
    (*n)++;
  }
  closedir(d);
}

static float larguraDe(TipoFileira t) {
  switch (t) {
    case FILEIRA_CONTINUE: return NV_DESTAQUE_W;
    default:               return NV_CARD_W;
  }
}
// Quantos titulos o hero percorre. Vem do catalogo quando existe.
static int nAcervoHero(void) { int n = cat_n(); if (n) return n; return nBd ? nBd : 1; }

static float alturaDe(TipoFileira t) {
  switch (t) {
    case FILEIRA_CONTINUE: return NV_DESTAQUE_H;
    default:               return NV_CARD_H;
  }
}
// O gap do tvOS e fixo em 40px e ja foi dimensionado para caber o crescimento
// do foco: um card de 410 crescendo 9% invade 18px de cada lado. O card
// destaque e maior que qualquer coisa que a Apple usa, entao para ele o gap
// segue proporcional — senao a invasao (31px) come quase todo o respiro.
static float gapDe(TipoFileira t) {
  (void)t;
  return NV_CARD_GAP;
}
// Poster 2:3 cresce mais que card 16:9 — numeros das tabelas oficiais.
static float escalaDe(TipoFileira t) {
  (void)t;
  return NV_FOCO_ESCALA;
}
static float passoDe(TipoFileira t) {
  return larguraDe(t) + gapDe(t);
}

int home_iniciar(const char *dirArte) {
  cat_carregar(dirArte);
  carregaDir(dirArte, bd, &nBd, NULL);
  carregaDir(dirArte, pst, &nPst, "poster");
  if (!nBd) { printf("home: nenhum backdrop em %s\n", dirArte); return 0; }
  if (!nPst) { printf("home: sem posters retrato, Top 10 usara backdrop\n"); }

  // No layout moderno legacy o hero é informativo; a navegação começa na
  // primeira fileira de conteúdo (como buildModernNavigationRows()).
  int cols[MAX_FIL];
  for (int i = 0; i < nFileiras; i++) cols[i] = fileiras[i].n;
  focus_iniciar(&foco, nFileiras, cols);
  heroTrocaEm = SDL_GetTicks() + NV_HERO_INTERVALO_MS;
  printf("home: %d backdrops, %d posters, %d fileiras\n", nBd, nPst, nFileiras);
  return 1;
}

void home_evento(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { sair = 1; return; }
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
#ifdef __APPLE__
  // Rodando no Mac, o Back na home NAO fecha: fechar a janela no meio de um
  // teste custa recompilar e reabrir. No aparelho ele sai do app, como deve.
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE) return;
  if (k == SDLK_q) { sair = 1; return; }
#else
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK) { sair = 1; return; }
#endif
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) { pedidoAbrir = 1; return; }
  if (k == SDLK_RIGHT) {
    // O `&&` aqui era um curto-circuito com efeito colateral: escrito como
    // `if (fileira == 0 && !focus_mover(...))`, o focus_mover so era chamado
    // NO HERO — em qualquer outra fileira a seta direita nao movia nada. Mover
    // primeiro, decidir depois.
    (void)focus_mover(&foco, 1, 0);
  } else if (k == SDLK_LEFT) {
  // Esquerda na primeira coluna chama o menu lateral, em QUALQUER fileira —
    // inclusive no hero. Antes o hero era excecao e usava a esquerda para
    // voltar um titulo no carrossel: quem chegava ali (voltando de outra tela,
    // por exemplo) nao tinha como abrir o menu sem antes descer. O carrossel
    // continua acessivel pela direita e pela troca automatica.
    if (foco.coluna == 0) { pedidoMenu = 1; return; }
    focus_mover(&foco, -1, 0);
  }
  else if (k == SDLK_DOWN)  focus_mover(&foco, 0, 1);
  else if (k == SDLK_UP)    focus_mover(&foco, 0, -1);
}

void home_atualizar(float dt, Uint32 agora) {
  // O carrossel fica ativo enquanto a home está visível. A troca é lenta e
  // independente do foco, igual à rotação automática do hero legacy.
  if (agora >= heroTrocaEm) {
    heroAnterior = heroAtual;
    heroAtual = (heroAtual + 1) % nAcervoHero();
    heroFade = 0.0f;
    heroTrocaEm = agora + NV_HERO_INTERVALO_MS;
  }
  if (heroFade < 1.0f) {
    heroFade += dt * (1000.0f / NV_HERO_FADE_MS);
    if (heroFade > 1.0f) heroFade = 1.0f;
  }

  // O passeio automatico do foco era so para ver o protótipo se mexendo sem
  // ninguem no controle. Com o app navegavel ele atrapalha: rouba o foco no
  // meio de qualquer teste.

  for (int r = 0; r < nFileiras; r++) {
    for (int c = 0; c < fileiras[r].n; c++) {
      float alvo = focus_indice(&foco, r, c) ? 1.0f : 0.0f;
      animFoco[r][c] = anim_mola(animFoco[r][c], alvo, dt,
                                 alvo > animFoco[r][c] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
    }
    if (r == foco.fileira) {
      // Roda so o necessario para o item focado caber na area util. Deslocar
      // proporcional a coluna, como estava, jogava o primeiro card para fora da
      // tela assim que o foco ia para o segundo — some conteudo a esquerda sem
      // que o usuario tenha andado ate la.
      float passo = passoDe(fileiras[r].tipo), lw = larguraDe(fileiras[r].tipo);
      float esq = (float)foco.coluna * passo;
      float dir = esq + lw;
      float util = NV_TELA_W - NV_LEGACY_CONTENT_X - NV_LEGACY_CONTENT_RIGHT;
      float alvo = scrollX[r];
      // a folga cobre o crescimento do foco: o card cresce para os dois lados,
      // e sem reservar essa metade ele encosta na borda ao ficar em foco
      float folga = lw * escalaDe(fileiras[r].tipo) * 0.5f;
      if (dir + folga - alvo > util)  alvo = dir + folga - util;
      if (esq - alvo < 0.0f)  alvo = esq;
      if (alvo < 0) alvo = 0;
      scrollX[r] = anim_mola(scrollX[r], alvo, dt, NV_MOLA_SCROLL);
    }
  }

  // O viewport legacy começa abaixo do hero. Quando o foco desce, a camera move
  // apenas o mínimo para manter a fileira dentro da metade inferior.
  float alvoY = 0.0f;
  if (foco.fileira > 0) {
    int r = foco.fileira;
    float topo = NV_SHELF_TOP;
    for (int i = 0; i < r; i++) {
      topo += NV_LEGACY_ROW_HEAD_H + alturaDe(fileiras[i].tipo) + NV_FILEIRA_GAP;
    }
    float hFoco = alturaDe(fileiras[r].tipo);
    float cardTopo = topo + NV_LEGACY_ROW_HEAD_H;
    float viewportTop = NV_SHELF_TOP + NV_LEGACY_ROW_HEAD_H;
    float viewportBottom = NV_TELA_H - NV_MARGEM_Y;
    float alvo = scrollY;
    if (cardTopo - alvo < viewportTop) alvo = cardTopo - viewportTop;
    if (cardTopo + hFoco - alvo > viewportBottom) {
      alvo = cardTopo + hFoco - viewportBottom;
    }
    if (alvo < 0.0f) alvo = 0.0f;
    alvoY = alvo;
  }
  scrollY = anim_mola(scrollY, alvoY, dt, NV_MOLA_SCROLL);
}

// ---------- Hero do layout moderno legacy ----------------------------------
//
// A mídia ocupa a direita dos 650px superiores; o texto fica no bloco esquerdo
// e as fileiras rolam em um viewport independente abaixo. O hero não captura
// foco: a navegação espacial começa no primeiro card, como no DOM legacy.
static void desenhaHero(Uint32 agora) {
  (void)agora;
  float aArte = 1.0f;
  // MEDIDO no app web: .home-modern-hero-media fica em x=555, y=0, 1421x670.
  // A conta que estava aqui (0.28*W - 56 = 481,6 de largura 1438) vinha de
  // proporcao estimada e punha a arte 73px a esquerda do lugar.
  GfxRect r = { NV_HERO_ARTE_X, 0, NV_HERO_ARTE_W, NV_HERO_ARTE_H };

  const CatItem *ci = cat_item(heroAtual);
  const char *arteA = (ci && ci->backdrop[0]) ? ci->backdrop : bd[heroAtual];
  const CatItem *cAnt = cat_item(heroAnterior);
  const char *arteB = (cAnt && cAnt->backdrop[0]) ? cAnt->backdrop : bd[heroAnterior];

  GLuint tAnt = tex_obter(arteB);
  GLuint tAtu = tex_obter(arteA);
  if (heroFade < 1.0f && tAnt) {
    gfx_tex_aspect_atual = tex_aspecto(arteB);
    gfx_rect(r, tAnt, GFX_HERO, 0, 0, 0, 0.0f, 0, 0, 0, aArte);
  }
  if (tAtu) {
    gfx_tex_aspect_atual = tex_aspecto(arteA);
    gfx_rect(r, tAtu, GFX_HERO, 0, 0, 0, 0.0f, 0, 0, 0, heroFade * aArte);
  }
  gfx_tex_aspect_atual = 0.0f;

  float aTexto = 1.0f;

  TxtLinha sub = txt_linha(TXT_CAPTION, (ci && ci->genero[0]) ? ci->genero
                           : "Programa de TV  \xc2\xb7  Suspense", 226, 228, 233, 255);
  // A linha de genero comeca com o LOGO DO SERVICO e termina com a
  // classificacao — e nessa ordem que ela aparece no aparelho.
  float wServ = 0.0f;
  GLuint tserv = (ci && ci->provLogo[0]) ? tex_obter(ci->provLogo) : 0;
  if (tserv) {
    float ap = tex_aspecto(ci->provLogo);
    if (ap <= 0.0f) ap = 1.0f;
    wServ = sub.h * 1.15f * ap + 14.0f;
  }
  const char *sinopse = (ci && ci->sinopse[0]) ? ci->sinopse : "Novos episodios as sextas.";
  float largSin = NV_HERO_SIN_W;

  // logo do titulo; nome em texto so quando o titulo nao tem logo. Ancorado no
  // TOPO (NV_HERO_LOGO_Y), como o web — nao empilhado a partir da base.
  GLuint tlogo = (ci && ci->logo[0]) ? tex_obter(ci->logo) : 0;
  if (tlogo) {
    float ap = tex_aspecto(ci->logo);
    if (ap <= 0.0f) ap = 4.0f;
    float hTit = NV_LOGO_HERO_H;
    float wTit = hTit * ap;
    if (wTit > NV_LOGO_HERO_MAX_W) { wTit = NV_LOGO_HERO_MAX_W; hTit = wTit / ap; }
    // O logo cresce para CIMA a partir da base da caixa reservada, para que um
    // logo baixo e largo nao flutue no meio dela.
    GfxRect rl = { NV_LEGACY_CONTENT_X,
                   NV_HERO_LOGO_Y + (NV_LOGO_HERO_H - hTit), wTit, hTit };
    gfx_tex_aspect_atual = 0.0f;
    gfx_rect(rl, tlogo, GFX_TEXTO, 0, 0, 0, 0.0f, 1, 1, 1, aTexto * heroFade);
  } else {
    // Sem logo o web usa .home-hero-title-text: 76px, peso 600. TITULO1 e 76.
    TxtLinha tit = txt_linha(TXT_TITULO1, (ci && ci->titulo[0]) ? ci->titulo
                             : TITULOS_DEMO[heroAtual % 10], 255, 255, 255, 255);
    txt_desenhar_alpha(tit, NV_LEGACY_CONTENT_X,
                       NV_HERO_LOGO_Y + (NV_LOGO_HERO_H - (float)tit.h),
                       aTexto * heroFade);
  }

  float y = NV_HERO_META_Y;
  if (tserv) {
    float hs = sub.h * 1.15f;
    GfxRect rs = { NV_LEGACY_CONTENT_X, y + (sub.h - hs) * 0.5f, wServ - 14.0f, hs };
    gfx_tex_aspect_atual = 0.0f;
    gfx_rect(rs, tserv, GFX_CARD, 0, 0, 0, 0.5f, 0, 0, 0, aTexto * heroFade);
  }
  txt_desenhar_alpha(sub, NV_LEGACY_CONTENT_X + wServ, y, aTexto * heroFade);
  if (ci && ci->classificacao[0]) {
    char cl[8]; snprintf(cl, sizeof cl, "A%s", ci->classificacao);
    TxtLinha lb = txt_linha(TXT_MINI, cl, 255, 255, 255, 255);
    GfxRect bg = { NV_LEGACY_CONTENT_X + wServ + sub.w + 14, y + (sub.h - lb.h) * 0.5f,
                   lb.w + 8, lb.h + 2 };
    gfx_cor(bg, 0.20f, 0.80f, 0.34f, 0.10f, 0.92f * aTexto * heroFade);
    txt_desenhar_alpha(lb, bg.x + 4, bg.y + 1, aTexto * heroFade);
  }
  txt_bloco(TXT_CAPTION, sinopse, 206, 208, 216, NV_LEGACY_CONTENT_X,
            NV_HERO_SIN_Y, largSin,
            NV_LD_CAPTION, aTexto * heroFade * 0.95f, 2);
}

// Fundo CINZA, e so. Eu tinha posto aqui a arte do titulo em destaque
// desfocada, achando que era isso o "cinza do Apple TV" — mas o efeito era o
// oposto do pedido: a arte do hero subia e saia normalmente, e a copia
// desfocada dela continuava no fundo, dando a impressao de que a imagem nunca
// tinha subido. Fundo neutro nao compete com nada.
static void desenhaFundo(void) {
  GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
  gfx_cor(tela, 0.0f, NV_COR_FUNDO_R, NV_COR_FUNDO_G, NV_COR_FUNDO_B, 1.0f);
}

void home_desenhar(Uint32 agora) {
  desenhaFundo();
  desenhaHero(agora);

  float y = NV_SHELF_TOP - scrollY;
  for (int r = 0; r < nFileiras; r++) {
    TipoFileira tipo = fileiras[r].tipo;
    float lw = larguraDe(tipo), lh = alturaDe(tipo), passo = passoDe(tipo);
    float artH = lh;
    // `y` é o topo do cabeçalho da fileira; os cards começam depois do título.
    // Separar os dois evita que o título da fileira seguinte seja desenhado
    // sobre a arte da anterior quando a fileira tem cards altos.
    float cardY = y + NV_LEGACY_ROW_HEAD_H;

    if (y < NV_TELA_H + 200 && y + NV_LEGACY_ROW_HEAD_H + lh > -200) {
      TxtLinha tl = txt_linha(TXT_ROW_TITULO, fileiras[r].titulo, 255, 255, 255, 255);
      txt_desenhar(tl, NV_LEGACY_CONTENT_X, y);

      for (int passe = 1; passe < 2; passe++) {
        for (int c = 0; c < fileiras[r].n; c++) {
          float f = animFoco[r][c];
          if (passe == 0 && f < 0.01f) continue;
          float esc = 1.0f + escalaDe(tipo) * f;
          float w = lw * esc, h = artH * esc;
          float cx = NV_LEGACY_CONTENT_X + c * passo - scrollX[r] + lw * 0.5f;
          float cy = cardY + artH * 0.5f - NV_FOCO_LIFT * f;
          if (cx < -lw * 1.5f || cx > NV_TELA_W + lw) continue;
          float px = cx - w * 0.5f, py = cy - h * 0.5f;

          if (passe == 0) {
            // Sem sombra. Ela existia para separar o card do fundo, mas sobre
            // arte colorida vira um halo escuro em volta do item focado — e o
            // aparelho nao tem isso: la o foco se marca por escala e brilho.
            (void)f;
            continue;
          }

          const int idxCat = r * 8 + c;
          const CatItem *cItem = cat_item(idxCat);
          const char *caminho;
          if (tipo == FILEIRA_CONTINUE)
            caminho = (cItem && cItem->backdrop[0]) ? cItem->backdrop
                    : (cItem && cItem->poster[0]) ? cItem->poster
                    : (nBd ? bd[idxCat % nBd] : NULL);
          else
            caminho = (cItem && cItem->poster[0]) ? cItem->poster
                    : (cItem && cItem->backdrop[0]) ? cItem->backdrop
                    : (nPst ? pst[idxCat % nPst] : NULL);
          if (!caminho) continue;

          if (focus_indice(&foco, r, c)) {
            GfxRect aqui = { px, py, w, h };
            itemFoco.indice = idxCat;
            itemFoco.rect   = aqui;
            itemFoco.arte   = caminho;
            itemFoco.titulo = cItem ? cItem->titulo : NULL;
            itemFoco.genero = cItem ? cItem->genero : NULL;
            itemFoco.meta   = cItem ? cItem->meta : NULL;
            temItemFoco = 1;
          }
          GLuint t = tex_obter(caminho);
          if (f > 0.01f) {
            GfxRect borda = { px - 3.0f, py - 3.0f, w + 6.0f, h + 6.0f };
            gfx_cor(borda, NV_RAIO_CARD, 0.20f, 0.62f, 0.96f, 0.88f * f);
          }
          GfxRect card = { px, py, w, h };
          if (t) {
            float fase = agora / 1000.0f + (r * 3 + c) * 0.6f;
            gfx_tex_aspect_atual = tex_aspecto(caminho);
            gfx_rect(card, t, GFX_CARD, f,
                     sinf(fase) * 0.010f * f, cosf(fase * 0.8f) * 0.006f * f,
                     NV_RAIO_CARD, 0, 0, 0, 1);
            gfx_tex_aspect_atual = 0.0f;
          } else {
            gfx_cor(card, NV_RAIO_CARD, 0.14f, 0.14f, 0.16f, 1.0f);
          }

          // O ranking legacy usa a mesma carta retrato das demais fileiras;
          // a posição só aparece quando a fonte de dados fornece esse rótulo.

          // 1. CONTINUE: legenda e barra DENTRO do card, sobre a arte.
          if (tipo == FILEIRA_CONTINUE) {
            const CatItem *ci = cItem;
            float frac = ci ? ci->progresso / 100.0f : 0.0f;
            if (ci && (ci->restanteMin > 0 || frac > 0.0f)) {
              // Veu na base: a legenda cai sobre arte de qualquer cor e sem ele
              // some no claro. Mesmo recurso que a fileira de destaque usa.
              GfxRect veu = { px, py, w, h };
              gfx_rect(veu, 0, GFX_VEU, 0, 0, 0, NV_RAIO_CARD, 0, 0, 0, 0.55f);
            }
            if (ci && ci->restanteMin > 0) {
              char leg[48];
              // Serie diz onde o dono parou e quanto falta; filme diz so quanto
              // falta, porque "T0, E0" nao significa nada. Temporada zerada e
              // justamente o que separa os dois casos.
              if (ci->temporada > 0 && ci->episodio > 0)
                snprintf(leg, sizeof leg, "T%d, E%d  \xc2\xb7  %d min",
                         ci->temporada, ci->episodio, ci->restanteMin);
              else
                snprintf(leg, sizeof leg, "%d min", ci->restanteMin);
              TxtLinha ll = txt_linha(TXT_CAPTION2, leg, 236, 237, 242, 255);
              txt_desenhar_alpha(ll, px + 16, py + h - 46, 0.95f);
            }
            if (frac > 0.0f) {
              // Trilho quase invisivel e barra fina: a 26% de alpha sobre arte
              // clara o trilho lia como barra cheia, e todo card parecia estar
              // no fim do episodio.
              float bx = px + 16, by2 = py + h - 18, bw2 = w - 32, bh2 = 4;
              GfxRect trilha = { bx, by2, bw2, bh2 };
              GfxRect ativo  = { bx, by2, bw2 * anim_clamp(frac, 0.02f, 1.0f), bh2 };
              gfx_cor(trilha, 0.5f, 1, 1, 1, 0.16f);
              gfx_cor(ativo,  0.5f, 1, 1, 1, 0.95f);
            }
          }

          // 4. DESTAQUE: titulo e metadados DENTRO da arte, sobre um veu
          // escuro na base — como o Apple TV faz. O titulo faz o papel do logo
          // embutido na arte-chave, que nos nao temos (o TMDB nem sempre tem
          // logo; quando tiver, entra aqui no lugar do texto).
          if (tipo == FILEIRA_DESTAQUE) {
            GfxRect veu = { px, py, w, h };
            gfx_rect(veu, 0, GFX_VEU, 0, 0, 0, NV_RAIO_CARD, 0, 0, 0, 0.88f);

            // Logo do titulo, como no aparelho: cada producao tem tipografia
            // propria, e escrever o nome com a fonte da interface apaga isso.
            const CatItem *ci = cat_item(c + 2);
            GLuint tlogo = (ci && ci->logo[0]) ? tex_obter(ci->logo) : 0;
            const char *nome = (ci && ci->titulo[0])
                             ? ci->titulo
                             : TITULOS_DEMO[(c + 2) % (int)(sizeof TITULOS_DEMO / sizeof *TITULOS_DEMO)];
            const char *genero = (ci && ci->genero[0])
                               ? ci->genero
                               : GENEROS_DEMO[c % (int)(sizeof GENEROS_DEMO / sizeof *GENEROS_DEMO)];
            TxtLinha tg = txt_linha(TXT_CAPTION, genero, 226, 228, 233, 255);

            float pad = 26.0f * (w / NV_DESTAQUE_W);
            float base = py + h - pad;
            float yMeta = base - tg.h;
            float escLogo = w / NV_DESTAQUE_W;   // acompanha o card ao crescer
            float hTit;
            if (tlogo) {
              float ap = tex_aspecto(ci->logo);
              if (ap <= 0.0f) ap = 4.0f;
              hTit = NV_LOGO_CARD_H * escLogo;
              float wTit = hTit * ap, maxW = w - pad * 2;
              if (wTit > maxW) { wTit = maxW; hTit = wTit / ap; }
              GfxRect rl = { px + pad, yMeta - hTit - 10.0f, wTit, hTit };
              gfx_tex_aspect_atual = 0.0f;
              gfx_rect(rl, tlogo, GFX_TEXTO, 0, 0, 0, 0.0f, 1, 1, 1, 1.0f);
            } else {
              TxtLinha tn = txt_linha(TXT_TITULO3, nome, 255, 255, 255, 255);
              hTit = (float)tn.h;
              txt_desenhar(tn, px + pad, yMeta - hTit - 10.0f);
            }
            txt_desenhar(tg, px + pad, yMeta);

            // badge etario vermelho, a direita da linha de genero
            char clas[8];
            snprintf(clas, sizeof clas, "A%s", (ci && ci->classificacao[0]) ? ci->classificacao : "16");
            TxtLinha tb = txt_linha(TXT_CAPTION, clas, 255, 255, 255, 255);
            float bx = px + pad + tg.w + 14.0f;
            GfxRect badge = { bx, yMeta + 2, tb.w + 16, tg.h - 4 };
            gfx_cor(badge, NV_RAIO_BADGE, 0.78f, 0.14f, 0.14f, 0.95f);
            txt_desenhar(tb, bx + 8, yMeta + 2);
          }
        }
      }
    }
    y += NV_LEGACY_ROW_HEAD_H + lh + NV_FILEIRA_GAP;
  }
}

void home_encerrar(void) {}
int home_quer_sair(void) { return sair; }

int home_item_focado(HomeItem *out) {
  if (!temItemFoco) return 0;
  *out = itemFoco;
  return 1;
}

int home_n_artes(void) { return nBd; }
// Quando ha catalogo, a arte vem dele (na ordem certa, casada com o titulo);
// sem catalogo, cai na varredura da pasta.
const char *home_backdrop(int i) {
  const CatItem *c = cat_item(i);
  if (c && c->backdrop[0]) return c->backdrop;
  return home_arte(i);
}
const char *home_arte(int i) { return (nBd && i >= 0) ? bd[i % nBd] : NULL; }

// Consome o pedido de abrir: quem le, zera. Assim o OK vale uma vez so, mesmo
// que o quadro demore.
int home_pediu_abrir(void) { int v = pedidoAbrir; pedidoAbrir = 0; return v; }

// Consome o pedido de abrir o menu lateral: quem le, zera.
int home_pediu_menu(void) { int v = pedidoMenu; pedidoMenu = 0; return v; }
