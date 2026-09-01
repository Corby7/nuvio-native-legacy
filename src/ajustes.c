// Ajustes: lista vertical em secoes, rotulo a esquerda e valor a direita.
//
// A regra que organiza a tela inteira: existem duas naturezas de linha e elas
// TEM que parecer diferentes. Linha que muda ganha pilula clara com texto
// escuro e as setas ao redor do valor; linha so de leitura ganha um realce
// apagado, sem setas. Com o mesmo desenho nas duas, o usuario aperta esquerda e
// direita em cima da versao do app esperando que algo aconteca — foi por isso
// que a distincao virou requisito, e nao enfeite.
#include "ajustes.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "anim.h"
#include "layout.h"
#include <stdio.h>
#include <string.h>

// Versao do app: mesma string do appinfo.json empacotado. Fica aqui porque a
// tela nao tem como ler o manifesto em tempo de execucao no aparelho.
#define AJ_VERSAO       "1.0.1"

#define AJ_LINHA_H       88.0f
#define AJ_LINHA_GAP      8.0f
#define AJ_SEC_GAP       46.0f    // fim de uma secao ao cabecalho da proxima
#define AJ_SEC_CABEC     44.0f    // altura reservada ao cabecalho da secao
// Nao e constante: acompanha a rail, como todo o resto do conteudo. Com a
// barra recolhida a lista tambem comeca em 104 — deixar 248 cravado aqui fazia
// a tela de Ajustes ser a unica desalinhada das outras.
#define AJ_LISTA_X      ajustes_conteudo_x()
#define AJ_LISTA_W     1120.0f
#define AJ_PAD           34.0f    // borda da linha ao texto
#define AJ_TOPO        (NV_MARGEM_Y + 118.0f)   // abaixo do titulo da tela
#define AJ_BASE        (NV_TELA_H - NV_MARGEM_Y)
// Raio da linha em fracao do menor lado (o SDF do shader e normalizado):
// 12px sobre 88 de altura, que e o canto que o tvOS usa nas linhas de ajuste.
#define AJ_RAIO           0.14f

typedef enum { AJ_QUALIDADE, AJ_DV, AJ_ATMOS,
               // Layout, com os MESMOS nomes e efeitos do app web. Nao sao
               // invencao desta tela: sao chaves de layoutPreferences.js, e o
               // dono ja mexe nelas na tela de Ajustes do web. Um port que le a
               // preferencia mas nao deixa mudar deixa metade do trabalho.
               AJ_RAIL, AJ_HERO, AJ_HERO_CHEIO, AJ_CW_ESTILO, AJ_ROTULOS,
               AJ_LANDSCAPE,
               AJ_IDIOMA, AJ_ANIM,
               AJ_VERSAO_I, AJ_ESPACO, AJ_N } OpcaoId;

static const char *V_QUALIDADE[] = { "Automática", "4K", "1080p", "720p" };
static const char *V_LIGA[]      = { "Ligado", "Desligado" };
static const char *V_IDIOMA[]    = { "Português", "English" };
static const char *V_ANIM[]      = { "Completas", "Reduzidas" };
// `collapseSidebar`: recolhida = a rail some e o conteudo comeca em 104.
static const char *V_RAIL[]      = { "Recolhida", "Fixa" };
// `continueWatchingCardStyle`, validado em layoutPreferences.js contra
// exatamente estes tres valores.
static const char *V_CW[]        = { "Card", "Largo", "P\xc3\xb4ster" };

typedef struct {
  const char  *rotulo;
  const char **valores;   // NULL = so leitura, o valor vem de textoLeitura()
  int          n;
} Opcao;

static const Opcao OPCOES[AJ_N] = {
  { "Qualidade máxima",        V_QUALIDADE, 4 },
  { "Dolby Vision",            V_LIGA,      2 },
  { "Dolby Atmos",             V_LIGA,      2 },
  { "Barra lateral",           V_RAIL,      2 },   // collapseSidebar
  { "Destaque na home",        V_LIGA,      2 },   // heroSectionEnabled
  { "Destaque em tela cheia",  V_LIGA,      2 },   // modernHeroFullScreenBackdropEnabled
  { "Estilo do \"Continuar assistindo\"", V_CW, 3 }, // continueWatchingCardStyle
  { "Rótulos nos pôsteres",    V_LIGA,      2 },   // posterLabelsEnabled
  { "Pôsteres deitados",       V_LIGA,      2 },   // modernLandscapePostersEnabled
  { "Idioma",                  V_IDIOMA,    2 },
  { "Animações",               V_ANIM,      2 },
  { "Versão",                  NULL,        0 },
  { "Espaço usado por imagens",NULL,        0 },
};

// Nome de cada opcao no arquivo. O formato era POSICIONAL — uma linha por
// opcao, na ordem do enum — e por isso acrescentar uma opcao no meio fazia o
// arquivo de quem ja tinha o app aplicar os valores errados, em silencio. Com
// chave por linha, opcao nova nasce no padrao e as antigas continuam onde
// estavam. Os nomes seguem os do app web onde existe correspondente.
static const char *CHAVE[AJ_N] = {
  "qualidade", "dolbyVision", "dolbyAtmos",
  "collapseSidebar", "heroSectionEnabled", "modernHeroFullScreenBackdropEnabled",
  "continueWatchingCardStyle", "posterLabelsEnabled", "modernLandscapePostersEnabled",
  "idioma", "animacoes", "-versao", "-espaco",
};

// Onde cada secao comeca e quantas opcoes ela tem. Secao e um agrupamento
// visual, nao um nivel de navegacao: cima/baixo atravessa os cabecalhos sem
// parar neles, como no aparelho.
static const struct { const char *titulo; int ini, n; } SECOES[] = {
  { "Reprodução", AJ_QUALIDADE,  3 },
  { "Layout",     AJ_RAIL,       6 },
  { "Interface",  AJ_IDIOMA,     2 },
  { "Sobre",      AJ_VERSAO_I,   2 },
};
#define AJ_N_SECOES (int)(sizeof SECOES / sizeof *SECOES)

// Valor escolhido de cada opcao; os indices de leitura ficam em 0 e nao sao
// usados. Os padroes sao os do aparelho recem-configurado: qualidade
// automatica, tudo ligado, animacoes completas.
// Padroes. Os de layout sao os DEFAULTS de layoutPreferences.js, com uma
// excecao anotada: `collapseSidebar` nasce recolhida e o destaque nasce em tela
// cheia porque e o estado do perfil do dono, que e o que ele ve hoje. As duas
// sao trocaveis aqui, que era o ponto.
static int valor[AJ_N] = {
  0, 0, 0,          /* qualidade, DV, Atmos */
  0,                /* barra lateral: recolhida */
  0,                /* destaque na home: ligado */
  0,                /* destaque em tela cheia: ligado */
  0,                /* continuar assistindo: card */
  0,                /* rotulos nos posteres: ligado */
  1,                /* posteres deitados: desligado (DEFAULT do web) */
  0, 0, 0, 0,
};
static int focoOp = 0;
// Uma lista de UMA coluna nao precisa do focus.h: a memoria de coluna que ele
// existe para resolver nao tem o que lembrar aqui, e o indice cru deixa o
// "pula o cabecalho da secao" ser uma soma em vez de um mapa de fileiras.
static float animFoco[AJ_N];
static float scrollY = 0.0f;
static int sair = 0;

int ajustes_animacoes_reduzidas(void) { return valor[AJ_ANIM]   == 1; }
int ajustes_dolby_vision(void)        { return valor[AJ_DV]      == 0; }
int ajustes_dolby_atmos(void)         { return valor[AJ_ATMOS]   == 0; }
int ajustes_idioma_ingles(void)       { return valor[AJ_IDIOMA]  == 1; }
int ajustes_rail_recolhida(void)      { return valor[AJ_RAIL]    == 0; }
int ajustes_hero_ligado(void)         { return valor[AJ_HERO]    == 0; }
int ajustes_hero_cheio(void)          { return valor[AJ_HERO_CHEIO] == 0; }
int ajustes_cw_estilo(void)           { return valor[AJ_CW_ESTILO]; }
int ajustes_rotulos_poster(void)      { return valor[AJ_ROTULOS] == 0; }
int ajustes_posteres_deitados(void)   { return valor[AJ_LANDSCAPE] == 0; }
// A regra do web, e nao dois layouts: o conteudo tem sempre 104 de recuo e a
// rail acrescenta os 144 dela quando esta fixa.
float ajustes_conteudo_x(void) {
  return ajustes_rail_recolhida() ? NV_CONTENT_PAD
                                  : NV_LEGACY_RAIL_W + NV_CONTENT_PAD;
}
const char *ajustes_qualidade(void)   { return V_QUALIDADE[valor[AJ_QUALIDADE]]; }

// Onde os ajustes ficam. Ate agora nada era gravado: mexer numa opcao valia so
// enquanto o app estivesse aberto, e voltar depois mostrava tudo no padrao —
// o que faz a tela inteira parecer decorativa.
static char dirAjustes[512];

void ajustes_dir(const char *dir) {
  FILE *f;
  char caminho[600], linha[64];
  if (!dir || !*dir) return;
  snprintf(dirAjustes, sizeof dirAjustes, "%s", dir);
  snprintf(caminho, sizeof caminho, "%s/ajustes.txt", dirAjustes);
  f = fopen(caminho, "r");
  if (!f) return;
  while (fgets(linha, sizeof linha, f)) {
    char chave[64]; int v, i;
    if (sscanf(linha, "%63s %d", chave, &v) != 2) continue;
    for (i = 0; i < AJ_N; i++) {
      if (strcmp(CHAVE[i], chave) || !OPCOES[i].valores) continue;
      // Valor fora da faixa (arquivo de outra versao, ou editado a mao) cai no
      // padrao em vez de indexar fora do vetor.
      if (v >= 0 && v < OPCOES[i].n) valor[i] = v;
      break;
    }
  }
  fclose(f);
}

static void gravar(void) {
  char caminho[600], tmp[600];
  FILE *f;
  int i;
  if (!dirAjustes[0]) return;
  snprintf(caminho, sizeof caminho, "%s/ajustes.txt", dirAjustes);
  snprintf(tmp, sizeof tmp, "%s/ajustes.tmp", dirAjustes);
  f = fopen(tmp, "w");
  if (!f) return;
  for (i = 0; i < AJ_N; i++)
    if (OPCOES[i].valores) fprintf(f, "%s %d\n", CHAVE[i], valor[i]);
  fclose(f);
  rename(tmp, caminho);
}

int ajustes_iniciar(void) { focoOp = 0; scrollY = 0.0f; sair = 0; return 1; }
void ajustes_encerrar(void) { }
int ajustes_quer_sair(void) { return sair; }

// Valor das linhas so de leitura. O espaco em disco NAO e um numero inventado:
// vem do cache de texturas, que e exatamente o que "imagens" consome no
// aparelho — um numero fixo aqui seria mentira e nunca mudaria.
static const char *textoLeitura(int op) {
  static char buf[64];
  if (op == AJ_VERSAO_I) return AJ_VERSAO;
  int itens = 0, pend = 0; long bytes = 0;
  tex_estatisticas(&itens, &pend, &bytes);
  snprintf(buf, sizeof buf, "%.1f MB em %d imagens", bytes / 1048576.0, itens);
  return buf;
}
static int soLeitura(int op) { return OPCOES[op].valores == NULL; }

// Deslocamento vertical do topo da lista ate a linha `op`, contando os
// cabecalhos das secoes que vieram antes.
static float yDaOpcao(int op) {
  float y = 0.0f;
  for (int s = 0; s < AJ_N_SECOES; s++) {
    y += (s ? AJ_SEC_GAP : 0.0f) + AJ_SEC_CABEC;
    for (int k = 0; k < SECOES[s].n; k++) {
      int o = SECOES[s].ini + k;
      if (o == op) return y;
      y += AJ_LINHA_H + AJ_LINHA_GAP;
    }
  }
  return y;
}

void ajustes_evento(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { sair = 1; return; }

  if (k == SDLK_DOWN)      { if (focoOp < AJ_N - 1) focoOp++; }
  else if (k == SDLK_UP)   { if (focoOp > 0)        focoOp--; }
  else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
    // Item so de leitura nao muda com nada: aqui, no OK, em lugar nenhum.
    if (soLeitura(focoOp)) return;
    int n = OPCOES[focoOp].n;
    // Circular: a lista de valores e curta e voltar do fim ao inicio poupa
    // quatro toques no controle. Sem circular, "720p" vira um beco.
    valor[focoOp] = (valor[focoOp] + (k == SDLK_RIGHT ? 1 : n - 1)) % n;
    gravar();   // grava a cada mudanca: nao ha botao de "salvar" nesta tela
  }
}

void ajustes_atualizar(float dt, Uint32 agora) {
  (void)agora;
  for (int i = 0; i < AJ_N; i++) {
    float alvo = (i == focoOp) ? 1.0f : 0.0f;
    animFoco[i] = anim_mola(animFoco[i], alvo, dt,
                            alvo > animFoco[i] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
  }
  // Rola o minimo para a linha focada caber, e leva junto o cabecalho da secao
  // quando a linha e a primeira dela — sem isso, entrar numa secao mostra a
  // opcao sem dizer a que grupo ela pertence.
  float topo = yDaOpcao(focoOp);
  for (int s = 0; s < AJ_N_SECOES; s++)
    if (SECOES[s].ini == focoOp) { topo -= AJ_SEC_CABEC; break; }
  float base = yDaOpcao(focoOp) + AJ_LINHA_H;
  float alvo = scrollY;
  if (base - alvo > AJ_BASE - AJ_TOPO) alvo = base - (AJ_BASE - AJ_TOPO);
  if (topo - alvo < 0.0f)              alvo = topo;
  if (alvo < 0.0f) alvo = 0.0f;
  scrollY = anim_mola(scrollY, alvo, dt, NV_MOLA_SCROLL);
}

static void desenhaLinha(int op, float y, float f) {
  if (y + AJ_LINHA_H < AJ_TOPO - 40.0f || y > AJ_BASE + 40.0f) return;
  // Some antes de cruzar o titulo da tela, como as secoes da pagina de detalhe:
  // texto passando por baixo de texto se le como borrao.
  float a = anim_clamp((y - (AJ_TOPO - 70.0f)) / 60.0f, 0.0f, 1.0f);
  if (a <= 0.005f) return;

  int leitura = soLeitura(op);
  GfxRect linha = { AJ_LISTA_X, y, AJ_LISTA_W, AJ_LINHA_H };
  // Aqui esta a diferenca visual entre mudavel e so leitura: a pilula clara com
  // texto escuro e a marca de "isto responde ao controle". A linha de leitura
  // recebe so um realce de 10% e mantem o texto claro — ela mostra que o foco
  // esta ali sem prometer interacao.
  if (leitura) {
    gfx_cor(linha, AJ_RAIO, 1, 1, 1, (0.03f + 0.09f * f) * a);
  } else {
    float lum = 0.62f + 0.34f * f;
    gfx_cor(linha, AJ_RAIO, lum, lum, lum, (0.06f + 0.86f * f) * a);
  }

  int escuro = (!leitura && f > 0.55f);
  int cr = escuro ? 24 : (leitura ? 176 : 240);
  TxtLinha rot = txt_linha(TXT_BODY, OPCOES[op].rotulo, cr, cr, cr, 255);
  txt_desenhar_alpha(rot, AJ_LISTA_X + AJ_PAD,
                     y + (AJ_LINHA_H - rot.h) * 0.5f, a);

  const char *v = leitura ? textoLeitura(op) : OPCOES[op].valores[valor[op]];
  int cv = escuro ? 70 : (leitura ? 132 : 178);
  TxtLinha val = txt_linha(TXT_BODY, v, cv, cv, cv, 255);
  float xDir = AJ_LISTA_X + AJ_LISTA_W - AJ_PAD;
  float vy = y + (AJ_LINHA_H - val.h) * 0.5f;

  // As setas so aparecem na linha em foco que MUDA. Elas sao a instrucao: sem
  // elas, nada na tela diz que esquerda/direita e o gesto certo.
  if (!leitura && f > 0.02f) {
    TxtLinha dir = txt_linha(TXT_CAPTION2, "\xe2\x96\xb6", cv, cv, cv, 255);
    TxtLinha esq = txt_linha(TXT_CAPTION2, "\xe2\x97\x80", cv, cv, cv, 255);
    txt_desenhar_alpha(dir, xDir - dir.w, y + (AJ_LINHA_H - dir.h) * 0.5f, a * f);
    txt_desenhar_alpha(val, xDir - dir.w - 16.0f - val.w, vy, a);
    txt_desenhar_alpha(esq, xDir - dir.w - 16.0f - val.w - 16.0f - esq.w,
                       y + (AJ_LINHA_H - esq.h) * 0.5f, a * f);
  } else {
    txt_desenhar_alpha(val, xDir - val.w, vy, a);
  }
}

void ajustes_desenhar(Uint32 agora) {
  (void)agora;
  // Fundo opaco proprio: a tela cobre tudo e nao pode depender de quem desenhou
  // antes dela — sem isto a home aparece entre as linhas da lista.
  GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
  gfx_cor(tela, 0.0f, NV_COR_FUNDO_R, NV_COR_FUNDO_G, NV_COR_FUNDO_B, 1.0f);

  TxtLinha tit = txt_linha(TXT_TITULO1, "Ajustes", 255, 255, 255, 255);
  txt_desenhar(tit, AJ_LISTA_X, NV_MARGEM_Y);

  float y = AJ_TOPO - scrollY;
  for (int s = 0; s < AJ_N_SECOES; s++) {
    if (s) y += AJ_SEC_GAP;
    // Cabecalho da secao em corpo pequeno e cinza: ele rotula o grupo, nao
    // compete com os rotulos das opcoes.
    float aC = anim_clamp((y - (AJ_TOPO - 70.0f)) / 60.0f, 0.0f, 1.0f);
    TxtLinha ts = txt_linha(TXT_CAPTION, SECOES[s].titulo, 150, 152, 160, 255);
    if (aC > 0.005f && y < AJ_BASE)
      txt_desenhar_alpha(ts, AJ_LISTA_X + AJ_PAD, y + AJ_SEC_CABEC - ts.h - 10.0f, aC);
    y += AJ_SEC_CABEC;
    for (int k = 0; k < SECOES[s].n; k++) {
      int op = SECOES[s].ini + k;
      desenhaLinha(op, y, animFoco[op]);
      y += AJ_LINHA_H + AJ_LINHA_GAP;
    }
  }
}
