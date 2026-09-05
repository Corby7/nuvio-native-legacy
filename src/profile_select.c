#include "profile_select.h"
#include "profiles.h"
#include "sync.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include "settings.h"
#include "session.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#define PS_COLS           4
#define PS_AVATAR      156.0f
#define PS_GAP           48.0f
#define PS_ROW_GAP       72.0f
#define PS_PIN_MAX       8
#define PS_KEY         96.0f
#define PS_KEY_GAP     18.0f

static int focus;
static int done, sair, retry;
static float animFocus[ACCOUNT_PROFILE_MAX];

// Estado do PIN: -1 = nenhum perfil pedindo PIN.
static int pinOf = -1;
static char pin[PS_PIN_MAX + 1];
static int pinFocus;              // 0..9 digitos, 10 = apagar, 11 = confirmar
static int pinWrong, pinNet;
static pthread_t threadPin;
static int verifying;
static _Atomic int resultPin; // 0 pendente, 1 ok, -1 PIN incorreto, -2 rede
static _Atomic unsigned pinGeneration;
typedef struct { unsigned generation; int slot, index_; char value[PS_PIN_MAX + 1]; } PinTask;

static int colorOf(const char *hex, float *r, float *g, float *b) {
  unsigned v = 0;
  if (!hex || hex[0] != '#' || strlen(hex) < 7) return 0;
  if (sscanf(hex + 1, "%6x", &v) != 1) return 0;
  *r = ((v >> 16) & 255) / 255.0f;
  *g = ((v >> 8) & 255) / 255.0f;
  *b = (v & 255) / 255.0f;
  return 1;
}

void profilesel_start(void) {
  int i;
  focus = 0;
  done = sair = retry = 0;
  pinOf = -1;
  pin[0] = 0;
  pinFocus = 0;
  pinWrong = 0;
  pinNet = 0;
  verifying = 0;
  atomic_fetch_add(&pinGeneration, 1);
  atomic_store(&resultPin, 0);
  for (i = 0; i < ACCOUNT_PROFILE_MAX; i++) animFocus[i] = 0.0f;
  // Se o perfil ativo ja e conhecido, comeca o foco nele: reabrir a tela e
  // encontrar o cursor no primeiro perfil sugere que a escolha se perdeu.
  for (i = 0; i < profiles_n(); i++)
    if (profiles_item(i)->index_ == profiles_active()) { focus = i; break; }
}

static void *threadVerify(void *u) {
  PinTask *t = u;
  char body[128]; int status = 0, result = -2;
  snprintf(body, sizeof body, "{\"p_profile_id\":%d,\"p_pin\":\"%s\"}", t->index_, t->value);
  { char *r = session_rpc("verify_profile_pin", body, &status);
    if (r && status >= 200 && status < 300) result = strstr(r, "true") ? 1 : -1;
    free(r); }
  if (t->generation == atomic_load(&pinGeneration) && pinOf == t->slot && verifying)
    atomic_store(&resultPin, result);
  free(t);
  return NULL;
}

static void choose(int i) {
  const AccountProfile *p = profiles_item(i);
  if (!p) return;
  if (p->hasPin) { pinOf = i; pin[0] = 0; pinFocus = 0; pinWrong = pinNet = 0; return; }
  profiles_set_active(p->index_);
  done = 1;
}

static void eventPin(SDL_Keycode k) {
  if (verifying) {
    if (k == SDLK_AC_BACK || k == SDLK_ESCAPE) {
      atomic_fetch_add(&pinGeneration, 1); atomic_store(&resultPin, 0);
      verifying = 0; pinNet = 0; pinWrong = 0;
    }
    return;
  }
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE) {
    if (pin[0]) { pin[strlen(pin) - 1] = 0; pinWrong = pinNet = 0; }
    else pinOf = -1;
    return;
  }
  if (k == SDLK_LEFT)  { if (pinFocus > 0)  pinFocus--; return; }
  if (k == SDLK_RIGHT) { if (pinFocus < 11) pinFocus++; return; }
  if (k == SDLK_UP)    { if (pinFocus >= 5) pinFocus -= 5; return; }
  if (k == SDLK_DOWN)  { if (pinFocus + 5 <= 11) pinFocus += 5; return; }
  if (k != SDLK_RETURN && k != SDLK_KP_ENTER) return;

  if (pinFocus == 10) { if (pin[0]) pin[strlen(pin) - 1] = 0; return; }
  if (pinFocus == 11) {
    if (!pin[0]) return;
    PinTask *t = malloc(sizeof *t);
    const AccountProfile *p = profiles_item(pinOf);
    if (!t || !p) { free(t); pinNet = 1; return; }
    t->generation = atomic_load(&pinGeneration); t->slot = pinOf; t->index_ = p->index_;
    snprintf(t->value, sizeof t->value, "%s", pin);
    verifying = 1;
    pinWrong = pinNet = 0;
    atomic_store(&resultPin, 0);
    // Verificar BLOQUEIA (uma viagem ao servidor). Num fio, para a tela nao
    // congelar por um segundo a cada tentativa.
    if (pthread_create(&threadPin, NULL, threadVerify, t) == 0) pthread_detach(threadPin);
    else { free(t); verifying = 0; pinNet = 1; }
    return;
  }
  { size_t n = strlen(pin);
    if (n < PS_PIN_MAX) { pin[n] = (char)('0' + pinFocus); pin[n + 1] = 0; } }
}

void profilesel_event(const SDL_Event *e) {
  SDL_Keycode k;
  if (e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (pinOf >= 0) { eventPin(k); return; }

  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE) { sair = 1; return; }
  if (sync_state() == SYNC_FAILED && (k == SDLK_RETURN || k == SDLK_KP_ENTER)) { retry = 1; return; }
  if (k == SDLK_RIGHT) { if (focus < profiles_n() - 1 && focus%PS_COLS < PS_COLS-1) focus++; }
  else if (k == SDLK_LEFT) { if (focus > 0 && focus%PS_COLS > 0) focus--; }
  else if (k == SDLK_UP) { if (focus >= PS_COLS) focus -= PS_COLS; }
  else if (k == SDLK_DOWN) { if (focus + PS_COLS < profiles_n()) focus += PS_COLS; }
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) choose(focus);
}

void profilesel_update(float dt, Uint32 now) {
  int i;
  (void)now;
  int reduced=settings_animations_reduced();
  for (i = 0; i < ACCOUNT_PROFILE_MAX; i++) {
    float target = (i == focus && pinOf < 0) ? 1.0f : 0.0f;
    animFocus[i] = anim_spring(animFocus[i], target, dt,
                            target > animFocus[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
    if(reduced)animFocus[i]=target;
  }
  { int result = atomic_load(&resultPin);
  if (verifying && result) {
    atomic_store(&resultPin, 0);
    verifying = 0;
    if (result == 1) {
      const AccountProfile *p = profiles_item(pinOf);
      if (p) profiles_set_active(p->index_);
      pinOf = -1;
      done = 1;
    } else if (result == -2) {
      pinNet = 1;
      pin[0] = 0;
    } else {
      pinWrong = 1;
      pin[0] = 0;
    }
  }
  }
  // NAO concluir enquanto o ciclo que BUSCA os perfis ainda esta rodando.
  //
  // O defeito que isto conserta: app.c troca para esta tela logo depois de
  // chamar sync_iniciar(), que e assincrono. No primeiro quadro perfis_n() e 0
  // porque a resposta nao chegou — e "0 perfis" e indistinguivel de "conta de
  // uma pessoa so". A tela se dispensava sozinha ANTES de existir, e uma conta
  // de duas pessoas caia no perfil 1 em silencio: o app sincronizava e
  // ESCREVIA progresso no perfil errado, sem nunca perguntar.
  if (sync_state() == SYNC_RUNNING) return;

  // Terminado o ciclo, "nenhum ou um" e resposta de verdade: seguir direto.
  // Um erro de rede nunca equivale a "uma conta sem perfis". So concluir
  // automaticamente quando o ciclo terminou com sucesso; assim a proxima
  // pessoa nao cai silenciosamente no perfil implicito 1.
  if (sync_state() == SYNC_READY && profiles_n() <= 1) done = 1;
}

int profilesel_wants_exit(void) { int v=sair; sair=0; return v; }
int profilesel_requested_retry(void) { int v=retry; retry=0; return v; }

static void drawPin(void) {
  static const char *ROT[12] = { "0","1","2","3","4","5","6","7","8","9","←","OK" };
  const AccountProfile *p = profiles_item(pinOf);
  float widthGrid = 5 * PS_KEY + 4 * PS_KEY_GAP;
  float x0 = (NV_SCREEN_W - widthGrid) * 0.5f;
  float y0 = 520.0f;
  int i;
  char mascara[PS_PIN_MAX + 1];
  size_t n = strlen(pin), k;

  { GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, 0.0f, 0.0f, 0.0f, 0.72f); }

  { char t[128];
    TxtLine l;
    snprintf(t, sizeof t, "PIN for %s", p ? p->name : "profile");
    l = txt_line(TXT_TITLE3, t, 255, 255, 255, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, 350.0f); }

  // Pontos, nunca os digitos: alguem passando na sala nao precisa ler o PIN.
  for (k = 0; k < n && k < PS_PIN_MAX; k++) mascara[k] = '*';
  mascara[k] = 0;
  { TxtLine l = txt_line(TXT_TITLE1, n ? mascara : "—", 255, 255, 255, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, 420.0f); }

  if (pinNet) {
    TxtLine l = txt_line(TXT_BODY, "No connection. Try again.", 236, 150, 150, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, 480.0f);
  } else if (pinWrong) {
    TxtLine l = txt_line(TXT_BODY, "Incorrect PIN", 236, 108, 108, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, 480.0f);
  }
  if (verifying) {
    TxtLine l = txt_line(TXT_BODY, "checking…", 176, 178, 186, 255);
    txt_draw(l, (NV_SCREEN_W - l.w) * 0.5f, 480.0f);
  }

  for (i = 0; i < 12; i++) {
    int col = i % 5, lin = i / 5;
    GfxRect r = { x0 + col * (PS_KEY + PS_KEY_GAP),
                  y0 + lin * (PS_KEY + PS_KEY_GAP), PS_KEY, PS_KEY };
    int f = (i == pinFocus);
    TxtLine l;
    gfx_color(r, 0.22f, 1.0f, 1.0f, 1.0f, f ? 0.92f : 0.10f);
    l = txt_line(TXT_TITLE3, ROT[i], f ? 24 : 235, f ? 24 : 235, f ? 26 : 240, 255);
    txt_draw(l, r.x + (r.w - l.w) * 0.5f, r.y + (r.h - l.h) * 0.5f);
  }
}

void profilesel_draw(Uint32 now) {
  int i, n = profiles_n();
  float widthTotal, x, y;
  (void)now;

  { GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f); }

  { TxtLine t = txt_line(TXT_TITLE1, "Who is watching?", 255, 255, 255, 255);
    txt_draw(t, (NV_SCREEN_W - t.w) * 0.5f, 200.0f); }

  // Enquanto a lista nao chega, dizer isso. Uma tela com titulo e nada abaixo
  // le como travamento.
  if (n == 0) {
    const char *msg=sync_state()==SYNC_FAILED?
      "Could not load the profiles. OK: try again":
      "Loading the profiles on your account…";
    TxtLine e = txt_line(TXT_BODY, msg,
                           160, 162, 170, 255);
    txt_draw(e, (NV_SCREEN_W - e.w) * 0.5f, 430.0f);
    return;
  }

  widthTotal = PS_COLS * PS_AVATAR + (PS_COLS - 1) * PS_GAP;
  x = (NV_SCREEN_W - widthTotal) * 0.5f;
  for (i = 0; i < n; i++) {
    const AccountProfile *p = profiles_item(i);
    float f = animFocus[i];
    int col=i%PS_COLS,row=i/PS_COLS;
    float px=x+col*(PS_AVATAR+PS_GAP);
    y=350.0f+row*(PS_AVATAR+PS_ROW_GAP);
    float cr = 0.12f, cg = 0.53f, cb = 0.90f;
    float grows = PS_AVATAR * NV_FOCUS_SCALE_P * f;
    GfxRect a = { px - grows * 0.5f, y - grows * 0.5f - NV_FOCUS_LIFT * f,
                  PS_AVATAR + grows, PS_AVATAR + grows };
    TxtLine name;
    colorOf(p->colorHex, &cr, &cg, &cb);
    // Circulo: raio = metade do lado no SDF normalizado.
    gfx_color(a, 0.5f, cr, cg, cb, 1.0f);
    // A inicial no lugar do avatar: a arte do avatar mora no Storage do
    // Supabase e baixa-la exige uma viagem por perfil antes de a tela existir.
    { char start[8] = { p->name[0] ? p->name[0] : '?', 0 };
      TxtLine l;
      if ((unsigned char)start[0] >= 0xC0 && p->name[1]) { start[1] = p->name[1]; start[2] = 0; }
      l = txt_line(TXT_TITLE1, start, 255, 255, 255, 255);
      txt_draw(l, a.x + (a.w - l.w) * 0.5f, a.y + (a.h - l.h) * 0.5f); }

    if (f > .02f)
      gfx_rect((GfxRect){a.x-NV_RING_FOCUS,a.y-NV_RING_FOCUS,
                         a.w+NV_RING_FOCUS*2,a.h+NV_RING_FOCUS*2},
               0,GFX_RING,0,NV_RING_FOCUS/(a.w+NV_RING_FOCUS*2),0,.5f,
               .96f,.96f,.98f,f);

    { int c = 140 + (int)(115 * f);
      name = txt_line_trim(TXT_BODY, p->name, c, c, c, 255, PS_AVATAR + PS_GAP); }
    txt_draw(name, px + (PS_AVATAR - name.w) * 0.5f, y + PS_AVATAR + 20.0f);

    if (p->hasPin) {
      // A PALAVRA, nao um cadeado. O emoji U+1F512 nao existe na fonte
      // embarcada e sai como retangulo vazio — a mesma armadilha que gfx.h ja
      // registra sobre o U+25B6 ("depender do glifo da fonte e loteria"), e na
      // qual eu cai de novo. Texto que a fonte tem sempre desenha.
      TxtLine cad = txt_line(TXT_CAPTION, "PIN", 150, 152, 160, 255);
      txt_draw(cad, px + (PS_AVATAR - cad.w) * 0.5f, y + PS_AVATAR + 54.0f);
    }
  }

  { TxtLine d = txt_line(TXT_CAPTION,
                           "Arrows: move  ·  OK: choose",
                           182,184,194,255);
    txt_draw(d,(NV_SCREEN_W-d.w)*.5f,870.0f); }

  if (pinOf >= 0) drawPin();
}

int profilesel_done(void) { return done; }
