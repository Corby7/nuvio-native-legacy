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
#include "ajustes.h"
#include "catalogo.h"
// Declarado a mao em vez de incluir detail.h: aquele header inclui ESTE (por
// causa do HomeItem), e o ciclo so nao explode por causa das guardas. Uma
// funcao de uma linha nao vale amarrar os dois arquivos.
float detail_progresso(void);
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <math.h>

#define MAX_ARTE   64
// 16, o teto do web para ESTE runtime: HOME_MAX_ROWS_LEGACY_TV em
// js/ui/screens/home/homeConstants.js, que e o ramo escolhido por
// isLegacyTvRuntime(). O HOME_MAX_ROWS_DEFAULT de 40 e do navegador de mesa.
#define MAX_FIL    16
#define MAX_CARDS  12

typedef struct {
  const char *titulo;
  TipoFileira tipo;
  int n;
  // Primeiro item DESTA fileira no catalogo. Antes o desenho fazia `r * 8 + c`,
  // ou seja, cada fileira era uma janela fixa de 8 no vetor plano — o que so
  // funcionava porque as fileiras eram quatro e cravadas. Com as fileiras
  // vindo dos catalogos dos addons cada uma tem tamanho proprio.
  int ini;
} Fileira;

static char bd[MAX_ARTE][512];    int nBd = 0;    // backdrops 16:9
static char pst[MAX_ARTE][512];   int nPst = 0;   // posters 2:3

// RESERVA, e so isso: e o que a home mostra enquanto a rede nao respondeu, ou
// quando nao respondeu nenhuma. As fileiras de verdade vem de cat_fileira(),
// montadas em descoberta.c a partir dos catalogos que os addons declaram.
static Fileira fileiras[MAX_FIL] = {
  { "Continuar assistindo", FILEIRA_CONTINUE, 8, 0  },
  { "Popular - Filme",      FILEIRA_NORMAL,   8, 8  },
  { "Popular - S\xc3\xa9rie", FILEIRA_NORMAL, 8, 16 },
  { "Em alta",              FILEIRA_NORMAL,   8, 24 },
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

// A forma do card sai de DUAS preferencias, e nao do tipo da fileira:
//
//   `modernLandscapePostersEnabled` troca o poster 2:3 (212x322) pelo card
//   deitado 16:9 (318x182.9). MEDIDO no app web com a preferencia ligada.
//
//   `continueWatchingCardStyle` decide a fileira de "Continuar assistindo":
//   "card" e "largo" desenham deitado, "poster" usa o mesmo 2:3 das outras.
static float larguraDe(TipoFileira t) {
  switch (t) {
    case FILEIRA_CONTINUE: return NV_DESTAQUE_W;
    default:               return ajustes_posteres_deitados() ? NV_CARD_LAND_W
                                                              : NV_CARD_W;
  }
}
// Quantos titulos o hero percorre. Vem do catalogo quando existe.
static int nAcervoHero(void) { int n = cat_n(); if (n) return n; return nBd ? nBd : 1; }

static float alturaDe(TipoFileira t) {
  switch (t) {
    case FILEIRA_CONTINUE: return NV_DESTAQUE_H;
    default:               return ajustes_posteres_deitados() ? NV_CARD_LAND_H
                                                              : NV_CARD_H;
  }
}
// Altura TOTAL que a fileira ocupa: a arte mais o bloco de rotulo, quando ele
// existe. Sem somar o rotulo aqui, a fileira seguinte sobe por cima do texto —
// foi o mesmo defeito que o titulo de fileira ja tinha tido sobre os cards.
static int temRotulo(TipoFileira t) {
  // O rotulo abaixo do poster so existe no poster EM PE. No card deitado o web
  // poe a legenda DENTRO da moldura (.home-poster-landscape-copy) e esconde o
  // bloco de fora (.home-poster-card.is-landscape .home-poster-copy{display:none}).
  return t != FILEIRA_CONTINUE && ajustes_rotulos_poster()
      && !ajustes_posteres_deitados();
}
static float alturaTotalDe(TipoFileira t) {
  return alturaDe(t) + (temRotulo(t) ? NV_POSTER_COPY_H : 0.0f);
}
// O gap do tvOS e fixo em 40px e ja foi dimensionado para caber o crescimento
// do foco: um card de 410 crescendo 9% invade 18px de cada lado. O card
// destaque e maior que qualquer coisa que a Apple usa, entao para ele o gap
// segue proporcional — senao a invasao (31px) come quase todo o respiro.
static float gapDe(TipoFileira t) {
  (void)t;
  return NV_CARD_GAP;
}
// Passo vertical entre fileiras. `.home-modern-landscape-posters` aperta o
// `--home-row-gap` de 32 para 24 (components.css:6473) — a fileira deitada e
// mais baixa e o respiro do poster em pe sobraria nela.
static float fileiraGap(void) {
  return ajustes_posteres_deitados() ? NV_FILEIRA_GAP_LAND : NV_FILEIRA_GAP;
}
// Raio do card, em fracao do menor lado (o SDF do shader e normalizado). Este e
// o UNICO numero do card moderno que sai mesmo de `posterCardCornerRadiusDp`:
// 12dp x 2 = 24px, conferido no app rodando. A largura NAO sai de la (ver a
// nota em ajustes.h).
static float raioDe(float w, float h) {
  float menor = w < h ? w : h;
  if (menor <= 0.0f) return NV_RAIO_CARD;
  return ajustes_raio_poster_px() / menor;
}

// --- Profundidade dos cartoes (`cardDepth*`) ---------------------------------
// O web faz isto com dois pseudo-elementos sobre a arte: um brilho na borda de
// CIMA com opacidade `--card-depth-edge` e uma faixa clara e discreta —
// `--card-depth-sheen` — atravessando a parte alta do cartao. `--card-depth-
// coverage` engorda a banda da borda: `12 + round(18 * coverage)` px
// (layoutPreferences.js:181). Sao os mesmos tres numeros da tela de Ajustes.
static void desenhaProfundidade(GfxRect card, float raio, int ligadaAqui) {
  if (!ajustes_profundidade() || !ligadaAqui) return;
  float borda = ajustes_profundidade_borda();
  float brilho = ajustes_profundidade_brilho();
  float cobertura = ajustes_profundidade_cobertura();
  if (borda > 0.001f) {
    float h = 12.0f + 18.0f * cobertura;
    GfxRect faixa = { card.x, card.y, card.w, h };
    // Raio proporcional: a faixa e muito mais baixa que o card, entao repetir a
    // fracao do card arredondaria demais e a borda descolaria do canto.
    gfx_cor(faixa, raio * (card.h / (h > 0.0f ? h : 1.0f)) * 0.5f,
            1.0f, 1.0f, 1.0f, borda * 0.55f);
  }
  if (brilho > 0.001f) {
    GfxRect refl = { card.x, card.y + card.h * 0.06f, card.w, card.h * 0.28f };
    gfx_cor(refl, raio, 1.0f, 1.0f, 1.0f, brilho * 0.18f);
  }
}
// ZERO. MEDIDO no app web (sessao logada, perfil do dono): o card em foco tem
// `transform: none`, `scale: none` e o mesmo getBoundingClientRect do card ao
// lado — 212x322 nos dois, mesma linha, mesmo topo. A escala de 9% e o
// levantamento de 8px vinham das tabelas de Top Shelf do tvOS, e nao desta
// interface. Eram eles que faziam o card focado subir 22px e encostar no titulo
// da fileira, que fica 15px acima dos cards (titulo 518..549, cards em 564).
//
// O foco no web se marca por um ANEL de 2px `#f5f5f5` desenhado por dentro e
// por fora da arte (box-shadow inset + outset), com o card mantendo a caixa.
static float escalaDe(TipoFileira t) {
  (void)t;
  return 0.0f;
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

// Reconstroi a lista a partir do catalogo. Chamada a cada quadro porque a
// descoberta roda noutro fio e pode trocar o catalogo a qualquer momento; sai
// cedo quando nada mudou, entao custa uma comparacao de inteiro.
static int filsAplicadas = -1;
// Assinatura das preferencias que MUDAM a lista de fileiras. Sem isto, desligar
// "Continuar assistindo" em Ajustes so valia depois que a rede trocasse o
// catalogo — a comparacao de `nCat` saia cedo e a fileira continuava na tela.
static int prefsAplicadas = -1;
static int assinaturaPrefs(void) {
  return (ajustes_cw_ligado() ? 1 : 0)
       | (ajustes_cw_estilo() << 1)
       | (ajustes_posteres_deitados() ? 8 : 0)
       | (ajustes_rotulos_poster() ? 16 : 0);
}
static void sincronizarFileiras(void) {
  int nCat = cat_n_fileiras(), r, destino = 0;
  int assin = assinaturaPrefs();
  if (nCat < 1 || (nCat == filsAplicadas && assin == prefsAplicadas)) return;
  for (r = 0; r < nCat && destino < MAX_FIL; r++) {
    const CatFileira *cf = cat_fileira(r);
    if (!cf) break;
    // `continueWatchingEnabled: false` tira a fileira da home inteira — nao a
    // esvazia, tira. E o que renderModernHomeLayout faz quando
    // computeContinueWatchingRenderState devolve a fileira desligada.
    if (!strcmp(cf->chave, "continue_watching") && !ajustes_cw_ligado()) continue;
    fileiras[destino].titulo = cf->titulo;
    // "Continuar assistindo" e a unica landscape: e o
    // `continueWatchingCardStyle: "card"` do perfil. Todo o resto e poster 2:3.
    // `continueWatchingCardStyle`: "card" e "largo" desenham landscape, "poster"
    // usa o mesmo 2:3 das outras fileiras. E a preferencia, nao o tipo da
    // fileira, que decide a forma.
    fileiras[destino].tipo = (!strcmp(cf->chave, "continue_watching")
                              && ajustes_cw_estilo() != 2)
                           ? FILEIRA_CONTINUE : FILEIRA_NORMAL;
    fileiras[destino].n   = cf->n > MAX_CARDS ? MAX_CARDS : cf->n;
    fileiras[destino].ini = cf->ini;
    destino++;
  }
  nFileiras = destino;
  filsAplicadas = nCat;
  prefsAplicadas = assin;
  if (nFileiras < 1) return;
  {
    int cols[MAX_FIL], k;
    for (k = 0; k < nFileiras; k++) cols[k] = fileiras[k].n;
    focus_iniciar(&foco, nFileiras, cols);
  }
  printf("[home] %d fileiras vindas do catalogo\n", nFileiras);
}

void home_atualizar(float dt, Uint32 agora) {
  sincronizarFileiras();

  // O HERO SEGUE O FOCO. Pedido do dono, e e uma DIVERGENCIA DELIBERADA do app
  // web: medi duas vezes com o foco andando de verdade (o card mudou de "54
  // minutos restantes" para "1h 12m restantes") e a arte do
  // `.home-hero-backdrop` continuou a mesma — no web ela nao acompanha a
  // selecao. Fica registrado para ninguem "corrigir" isto de volta achando que
  // e desvio: e melhoria escolhida, nao erro.
  //
  // Enquanto ha foco num card, o carrossel automatico nao roda: duas fontes
  // mexendo na mesma arte dariam trocas em cima da escolha do usuario.
  {
    int alvo = -1;
    if (foco.fileira >= 0 && foco.fileira < nFileiras) {
      int i = fileiras[foco.fileira].ini + foco.coluna;
      if (i >= 0 && i < cat_n()) alvo = i;
    }
    if (alvo >= 0 && alvo != heroAtual) {
      heroAnterior = heroAtual;
      heroAtual = alvo;
      heroFade = 0.0f;                       // recomeca o crossfade
      heroTrocaEm = agora + NV_HERO_INTERVALO_MS;
    } else if (alvo < 0 && agora >= heroTrocaEm) {
      // Sem card em foco (acervo vazio, por exemplo) o carrossel volta a girar.
      heroAnterior = heroAtual;
      heroAtual = (heroAtual + 1) % nAcervoHero();
      heroFade = 0.0f;
      heroTrocaEm = agora + NV_HERO_INTERVALO_MS;
    }
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
      float util = NV_TELA_W - ajustes_conteudo_x() - NV_LEGACY_CONTENT_RIGHT;
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

  // A FILEIRA EM FOCO FICA SEMPRE NO MESMO Y, e as de cima SOMEM.
  //
  // Observado nas duas capturas de referencia do dono: com o foco em "Continuar
  // assistindo" esse titulo aparece na mesma altura em que, ao descer uma
  // fileira, aparece "For You - Filme". A fileira anterior nao sobe — ela deixa
  // de ser desenhada. Palavras dele: "quando desce uma linha as coisas somem e
  // nao sobem".
  //
  // O que estava aqui era uma CAMERA: mantinha a fileira em foco dentro de um
  // viewport e rolava o minimo necessario. Isso desliza tudo para cima, e foi o
  // que fez o texto do hero passar por cima do titulo da fileira.
  //
  // O deslocamento e a soma das fileiras ANTES da que tem foco, entao o topo da
  // focada cai exatamente em NV_SHELF_TOP. Continua com mola: o salto seco
  // entre fileiras de alturas diferentes le como corte, nao como navegacao.
  float alvoY = 0.0f;
  { int r = foco.fileira;
    for (int i = 0; i < r && i < nFileiras; i++)
      alvoY += NV_LEGACY_ROW_HEAD_H + alturaTotalDe(fileiras[i].tipo) + fileiraGap();
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
  // Faixa ou tela cheia, conforme `modernHeroFullScreenBackdropEnabled`. Sao os
  // dois estados da MESMA tela, nao dois layouts — e cada um tem a sua rampa de
  // degrade, medida separadamente (ver GFX_HERO e GFX_HERO_CHEIO em gfx.c).
  int cheio = ajustes_hero_cheio();
  // Em tela cheia o bloco sobe 70px (ver layout.h).
  GfxModo modoHero = cheio ? GFX_HERO_CHEIO : GFX_HERO;
  GfxRect r = cheio ? (GfxRect){ 0, 0, NV_TELA_W, NV_HERO_CHEIO_H }
                    : (GfxRect){ NV_HERO_ARTE_X, 0, NV_HERO_ARTE_W, NV_HERO_ARTE_H };

  const CatItem *ci = cat_item(heroAtual);
  const char *arteA = (ci && ci->backdrop[0]) ? ci->backdrop : bd[heroAtual];
  const CatItem *cAnt = cat_item(heroAnterior);
  const char *arteB = (cAnt && cAnt->backdrop[0]) ? cAnt->backdrop : bd[heroAnterior];

  // Teto de 1920: o hero ocupa a tela e a 960 saia esticado ao dobro.
  GLuint tAnt = tex_obter_hero(arteB);
  GLuint tAtu = tex_obter_hero(arteA);
  if (heroFade < 1.0f && tAnt) {
    gfx_tex_aspect_atual = tex_aspecto(arteB);
    gfx_rect(r, tAnt, modoHero, 0, 0, 0, 0.0f, 0, 0, 0, aArte);
  }
  if (tAtu) {
    gfx_tex_aspect_atual = tex_aspecto(arteA);
    gfx_rect(r, tAtu, modoHero, 0, 0, 0, 0.0f, 0, 0, 0, heroFade * aArte);
  }
  gfx_tex_aspect_atual = 0.0f;

  float aTexto = 1.0f;

  // BLOCO DE TEXTO DO HERO — transcrito do CSS do app web, nao deduzido de
  // captura. `.home-modern-hero-copy` e um flex column com justify-content
  // flex-end e gap 16, ancorado numa base fixa; os filhos, na ordem:
  //   .home-hero-brand         caixa do logo, 440x200, arte no topo-esquerda
  //   .home-modern-hero-meta-line   21/500 #b3b3b3, tokens separados por •
  //   .home-modern-hero-secondary   18/600 branco 88%, com selos e o IMDb
  //   .home-hero-description        22/400 branco, largura 560, leading 30
  // Cada bloco vazio some (`.is-empty { display: none }`), e e por isso que a
  // altura do conjunto muda de titulo para titulo — nao por posicao absoluta.
  //
  // O conteudo de cada linha vem de buildModernHeroPresentation
  // (homeScreen.js:2497), que separa o caso "continuar assistindo" do resto.
  int contHero = (ci && ci->progresso > 0 && ci->restanteMin > 0);

  // Linha de meta. No web sao tokens juntados por "•"; ci->genero ja chega
  // como "Filme · Terror", que e o par (tipo, primeiro genero) do web.
  char metaLinha[288];
  metaLinha[0] = 0;
  if (contHero && ci->temporada > 0) {
    char cab[64];
    snprintf(cab, sizeof cab, "S%d E%d", ci->temporada, ci->episodio);
    snprintf(metaLinha, sizeof metaLinha, "%s%s%s", cab,
             (ci->genero[0] ? "  \xc2\xb7  " : ""), ci->genero);
  } else if (ci && ci->genero[0]) {
    snprintf(metaLinha, sizeof metaLinha, "%s", ci->genero);
  }
  if (ci && ci->meta[0]) {
    size_t n = strlen(metaLinha);
    snprintf(metaLinha + n, sizeof metaLinha - n, "%s%s",
             n ? "   \xe2\x80\xa2   " : "", ci->meta);
  }

  // Linha secundaria: destaque de progresso, selos e a nota do IMDb. O web so
  // mostra o IMDb aqui quando ja existe destaque ou selo (showImdbSecondary);
  // no outro caso ele vai para o fim da linha de meta.
  char destaque[64];
  destaque[0] = 0;
  if (contHero) snprintf(destaque, sizeof destaque, "%d MINUTOS RESTANTES",
                         ci->restanteMin);
  const char *selo = (ci && ci->classificacao[0] && !contHero) ? ci->classificacao : NULL;
  char nota[8];
  nota[0] = 0;
  if (ci && ci->nota > 0) snprintf(nota, sizeof nota, "%.1f", ci->nota / 10.0f);
  int temSec = (destaque[0] || selo || nota[0]);

  const char *sinopse = (ci && ci->sinopse[0]) ? ci->sinopse : "";

  // --- empilhamento de baixo para cima, como o flex-end do CSS ---
  float base = NV_SHELF_TOP - NV_HERO_COPY_GAP;
  float hSin = sinopse[0] ? txt_bloco(TXT_HERO_SIN, sinopse, 255, 255, 255, -1, 0,
                                      NV_HERO_SIN_W, NV_LD_HERO_SIN, 0.0f, 3)
                          : 0.0f;
  float ySin  = base - hSin;
  float ySec  = temSec ? (ySin - (sinopse[0] ? NV_HERO_COPY_LINHA : 0.0f)
                          - NV_LD_HERO_SEC) : ySin;
  float yMeta = ySec - ((temSec || sinopse[0]) ? NV_HERO_COPY_LINHA : 0.0f)
                - (metaLinha[0] ? NV_LD_HERO_META : 0.0f);
  float logoY = yMeta - NV_HERO_COPY_LINHA - NV_LOGO_HERO_H;
  float x = ajustes_conteudo_x();

  // Logo do titulo, ou o nome em texto quando nao ha logo
  // (.home-hero-title-text, 56/600 no modern — nao os 76 do TXT_TITULO1).
  GLuint tlogo = (ci && ci->logo[0]) ? tex_obter(ci->logo) : 0;
  if (tlogo) {
    float ap = tex_aspecto(ci->logo);
    if (ap <= 0.0f) ap = 4.0f;
    float hTit = NV_LOGO_HERO_H, wTit = hTit * ap;
    float maxW = cheio ? NV_LOGO_HERO_CHEIO_MAX_W : NV_LOGO_HERO_MAX_W;
    if (wTit > maxW) { wTit = maxW; hTit = wTit / ap; }
    // object-position: left top — a arte encosta no TOPO da caixa.
    GfxRect rl = { x, logoY, wTit, hTit };
    gfx_tex_aspect_atual = 0.0f;
    gfx_rect(rl, tlogo, GFX_TEXTO, 0, 0, 0, 0.0f, 1, 1, 1, aTexto * heroFade);
  } else {
    // .legacy-webos .home-hero-title-text: 76px (components.css:19164), nao os
    // 56 do tema padrao.
    TxtLinha tit = txt_linha(TXT_TITULO1, (ci && ci->titulo[0]) ? ci->titulo
                             : TITULOS_DEMO[heroAtual % 10], 255, 255, 255, 255);
    txt_desenhar_alpha(tit, x, logoY + NV_LOGO_HERO_H - (float)tit.h,
                       aTexto * heroFade);
  }

  if (metaLinha[0]) {
    TxtLinha lm = txt_linha_corta(TXT_HERO_META, metaLinha, 179, 179, 179, 255,
                                  NV_HERO_SIN_W);
    txt_desenhar_alpha(lm, x, yMeta, aTexto * heroFade);
  }

  if (temSec) {
    float cx = x;
    float a = aTexto * heroFade;
    if (destaque[0]) {
      // .home-modern-hero-highlight: branco cheio, peso 600, tracking 0.04em.
      cx += txt_tracking(TXT_HERO_SEC, destaque, 255, 255, 255, cx, ySec, a,
                         NV_FT_HERO_SEC * 0.04f);
      cx += 14.0f;
    }
    if (selo) {
      // .home-modern-hero-badge: 40 de altura, raio 12, so a BORDA na cor de
      // foco a 55%. Sem helper de contorno, a borda sai de quatro faixas.
      TxtLinha lb = txt_linha(TXT_HERO_SEC, selo, 235, 235, 240, 255);
      float bw = lb.w + 36.0f, bh = 40.0f, by = ySec - (bh - lb.h) * 0.5f;
      float br = 0.35f, bg = 0.60f, bb = 1.00f, ba = 0.55f * a;
      gfx_cor((GfxRect){ cx, by, bw, 2 }, 0, br, bg, bb, ba);
      gfx_cor((GfxRect){ cx, by + bh - 2, bw, 2 }, 0, br, bg, bb, ba);
      gfx_cor((GfxRect){ cx, by, 2, bh }, 0, br, bg, bb, ba);
      gfx_cor((GfxRect){ cx + bw - 2, by, 2, bh }, 0, br, bg, bb, ba);
      txt_desenhar_alpha(lb, cx + 18.0f, ySec, a);
      cx += bw + 14.0f;
    }
    if (nota[0]) {
      // .home-hero-imdb: o selo amarelo de 40px e a nota logo depois, com 10
      // de respiro. O SVG do IMDb nao esta empacotado aqui; o retangulo
      // amarelo com as letras pretas e a mesma leitura a esta escala.
      TxtLinha ls = txt_linha(TXT_MINI, "IMDb", 8, 8, 8, 255);
      float sw = 40.0f, sh = ls.h + 6.0f;
      gfx_cor((GfxRect){ cx, ySec + (NV_LD_HERO_SEC - sh) * 0.5f, sw, sh },
              0.12f, 0.96f, 0.78f, 0.06f, a);
      txt_desenhar_alpha(ls, cx + (sw - ls.w) * 0.5f,
                         ySec + (NV_LD_HERO_SEC - sh) * 0.5f + 3.0f, a);
      cx += sw + 10.0f;
      TxtLinha ln = txt_linha(TXT_HERO_SEC, nota, 179, 179, 179, 255);
      txt_desenhar_alpha(ln, cx, ySec, a);
    }
  }

  if (sinopse[0])
    txt_bloco(TXT_HERO_SIN, sinopse, 255, 255, 255, x, ySin, NV_HERO_SIN_W,
              NV_LD_HERO_SIN, aTexto * heroFade, 3);
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
  if (ajustes_hero_ligado()) desenhaHero(agora);

  // ABERTURA DO DETALHE: as fileiras DESCEM e apagam; a arte de fundo fica.
  //
  // E o movimento que o dono descreveu — "so os posters descem e mantem o
  // background". O detalhe ja nao voa mais a partir do card: a arte dele entra
  // em tela cheia ganhando opacidade, entao o que o olho segue e a saida das
  // fileiras. Descer 8% da altura da tela e o bastante para ler como saida sem
  // que a ultima fileira suma antes da hora.
  //
  // O `pd` vem da MESMA mola que o detalhe usa para desenhar (detail_progresso),
  // e nao de um relogio proprio: dois relogios descasariam e a home sairia
  // adiantada ou atrasada em relacao a arte que entra.
  float pd = detail_progresso();
  float descida = pd * NV_TELA_H * 0.08f;
  if (pd >= 0.996f) return;   // detalhe assentado: nada da home aparece

  // VIEWPORT DAS FILEIRAS. `.home-modern-rows-viewport` (components.css:6929) e
  // um bloco absoluto com bottom:0, height 52% e overflow-y:auto — ou seja as
  // fileiras rolam DENTRO dos 52% de baixo e o que sobe alem disso e CLIPADO.
  // O port desenhava as fileiras soltas sobre a tela inteira, e por isso a
  // fileira que saia por cima aparecia atravessada no bloco do hero em vez de
  // sumir. O hero nao rola: so o conteudo dele muda com o foco.
  gfx_recorte(0, NV_SHELF_TOP, NV_TELA_W, NV_TELA_H - NV_SHELF_TOP);
  float y = NV_SHELF_TOP - scrollY + descida;
  for (int r = 0; r < nFileiras; r++) {
    TipoFileira tipo = fileiras[r].tipo;
    float lw = larguraDe(tipo), lh = alturaDe(tipo), passo = passoDe(tipo);
    float artH = lh;
    // `y` é o topo do cabeçalho da fileira; os cards começam depois do título.
    // Separar os dois evita que o título da fileira seguinte seja desenhado
    // sobre a arte da anterior quando a fileira tem cards altos.
    float cardY = y + NV_LEGACY_ROW_HEAD_H;

    int deitado = (tipo != FILEIRA_CONTINUE) && ajustes_posteres_deitados();
    int rotuloFora = temRotulo(tipo);
    if (y < NV_TELA_H + 200 && y + NV_LEGACY_ROW_HEAD_H + lh > -200) {
      // `catalogTypeSuffixEnabled`. formatCatalogRowTitle (homeUtils.js:62) faz
      // `if (!showTypeSuffix) return base;` — devolve o nome capitalizado e
      // pronto. Aqui o sufixo e tirado no DESENHO e nao na descoberta, senao a
      // preferencia so valeria depois que a rede trouxesse os catalogos de
      // novo — ou seja, so no proximo arranque.
      const char *rotFil = fileiras[r].titulo;
      char semSufixo[96];
      if (!ajustes_sufixo_tipo() && rotFil) {
        const char *corte = strstr(rotFil, " - ");
        const char *ultimo = NULL;
        while (corte) { ultimo = corte; corte = strstr(corte + 3, " - "); }
        if (ultimo && (!strcmp(ultimo + 3, "Filme")
                       || !strcmp(ultimo + 3, "S\xc3\xa9rie"))) {
          size_t n = (size_t)(ultimo - rotFil);
          if (n >= sizeof semSufixo) n = sizeof semSufixo - 1;
          memcpy(semSufixo, rotFil, n);
          semSufixo[n] = 0;
          rotFil = semSufixo;
        }
      }
      TxtLinha tl = txt_linha(TXT_ROW_TITULO, rotFil, 255, 255, 255, 255);
      txt_desenhar(tl, ajustes_conteudo_x(), y);

      for (int passe = 1; passe < 2; passe++) {
        for (int c = 0; c < fileiras[r].n; c++) {
          float f = animFoco[r][c];
          if (passe == 0 && f < 0.01f) continue;
          float esc = 1.0f + escalaDe(tipo) * f;
          float w = lw * esc, h = artH * esc;
          float cx = ajustes_conteudo_x() + c * passo - scrollX[r] + lw * 0.5f;
          // Sem levantamento: no web o card focado nao sai do lugar.
          float cy = cardY + artH * 0.5f;
          if (cx < -lw * 1.5f || cx > NV_TELA_W + lw) continue;
          float px = cx - w * 0.5f, py = cy - h * 0.5f;

          if (passe == 0) {
            // Sem sombra. Ela existia para separar o card do fundo, mas sobre
            // arte colorida vira um halo escuro em volta do item focado — e o
            // aparelho nao tem isso: la o foco se marca por escala e brilho.
            (void)f;
            continue;
          }

          const int idxCat = fileiras[r].ini + c;
          const CatItem *cItem = cat_item(idxCat);
          const char *caminho;
          // Card DEITADO pede arte deitada. No web o poster do card landscape sai
          // de `landscapePoster` -> `background` -> `backdrop` -> `poster`
          // (homeScreen.js:3155), nao do poster 2:3 — usar o retrato aqui faria o
          // shader recortar a cabeca de todo mundo para caber em 16:9.
          if (tipo == FILEIRA_CONTINUE || deitado)
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
          // Anel de foco. MEDIDO no app web: `box-shadow` de 2px em `#f5f5f5`,
          // por dentro e por fora da arte — cinza quase branco, nao o azul de
          // 3px que estava aqui. O azul nao sai de lugar nenhum da interface:
          // e a unica cor saturada da home e puxa o olho para a moldura em vez
          // do cartaz. Com a escala do foco removida, este anel passou a ser o
          // UNICO sinal de foco, e por isso ele tem de ser o do original.
          float raio = raioDe(w, h);
          if (f > 0.01f) {
            GfxRect borda = { px - 2.0f, py - 2.0f, w + 4.0f, h + 4.0f };
            gfx_cor(borda, raio, 0.961f, 0.961f, 0.961f, f);
          }
          GfxRect card = { px, py, w, h };
          if (t) {
            float fase = agora / 1000.0f + (r * 3 + c) * 0.6f;
            gfx_tex_aspect_atual = tex_aspecto(caminho);
            gfx_rect(card, t, GFX_CARD, f,
                     sinf(fase) * 0.010f * f, cosf(fase * 0.8f) * 0.006f * f,
                     raio, 0, 0, 0, 1);
            gfx_tex_aspect_atual = 0.0f;
          } else {
            gfx_cor(card, raio, 0.14f, 0.14f, 0.16f, 1.0f);
          }
          // `cardDepthEnabled` mais o interruptor por secao: `cardDepthPosters`
          // nas fileiras de catalogo, `cardDepthContinueWatching` na primeira.
          desenhaProfundidade(card, raio,
                              tipo == FILEIRA_CONTINUE ? ajustes_profundidade_cw()
                                                       : ajustes_profundidade_posters());

          // --- posterLabelsEnabled ---------------------------------------
          // Card DEITADO: a legenda vai DENTRO da moldura, sobre um degrade que
          // cobre 54% da altura, com 14 de recuo lateral e 12 da base
          // (.home-poster-landscape-copy). Card EM PE: vai ABAIXO do poster, num
          // bloco de 74 de altura com 8 de padding no topo (.home-poster-copy).
          if (tipo != FILEIRA_CONTINUE && ajustes_rotulos_poster() && cItem) {
            const char *nome = cItem->titulo[0] ? cItem->titulo : NULL;
            const char *sub  = cItem->genero[0] ? cItem->genero : NULL;
            if (deitado && nome) {
              GfxRect veu = { px, py + h * (1.0f - NV_LAND_VEU), w, h * NV_LAND_VEU };
              gfx_rect(veu, 0, GFX_VEU, 0, 0, 0, raio, 0, 0, 0, 0.80f);
              float maxW = w * NV_LAND_COPY_MAXW;
              float bx = px + NV_LAND_COPY_PAD;
              TxtLinha tn = txt_linha_corta(TXT_CAPTION, nome, 245, 246, 250, 255, maxW);
              if (sub) {
                TxtLinha ts = txt_linha_corta(TXT_MINI, sub, 200, 202, 210, 255, maxW);
                txt_desenhar_alpha(ts, bx, py + h - NV_LAND_COPY_BASE - ts.h, 0.85f);
                txt_desenhar_alpha(tn, bx,
                                   py + h - NV_LAND_COPY_BASE - ts.h - 4.0f - tn.h, 0.98f);
              } else {
                txt_desenhar_alpha(tn, bx, py + h - NV_LAND_COPY_BASE - tn.h, 0.98f);
              }
            } else if (rotuloFora && nome) {
              float bx = px + NV_POSTER_COPY_PADX;
              float by = py + h + NV_POSTER_COPY_PADT;
              float maxW = w - NV_POSTER_COPY_PADX * 2.0f;
              // 16/500 e 13/400 rgba(255,255,255,.7) — os corpos de
              // .home-poster-title e .home-poster-subtitle.
              TxtLinha tn = txt_linha_corta(TXT_CAPTION2, nome, 245, 246, 250, 255, maxW);
              txt_desenhar_alpha(tn, bx, by, 0.98f);
              if (sub) {
                TxtLinha ts = txt_linha_corta(TXT_MINI, sub, 255, 255, 255, 255, maxW);
                txt_desenhar_alpha(ts, bx, by + tn.h + 2.0f, 0.70f);
              }
            }
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
    y += NV_LEGACY_ROW_HEAD_H + alturaTotalDe(tipo) + fileiraGap();
  }
  gfx_sem_recorte();
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
