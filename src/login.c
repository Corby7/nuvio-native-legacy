#include "login.h"
#include "session.h"
#include "cloud.h"
#include "qr.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// O QR e o caminho principal, e nao por gosto. MEDIDO na resposta do servidor:
// o codigo tem 32 digitos hexadecimais e a URL ~63 caracteres. Ninguem
// transcreve isso da TV para o celular sem errar — sem QR, esta tela nao
// funciona.
#define LG_QR_SIDE       440.0f
#define LG_BLOCK_W      1100.0f
#define LG_PILL_W        360.0f
#define LG_PILL_H         76.0f
// Zona de silencio: 4 modulos claros em volta, exigidos pela norma. Vao DENTRO
// da textura para que nenhum ajuste de layout possa comer a margem por
// acidente — sem ela, leitor nenhum acha o simbolo.
#define LG_QR_MARGIN       4

static float animButton;
static float pulse;

static GLuint texQr;
static char   qrOf[512];   // conteudo ja desenhado, para nao refazer por quadro

// Sobe o simbolo como textura em vez de desenhar um retangulo por modulo: a
// versao 4 tem 33x33 = 1089 modulos, e mil chamadas de desenho por quadro
// custam mais que a tela inteira.
static void generateTexQr(const char *text) {
  Qr q;
  int n, side, x, y;
  unsigned char *px;
  if (!text || !text[0]) return;
  if (!strcmp(qrOf, text) && texQr) return;
  if (!qr_generate(&q, text)) { printf("[login] URL does not fit in a QR: %s\n", text); return; }

  side = q.side + 2 * LG_QR_MARGIN;
  px = (unsigned char *)malloc((size_t)side * side * 3);
  if (!px) return;
  memset(px, 255, (size_t)side * side * 3);   // fundo claro, inclusive a margem
  for (y = 0; y < q.side; y++)
    for (x = 0; x < q.side; x++)
      if (qr_modulo(&q, x, y)) {
        size_t i = ((size_t)(y + LG_QR_MARGIN) * side + (x + LG_QR_MARGIN)) * 3;
        px[i] = px[i + 1] = px[i + 2] = 0;
      }

  if (!texQr) glGenTextures(1, &texQr);
  glBindTexture(GL_TEXTURE_2D, texQr);
  // NEAREST, nao LINEAR: um modulo borrado com o vizinho e o jeito mais rapido
  // de tornar o simbolo ilegivel numa camera de celular.
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, side, side, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  free(px);
  n = snprintf(qrOf, sizeof qrOf, "%s", text);
  (void)n;
}

void login_start(void) {
  animButton = 0.0f;
  pulse = 0.0f;
  // Pedir o codigo JA, sem esperar o OK: a pessoa que acabou de instalar o app
  // nao tem nada para decidir nesta tela, e um botao "entrar" antes do codigo
  // so acrescenta um toque e uns segundos de espera depois dele.
  if (!session_loggedin()) session_login_begin();
}

void login_event(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  { SDL_Keycode k = e->key.keysym.sym;
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      // OK so faz sentido quando ha o que refazer. Com o codigo na tela ele nao
      // faz nada de proposito: reiniciar o fluxo aqui trocaria o codigo que a
      // pessoa acabou de digitar no celular.
      if (session_state() == SESS_ERROR || session_state() == SESS_LOGGEDOUT)
        session_login_begin();
    } }
}

void login_update(float dt, Uint32 now) {
  session_step((unsigned)now);
  animButton = anim_spring(animButton, 1.0f, dt, NV_SPRING_FOCUS);
  pulse += dt;
}

static void lineCentered(TxtStyle st, const char *s, int r, int g, int b,
                          float y, float alpha) {
  TxtLine l = txt_line_trim(st, s, r, g, b, 255, LG_BLOCK_W);
  txt_draw_alpha(l, (NV_SCREEN_W - l.w) * 0.5f, y, alpha);
}

void login_draw(Uint32 now) {
  SessState st = session_state();
  float y;
  (void)now;

  { GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f); }

  y = 118.0f;
  lineCentered(TXT_TITLE1, "Sign in to your account", 255, 255, 255, y, 1.0f);
  y += 118.0f;

  if (!cloud_ready()) {
    // Este caso e de COMPILACAO, nao do usuario: o pacote saiu sem a
    // configuracao do servidor. Dizer "erro ao entrar" mandaria a pessoa tentar
    // de novo para sempre contra algo que nunca vai funcionar.
    lineCentered(TXT_HEADLINE, "This package was built without a server.",
                  236, 108, 108, y, 1.0f);
    lineCentered(TXT_BODY,
                  "Whoever built the .ipk needs to supply the project URL and key.",
                  176, 178, 186, y + 62.0f, 1.0f);
    return;
  }

  switch (st) {
    case SESS_REQUESTING:
      lineCentered(TXT_HEADLINE, "Preparing the code…", 210, 212, 220, y, 1.0f);
      break;

    case SESS_WAITING: {
      const char *url = session_url_login();
      lineCentered(TXT_BODY, "Point your phone camera at the code:",
                    176, 178, 186, y, 1.0f);
      y += 62.0f;

      generateTexQr(url);
      if (texQr) {
        // Moldura clara um pouco maior que o simbolo: sobre o fundo escuro da
        // tela, a zona de silencio da textura sozinha ja bastaria, mas a
        // moldura arredondada faz o bloco ler como um cartao e nao como um
        // buraco branco no meio da tela.
        GfxRect frame = { (NV_SCREEN_W - LG_QR_SIDE - 32.0f) * 0.5f, y - 16.0f,
                            LG_QR_SIDE + 32.0f, LG_QR_SIDE + 32.0f };
        GfxRect r = { (NV_SCREEN_W - LG_QR_SIDE) * 0.5f, y, LG_QR_SIDE, LG_QR_SIDE };
        gfx_color(frame, 0.06f, 1.0f, 1.0f, 1.0f, 1.0f);
        gfx_tex_aspect_current = 0.0f;   // 1:1, sem recorte
        gfx_rect(r, texQr, GFX_SNAP, 0, 0.0f, 0.0f, 0.0f, 0, 0, 0, 1.0f);
      } else {
        lineCentered(TXT_HEADLINE, "could not draw the code",
                      236, 108, 108, y + 100.0f, 1.0f);
      }
      y += LG_QR_SIDE + 42.0f;

      // O endereco em texto e a saida de emergencia de quem nao tem camera —
      // nao e o caminho principal, e por isso vem em corpo pequeno.
      if (url[0]) lineCentered(TXT_CAPTION, url, 150, 152, 160, y, 1.0f);
      y += 46.0f;

      // Sinal de vida. Sem ele a tela fica parada por minutos e parece travada
      // — e a pessoa reinicia o app no meio do login. Respiracao lenta (ciclo
      // de 2s), nao piscada: piscar em texto de espera le como alerta.
      { float a = 0.5f + 0.5f * sinf(pulse * 3.14159f);
        lineCentered(TXT_CAPTION, "Waiting for authorisation…",
                      150, 152, 160, y, 0.45f + 0.40f * a); }
      break;
    }

    case SESS_SWITCHING:
      lineCentered(TXT_HEADLINE, "Authorised. Signing in…", 210, 212, 220, y, 1.0f);
      break;

    case SESS_LOGGEDIN:
      lineCentered(TXT_HEADLINE, "Ready.", 210, 212, 220, y, 1.0f);
      break;

    case SESS_ERROR:
    case SESS_LOGGEDOUT:
    default: {
      const char *msg = session_error();
      lineCentered(TXT_HEADLINE, msg[0] ? msg : "Could not reach the server.",
                    236, 108, 108, y, 1.0f);
      y += 96.0f;
      { GfxRect pill = { (NV_SCREEN_W - LG_PILL_W) * 0.5f, y, LG_PILL_W, LG_PILL_H };
        TxtLine t;
        gfx_color(pill, NV_RADIUS_PILL, 1.0f, 1.0f, 1.0f, 0.92f * animButton);
        t = txt_line(TXT_BODY, "Try again", 24, 24, 26, 255);
        txt_draw_alpha(t, (NV_SCREEN_W - t.w) * 0.5f,
                           y + (LG_PILL_H - t.h) * 0.5f, animButton); }
      break;
    }
  }
}

int login_done(void) { return session_loggedin(); }
