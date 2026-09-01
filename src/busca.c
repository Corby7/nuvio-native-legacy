// Busca: teclado na tela a esquerda, resultados em grade a direita.
//
// DECISAO DE PROJETO — teclado em GRADE, nao a linha unica do tvOS.
// O tvOS de verdade usa uma faixa horizontal rolavel com o alfabeto inteiro. Ela
// e bonita e cabe em pouca altura, mas custa caro no D-pad: sao 38 teclas em
// UMA dimensao, entao a distancia media entre duas letras e ~13 toques e o pior
// caso ("a" -> "9") passa de 37. Digitar "fundacao" nela da mais de cem toques.
// A grade 6x7 usada aqui (a mesma forma que Netflix e YouTube adotaram na TV
// justamente por isso) coloca a mesma tecla a no maximo 5+6 toques e ~5 em
// media: a mesma palavra sai em menos de um terco dos toques. Como a busca so
// existe para quem ja sabe o que quer digitar, o numero de toques por letra E a
// experiencia — e o unico ponto em que copiar o aparelho seria copiar um
// defeito conhecido dele. O resto da tela (campo acima, grade de posteres
// filtrando a cada letra, tratamento de foco) segue o aparelho.
#include "busca.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "focus.h"
#include "anim.h"
#include "layout.h"
#include "catalogo.h"
#include <string.h>
#include <stdio.h>

// --- Geometria (tokens locais: layout.h e de outro dono) ---------------------
// Tecla de 74px porque e o menor alvo que ainda deixa a letra em TXT_TITULO3
// respirar; abaixo disso a grade lida a distancia de sofa vira uma mancha.
#define BUSCA_TECLA_W    74.0f
#define BUSCA_TECLA_GAP  12.0f
#define BUSCA_KB_COLS     6
#define BUSCA_KB_FILEIRAS 7            // 6 fileiras de A-Z/0-9 + 1 de espaco/apagar
#define BUSCA_KB_X       NV_LEGACY_CONTENT_X
#define BUSCA_KB_PASSO   (BUSCA_TECLA_W + BUSCA_TECLA_GAP)
#define BUSCA_KB_W       (BUSCA_KB_COLS * BUSCA_TECLA_W + (BUSCA_KB_COLS - 1) * BUSCA_TECLA_GAP)
#define BUSCA_CAMPO_Y   132.0f
#define BUSCA_CAMPO_H    78.0f
#define BUSCA_KB_Y      (BUSCA_CAMPO_Y + BUSCA_CAMPO_H + 38.0f)
// A ultima fileira tem 2 teclas largas: cada uma cobre 3 colunas da grade. Elas
// sao as unicas com rotulo em palavra, e uma tecla de 74px nao comporta
// "apagar" em corpo legivel.
#define BUSCA_TECLA_LARGA (3 * BUSCA_TECLA_W + 2 * BUSCA_TECLA_GAP)

#define BUSCA_RES_COLS    4
#define BUSCA_RES_PASSO_X (NV_POSTER_W + NV_CARD_GAP)
#define BUSCA_RES_W       (BUSCA_RES_COLS * NV_POSTER_W + (BUSCA_RES_COLS - 1) * NV_CARD_GAP)
#define BUSCA_RES_X       (NV_TELA_W - NV_LEGACY_CONTENT_RIGHT - BUSCA_RES_W)
#define BUSCA_RES_Y       BUSCA_CAMPO_Y
#define BUSCA_RES_ROTULO  26.0f
#define BUSCA_RES_PASSO_Y (NV_POSTER_H + BUSCA_RES_ROTULO + 24.0f)
#define BUSCA_RES_AREA_H  (NV_TELA_H - NV_MARGEM_Y - BUSCA_RES_Y)
// Crescimento da tecla em foco. Menor que o do poster de proposito: a tecla e
// pequena e vizinha imediata das outras, e com 14% ela invade o gap de 12px e
// encosta na tecla ao lado — o foco passa a parecer bagunca de layout.
#define BUSCA_TECLA_ESCALA 0.10f
#define BUSCA_MAX_CONSULTA 48

// --- Estado ------------------------------------------------------------------
static Foco  focoKb;
static int   painel = 0;            // 0 = teclado, 1 = resultados
static int   resSel = 0;            // posicao dentro da lista FILTRADA
static char  consulta[BUSCA_MAX_CONSULTA];
static int   nConsulta = 0;
static int   res[CAT_MAX];          // indices de catalogo que passaram no filtro
static int   nRes = 0;
static int   sair = 0;
static int   pedido = -1;           // indice de catalogo escolhido, -1 = nenhum
static float animTecla[BUSCA_KB_FILEIRAS][BUSCA_KB_COLS];
static float animRes[CAT_MAX];
static float scrollY = 0.0f, scrollAlvo = 0.0f;
static HomeItem itemFoco;
static int   temItemFoco = 0;

// As duas ultimas teclas nao sao letras, entao a fileira 6 tem 2 colunas e nao
// 6. O Foco ja trata fileiras de larguras diferentes (e lembra a coluna ao
// voltar), que e o motivo de usar o modulo em vez de dois inteiros soltos.
static const int KB_COLUNAS[BUSCA_KB_FILEIRAS] = { 6, 6, 6, 6, 6, 6, 2 };
// Minusculas como no aparelho: o campo mostra o que foi digitado, e uma consulta
// em caixa alta le como grito. A comparacao ignora caixa de qualquer forma.
static const char *TECLAS =
  "abcdefghijklmnopqrstuvwxyz0123456789";   // 36 = 6 fileiras x 6 colunas

// --- Normalizacao ------------------------------------------------------------
// Dobra uma letra latina acentuada (segundo byte de uma sequencia UTF-8 iniciada
// por 0xC3) na letra ASCII correspondente. Sem isto, buscar "fundacao" nao acha
// "Fundacao" — o caso de uso mais obvio da tela, ja que ninguem digita cedilha
// num teclado de D-pad. Escrito a mao porque a alternativa (iconv/ICU) traria
// uma dependencia inteira para resolver 30 codepoints, e a TV nao tem locale
// configurado de forma confiavel.
static char dobraLatina(unsigned char segundo) {
  // A sequencia 0xC3 0xNN cobre U+00C0..U+00FF: o codepoint e o segundo byte
  // mais 0x40.
  unsigned cp = (unsigned)segundo + 0x40u;
  // caixa alta -> baixa dentro do bloco (0xD7 e o sinal de multiplicacao, nao
  // uma letra, e por isso fica de fora)
  if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 0x20;
  if (cp >= 0xE0 && cp <= 0xE6) return 'a';   // a com todos os acentos, e ae
  if (cp == 0xE7)               return 'c';   // c cedilha
  if (cp >= 0xE8 && cp <= 0xEB) return 'e';
  if (cp >= 0xEC && cp <= 0xEF) return 'i';
  if (cp == 0xF0)               return 'd';
  if (cp == 0xF1)               return 'n';
  if ((cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8) return 'o';
  if (cp >= 0xF9 && cp <= 0xFC) return 'u';
  if (cp == 0xFD || cp == 0xFF) return 'y';
  return ' ';
}

// Reduz a string a ASCII minusculo sem acento. Tudo que nao e letra latina vira
// espaco — inclusive o "·" que separa os campos do catalogo — para nunca colar
// duas palavras que estavam separadas por um simbolo.
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
      // outro multibyte: descarta as continuacoes junto, senao os bytes soltos
      // entrariam na string normalizada e poderiam casar por acaso
      while ((*p & 0xC0) == 0x80) p++;
      saida = ' ';
    }
    destino[k++] = saida;
  }
  destino[k] = 0;
}

// --- Filtro ------------------------------------------------------------------
// So roda quando o texto muda. Percorrer o catalogo por quadro seria barato hoje
// (12 titulos), mas o filtro tambem reordena o foco: recalcular durante o
// desenho faria o item sob o foco trocar no meio de um quadro.
static void refiltrar(void) {
  char alvo[BUSCA_MAX_CONSULTA * 2];
  normalizar(consulta, alvo, sizeof alvo);
  int antes = (resSel >= 0 && resSel < nRes) ? res[resSel] : -1;

  nRes = 0;
  int n = cat_n();
  for (int i = 0; i < n && nRes < CAT_MAX; i++) {
    const CatItem *c = cat_item(i);
    if (!c) continue;
    if (!alvo[0]) { res[nRes++] = i; continue; }   // sem texto: o acervo inteiro
    char titulo[320];
    normalizar(c->titulo, titulo, sizeof titulo);
    if (strstr(titulo, alvo)) res[nRes++] = i;
  }

  // Mantem o foco no MESMO titulo quando ele sobrevive ao novo filtro: apagar
  // uma letra nao deveria jogar o foco para o primeiro poster.
  resSel = 0;
  for (int i = 0; i < nRes; i++) if (res[i] == antes) { resSel = i; break; }
  if (nRes == 0) painel = 0;
}

// --- Teclas ------------------------------------------------------------------
static void aplicarTecla(void) {
  if (focoKb.fileira < BUSCA_KB_FILEIRAS - 1) {
    int k = focoKb.fileira * BUSCA_KB_COLS + focoKb.coluna;
    if (nConsulta + 1 < BUSCA_MAX_CONSULTA) consulta[nConsulta++] = TECLAS[k];
  } else if (focoKb.coluna == 0) {
    // espaco no comeco nao entra: ele nao muda o filtro e so acumula lixo no campo
    if (nConsulta > 0 && nConsulta + 1 < BUSCA_MAX_CONSULTA) consulta[nConsulta++] = ' ';
  } else {
    if (nConsulta > 0) nConsulta--;
  }
  consulta[nConsulta] = 0;
  refiltrar();
}

static GfxRect retanguloTecla(int fileira, int coluna) {
  GfxRect r;
  r.y = BUSCA_KB_Y + fileira * (BUSCA_TECLA_W + BUSCA_TECLA_GAP);
  r.h = BUSCA_TECLA_W;
  if (fileira < BUSCA_KB_FILEIRAS - 1) {
    r.x = BUSCA_KB_X + coluna * BUSCA_KB_PASSO;
    r.w = BUSCA_TECLA_W;
  } else {
    r.x = BUSCA_KB_X + coluna * (BUSCA_TECLA_LARGA + BUSCA_TECLA_GAP);
    r.w = BUSCA_TECLA_LARGA;
  }
  return r;
}

// --- Ciclo de vida -----------------------------------------------------------
int busca_iniciar(void) {
  focus_iniciar(&focoKb, BUSCA_KB_FILEIRAS, KB_COLUNAS);
  painel = 0; sair = 0; pedido = -1;
  nConsulta = 0; consulta[0] = 0;
  scrollY = scrollAlvo = 0.0f;
  temItemFoco = 0;
  memset(animTecla, 0, sizeof animTecla);
  memset(animRes, 0, sizeof animRes);
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

  if (k == SDLK_AC_BACK) {
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
          if (nRes > 0) painel = 1;
        } else focus_mover(&focoKb, 1, 0);
        break;
      case SDLK_UP:     focus_mover(&focoKb, 0, -1); break;
      case SDLK_DOWN:   focus_mover(&focoKb, 0,  1); break;
      case SDLK_RETURN: aplicarTecla(); break;
      default: break;
    }
    return;
  }

  switch (k) {
    case SDLK_LEFT:
      // Voltar da primeira coluna dos resultados devolve o foco ao teclado.
      if (resSel % BUSCA_RES_COLS == 0) painel = 0;
      else resSel--;
      break;
    case SDLK_RIGHT:
      if ((resSel + 1) % BUSCA_RES_COLS != 0 && resSel + 1 < nRes) resSel++;
      break;
    case SDLK_UP:
      if (resSel >= BUSCA_RES_COLS) resSel -= BUSCA_RES_COLS;
      break;
    case SDLK_DOWN:
      // A ultima fileira costuma estar incompleta; descer para uma coluna que
      // nao existe la travaria o movimento, entao o foco encosta no ultimo item.
      if (resSel / BUSCA_RES_COLS < (nRes - 1) / BUSCA_RES_COLS) {
        resSel += BUSCA_RES_COLS;
        if (resSel >= nRes) resSel = nRes - 1;
      }
      break;
    case SDLK_RETURN:
      if (resSel >= 0 && resSel < nRes) pedido = res[resSel];
      break;
    default: break;
  }
}

void busca_atualizar(float dt, Uint32 agora) {
  (void)agora;
  for (int f = 0; f < BUSCA_KB_FILEIRAS; f++)
    for (int c = 0; c < KB_COLUNAS[f]; c++) {
      float alvo = (painel == 0 && focus_indice(&focoKb, f, c)) ? 1.0f : 0.0f;
      animTecla[f][c] = anim_mola(animTecla[f][c], alvo, dt,
                                  alvo > animTecla[f][c] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
    }
  for (int i = 0; i < CAT_MAX; i++) {
    float alvo = (painel == 1 && i == resSel) ? 1.0f : 0.0f;
    animRes[i] = anim_mola(animRes[i], alvo, dt,
                           alvo > animRes[i] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
  }

  // Rola so o necessario para a fileira em foco caber inteira na area util —
  // rolagem proporcional ao indice esconderia a primeira fileira antes do
  // usuario ter chegado nela.
  if (painel == 1 && nRes > 0) {
    float topo = (resSel / BUSCA_RES_COLS) * BUSCA_RES_PASSO_Y;
    float base = topo + NV_POSTER_H + BUSCA_RES_ROTULO;
    if (topo - scrollAlvo < 0.0f)                    scrollAlvo = topo;
    if (base - scrollAlvo > BUSCA_RES_AREA_H)        scrollAlvo = base - BUSCA_RES_AREA_H;
  } else if (nRes == 0) {
    scrollAlvo = 0.0f;
  }
  scrollY = anim_mola(scrollY, scrollAlvo, dt, NV_MOLA_SCROLL);
}

// --- Desenho -----------------------------------------------------------------
static void desenhaCampo(Uint32 agora) {
  GfxRect campo = { BUSCA_KB_X, BUSCA_CAMPO_Y, BUSCA_KB_W, BUSCA_CAMPO_H };
  gfx_cor(campo, NV_RAIO_PILL, 1.0f, 1.0f, 1.0f, 0.10f);
  float tx = campo.x + 30.0f;
  if (nConsulta) {
    TxtLinha l = txt_linha(TXT_TITULO3, consulta, 245, 246, 250, 255);
    txt_desenhar(l, tx, campo.y + (campo.h - l.h) * 0.5f);
    tx += l.w + 6.0f;
  } else {
    TxtLinha l = txt_linha(TXT_TITULO3, "Buscar", 255, 255, 255, 255);
    txt_desenhar_alpha(l, tx, campo.y + (campo.h - l.h) * 0.5f, 0.40f);
  }
  // O cursor piscando e o unico sinal de que o campo esta ativo: sem teclado
  // fisico, nada mais indica que as teclas vao parar ali.
  if ((agora / 500) % 2 == 0) {
    GfxRect cur = { tx, campo.y + 18.0f, 3.0f, campo.h - 36.0f };
    gfx_cor(cur, 0.0f, 1.0f, 1.0f, 1.0f, 0.85f);
  }
}

static void desenhaTeclado(void) {
  char rotulo[8];
  for (int f = 0; f < BUSCA_KB_FILEIRAS; f++) {
    for (int c = 0; c < KB_COLUNAS[f]; c++) {
      float k = animTecla[f][c];
      GfxRect base = retanguloTecla(f, c);
      float esc = 1.0f + BUSCA_TECLA_ESCALA * k;
      GfxRect t = { base.x - base.w * (esc - 1.0f) * 0.5f,
                    base.y - base.h * (esc - 1.0f) * 0.5f - NV_FOCO_LIFT * k,
                    base.w * esc, base.h * esc };
      // Sem sombra, como no resto do app: sobre o cinza da tela ela vira halo.
      // A tecla focada INVERTE (fundo claro, glifo escuro) em vez de so
      // acender — a distancia de sofa, a inversao e o unico contraste que se
      // enxerga de relance numa grade de 38 alvos iguais.
      gfx_cor(t, NV_RAIO_CARD, 1.0f, 1.0f, 1.0f, anim_mistura(0.09f, 1.0f, k));
      const char *s;
      if (f < BUSCA_KB_FILEIRAS - 1) {
        rotulo[0] = TECLAS[f * BUSCA_KB_COLS + c]; rotulo[1] = 0;
        s = rotulo;
      } else s = (c == 0) ? "espa\xc3\xa7o" : "apagar";
      int tom = (int)anim_mistura(236.0f, 26.0f, k);
      TxtEstilo est = (f < BUSCA_KB_FILEIRAS - 1) ? TXT_TITULO3 : TXT_HEADLINE;
      TxtLinha l = txt_linha(est, s, tom, tom, tom, 255);
      txt_desenhar(l, t.x + (t.w - l.w) * 0.5f, t.y + (t.h - l.h) * 0.5f);
    }
  }
}

static void desenhaResultados(Uint32 agora) {
  (void)agora;
  temItemFoco = 0;
  if (nRes == 0) {
    TxtLinha l = txt_linha(TXT_HEADLINE, "Nenhum resultado", 236, 237, 242, 255);
    txt_desenhar_alpha(l, BUSCA_RES_X, BUSCA_RES_Y + 8.0f, 0.75f);
    return;
  }

  // A grade rola: sem recorte, a fileira que sai por cima seria desenhada em
  // cima do campo de texto e a de baixo passaria da safe area.
  gfx_recorte(BUSCA_RES_X - NV_POSTER_W * 0.5f, BUSCA_RES_Y - 30.0f,
              BUSCA_RES_W + NV_POSTER_W, BUSCA_RES_AREA_H + 30.0f);
  // Dois passes so pelo item em foco: ele cresce e precisa ficar POR CIMA dos
  // vizinhos, senao a borda do poster ao lado corta o que cresceu.
  for (int passe = 0; passe < 2; passe++) {
    for (int i = 0; i < nRes; i++) {
      float f = animRes[i];
      if ((passe == 1) != (f > 0.01f)) continue;
      const CatItem *ci = cat_item(res[i]);
      if (!ci) continue;

      float cx = BUSCA_RES_X + (i % BUSCA_RES_COLS) * BUSCA_RES_PASSO_X + NV_POSTER_W * 0.5f;
      float cy = BUSCA_RES_Y + (i / BUSCA_RES_COLS) * BUSCA_RES_PASSO_Y - scrollY
               + NV_POSTER_H * 0.5f - NV_FOCO_LIFT * f;
      if (cy < -NV_POSTER_H || cy > NV_TELA_H + NV_POSTER_H) continue;

      float esc = 1.0f + NV_FOCO_ESCALA_P * f;
      float w = NV_POSTER_W * esc, h = NV_POSTER_H * esc;
      GfxRect r = { cx - w * 0.5f, cy - h * 0.5f, w, h };

      GLuint tex = ci->poster[0] ? tex_obter(ci->poster) : 0;
      if (tex) {
        // Sem o aspecto a arte 2:3 estica; e o poster e justamente onde isso
        // salta aos olhos, porque todos ficam lado a lado.
        gfx_tex_aspect_atual = tex_aspecto(ci->poster);
        gfx_rect(r, tex, GFX_CARD, f, 0.0f, 0.0f, NV_RAIO_CARD, 0, 0, 0, 1);
        gfx_tex_aspect_atual = 0.0f;
      } else {
        gfx_cor(r, NV_RAIO_CARD, 0.14f, 0.14f, 0.16f, 1.0f);
      }

      float tomA = anim_mistura(0.62f, 1.0f, f);
      txt_bloco(TXT_CAPTION2, ci->titulo, 236, 237, 242,
                r.x, r.y + h + 8.0f, w, NV_LD_CAPTION2, tomA, 1);

      if (painel == 1 && i == resSel) {
        itemFoco.rect   = r;
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
  GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
  gfx_cor(tela, 0.0f, NV_COR_FUNDO_R, NV_COR_FUNDO_G, NV_COR_FUNDO_B, 1.0f);
  desenhaCampo(agora);
  desenhaTeclado();
  desenhaResultados(agora);
}
