#include "perfilsel.h"
#include "perfis.h"
#include "sync.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define PS_AVATAR      196.0f
#define PS_GAP          64.0f
#define PS_PIN_MAX       8
#define PS_TECLA         96.0f
#define PS_TECLA_GAP     18.0f

static int foco;
static int concluido;
static float animFoco[CONTA_PERFIL_MAX];

// Estado do PIN: -1 = nenhum perfil pedindo PIN.
static int pinDe = -1;
static char pin[PS_PIN_MAX + 1];
static int pinFoco;              // 0..9 digitos, 10 = apagar, 11 = confirmar
static int pinErrado;
static pthread_t fioPin;
static int verificando, resultadoPin;   // resultadoPin: 0 nada, 1 ok, -1 errou

static int corDe(const char *hex, float *r, float *g, float *b) {
  unsigned v = 0;
  if (!hex || hex[0] != '#' || strlen(hex) < 7) return 0;
  if (sscanf(hex + 1, "%6x", &v) != 1) return 0;
  *r = ((v >> 16) & 255) / 255.0f;
  *g = ((v >> 8) & 255) / 255.0f;
  *b = (v & 255) / 255.0f;
  return 1;
}

void perfilsel_iniciar(void) {
  int i;
  foco = 0;
  concluido = 0;
  pinDe = -1;
  pin[0] = 0;
  pinFoco = 0;
  pinErrado = 0;
  for (i = 0; i < CONTA_PERFIL_MAX; i++) animFoco[i] = 0.0f;
  // Se o perfil ativo ja e conhecido, comeca o foco nele: reabrir a tela e
  // encontrar o cursor no primeiro perfil sugere que a escolha se perdeu.
  for (i = 0; i < perfis_n(); i++)
    if (perfis_item(i)->indice == perfis_ativo()) { foco = i; break; }
}

static void *fioVerificar(void *u) {
  const ContaPerfil *p = perfis_item(pinDe);
  (void)u;
  resultadoPin = (p && perfis_verificar_pin(p->indice, pin)) ? 1 : -1;
  return NULL;
}

static void escolher(int i) {
  const ContaPerfil *p = perfis_item(i);
  if (!p) return;
  if (p->temPin) { pinDe = i; pin[0] = 0; pinFoco = 0; pinErrado = 0; return; }
  perfis_definir_ativo(p->indice);
  concluido = 1;
}

static void eventoPin(SDL_Keycode k) {
  if (verificando) return;
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE) {
    if (pin[0]) pin[strlen(pin) - 1] = 0;
    else pinDe = -1;
    return;
  }
  if (k == SDLK_LEFT)  { if (pinFoco > 0)  pinFoco--; return; }
  if (k == SDLK_RIGHT) { if (pinFoco < 11) pinFoco++; return; }
  if (k == SDLK_UP)    { if (pinFoco >= 5) pinFoco -= 5; return; }
  if (k == SDLK_DOWN)  { if (pinFoco + 5 <= 11) pinFoco += 5; return; }
  if (k != SDLK_RETURN && k != SDLK_KP_ENTER) return;

  if (pinFoco == 10) { if (pin[0]) pin[strlen(pin) - 1] = 0; return; }
  if (pinFoco == 11) {
    if (!pin[0]) return;
    verificando = 1;
    resultadoPin = 0;
    // Verificar BLOQUEIA (uma viagem ao servidor). Num fio, para a tela nao
    // congelar por um segundo a cada tentativa.
    if (pthread_create(&fioPin, NULL, fioVerificar, NULL) == 0) pthread_detach(fioPin);
    else { verificando = 0; pinErrado = 1; }
    return;
  }
  { size_t n = strlen(pin);
    if (n < PS_PIN_MAX) { pin[n] = (char)('0' + pinFoco); pin[n + 1] = 0; } }
}

void perfilsel_evento(const SDL_Event *e) {
  SDL_Keycode k;
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (pinDe >= 0) { eventoPin(k); return; }

  if (k == SDLK_RIGHT) { if (foco < perfis_n() - 1) foco++; }
  else if (k == SDLK_LEFT) { if (foco > 0) foco--; }
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) escolher(foco);
}

void perfilsel_atualizar(float dt, Uint32 agora) {
  int i;
  (void)agora;
  for (i = 0; i < CONTA_PERFIL_MAX; i++) {
    float alvo = (i == foco && pinDe < 0) ? 1.0f : 0.0f;
    animFoco[i] = anim_mola(animFoco[i], alvo, dt,
                            alvo > animFoco[i] ? NV_MOLA_FOCO : NV_MOLA_DESFOCO);
  }
  if (verificando && resultadoPin) {
    verificando = 0;
    if (resultadoPin == 1) {
      const ContaPerfil *p = perfis_item(pinDe);
      if (p) perfis_definir_ativo(p->indice);
      pinDe = -1;
      concluido = 1;
    } else {
      pinErrado = 1;
      pin[0] = 0;
    }
    resultadoPin = 0;
  }
  // NAO concluir enquanto o ciclo que BUSCA os perfis ainda esta rodando.
  //
  // O defeito que isto conserta: app.c troca para esta tela logo depois de
  // chamar sync_iniciar(), que e assincrono. No primeiro quadro perfis_n() e 0
  // porque a resposta nao chegou — e "0 perfis" e indistinguivel de "conta de
  // uma pessoa so". A tela se dispensava sozinha ANTES de existir, e uma conta
  // de duas pessoas caia no perfil 1 em silencio: o app sincronizava e
  // ESCREVIA progresso no perfil errado, sem nunca perguntar.
  if (sync_estado() == SYNC_RODANDO) return;

  // Terminado o ciclo, "nenhum ou um" e resposta de verdade: seguir direto.
  if (perfis_n() <= 1) concluido = 1;
}

static void desenhaPin(void) {
  static const char *ROT[12] = { "0","1","2","3","4","5","6","7","8","9","←","OK" };
  const ContaPerfil *p = perfis_item(pinDe);
  float larguraGrade = 5 * PS_TECLA + 4 * PS_TECLA_GAP;
  float x0 = (NV_TELA_W - larguraGrade) * 0.5f;
  float y0 = 520.0f;
  int i;
  char mascara[PS_PIN_MAX + 1];
  size_t n = strlen(pin), k;

  { GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
    gfx_cor(tela, 0.0f, 0.0f, 0.0f, 0.0f, 0.72f); }

  { char t[128];
    TxtLinha l;
    snprintf(t, sizeof t, "PIN de %s", p ? p->nome : "perfil");
    l = txt_linha(TXT_TITULO3, t, 255, 255, 255, 255);
    txt_desenhar(l, (NV_TELA_W - l.w) * 0.5f, 350.0f); }

  // Pontos, nunca os digitos: alguem passando na sala nao precisa ler o PIN.
  for (k = 0; k < n && k < PS_PIN_MAX; k++) mascara[k] = '*';
  mascara[k] = 0;
  { TxtLinha l = txt_linha(TXT_TITULO1, n ? mascara : "—", 255, 255, 255, 255);
    txt_desenhar(l, (NV_TELA_W - l.w) * 0.5f, 420.0f); }

  if (pinErrado) {
    TxtLinha l = txt_linha(TXT_BODY, "PIN incorreto", 236, 108, 108, 255);
    txt_desenhar(l, (NV_TELA_W - l.w) * 0.5f, 480.0f);
  }
  if (verificando) {
    TxtLinha l = txt_linha(TXT_BODY, "verificando…", 176, 178, 186, 255);
    txt_desenhar(l, (NV_TELA_W - l.w) * 0.5f, 480.0f);
  }

  for (i = 0; i < 12; i++) {
    int col = i % 5, lin = i / 5;
    GfxRect r = { x0 + col * (PS_TECLA + PS_TECLA_GAP),
                  y0 + lin * (PS_TECLA + PS_TECLA_GAP), PS_TECLA, PS_TECLA };
    int f = (i == pinFoco);
    TxtLinha l;
    gfx_cor(r, 0.22f, 1.0f, 1.0f, 1.0f, f ? 0.92f : 0.10f);
    l = txt_linha(TXT_TITULO3, ROT[i], f ? 24 : 235, f ? 24 : 235, f ? 26 : 240, 255);
    txt_desenhar(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f);
  }
}

void perfilsel_desenhar(Uint32 agora) {
  int i, n = perfis_n();
  float larguraTotal, x;
  (void)agora;

  { GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
    gfx_cor(tela, 0.0f, NV_COR_FUNDO_R, NV_COR_FUNDO_G, NV_COR_FUNDO_B, 1.0f); }

  { TxtLinha t = txt_linha(TXT_TITULO1, "Quem está assistindo?", 255, 255, 255, 255);
    txt_desenhar(t, (NV_TELA_W - t.w) * 0.5f, 200.0f); }

  // Enquanto a lista nao chega, dizer isso. Uma tela com titulo e nada abaixo
  // le como travamento.
  if (n == 0) {
    TxtLinha e = txt_linha(TXT_BODY, "Carregando os perfis da sua conta…",
                           160, 162, 170, 255);
    txt_desenhar(e, (NV_TELA_W - e.w) * 0.5f, 430.0f);
    return;
  }

  larguraTotal = n * PS_AVATAR + (n - 1) * PS_GAP;
  x = (NV_TELA_W - larguraTotal) * 0.5f;
  for (i = 0; i < n; i++) {
    const ContaPerfil *p = perfis_item(i);
    float f = animFoco[i];
    float cr = 0.12f, cg = 0.53f, cb = 0.90f;
    float cresce = PS_AVATAR * NV_FOCO_ESCALA_P * f;
    GfxRect a = { x - cresce * 0.5f, 420.0f - cresce * 0.5f - NV_FOCO_LIFT * f,
                  PS_AVATAR + cresce, PS_AVATAR + cresce };
    TxtLinha nome;
    corDe(p->corHex, &cr, &cg, &cb);
    // Circulo: raio = metade do lado no SDF normalizado.
    gfx_cor(a, 0.5f, cr, cg, cb, 1.0f);
    // A inicial no lugar do avatar: a arte do avatar mora no Storage do
    // Supabase e baixa-la exige uma viagem por perfil antes de a tela existir.
    { char ini[8] = { p->nome[0] ? p->nome[0] : '?', 0 };
      TxtLinha l;
      if ((unsigned char)ini[0] >= 0xC0 && p->nome[1]) { ini[1] = p->nome[1]; ini[2] = 0; }
      l = txt_linha(TXT_TITULO1, ini, 255, 255, 255, 255);
      txt_desenhar(l, a.x + (a.w - l.w) * 0.5f, a.y + (a.h - l.h) * 0.5f); }

    { int c = 140 + (int)(115 * f);
      nome = txt_linha_corta(TXT_BODY, p->nome, c, c, c, 255, PS_AVATAR + PS_GAP); }
    txt_desenhar(nome, x + (PS_AVATAR - nome.w) * 0.5f, 420.0f + PS_AVATAR + 28.0f);

    if (p->temPin) {
      // A PALAVRA, nao um cadeado. O emoji U+1F512 nao existe na fonte
      // embarcada e sai como retangulo vazio — a mesma armadilha que gfx.h ja
      // registra sobre o U+25B6 ("depender do glifo da fonte e loteria"), e na
      // qual eu cai de novo. Texto que a fonte tem sempre desenha.
      TxtLinha cad = txt_linha(TXT_CAPTION, "PIN", 150, 152, 160, 255);
      txt_desenhar(cad, x + (PS_AVATAR - cad.w) * 0.5f, 420.0f + PS_AVATAR + 70.0f);
    }
    x += PS_AVATAR + PS_GAP;
  }

  if (pinDe >= 0) desenhaPin();
}

int perfilsel_concluido(void) { return concluido; }
