#include "login.h"
#include "sessao.h"
#include "nuvem.h"
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
#define LG_QR_LADO       440.0f
#define LG_BLOCO_W      1100.0f
#define LG_PILL_W        360.0f
#define LG_PILL_H         76.0f
// Zona de silencio: 4 modulos claros em volta, exigidos pela norma. Vao DENTRO
// da textura para que nenhum ajuste de layout possa comer a margem por
// acidente — sem ela, leitor nenhum acha o simbolo.
#define LG_QR_MARGEM       4

static float animBotao;
static float pulso;

static GLuint texQr;
static char   qrDe[512];   // conteudo ja desenhado, para nao refazer por quadro

// Sobe o simbolo como textura em vez de desenhar um retangulo por modulo: a
// versao 4 tem 33x33 = 1089 modulos, e mil chamadas de desenho por quadro
// custam mais que a tela inteira.
static void gerarTexQr(const char *texto) {
  Qr q;
  int n, lado, x, y;
  unsigned char *px;
  if (!texto || !texto[0]) return;
  if (!strcmp(qrDe, texto) && texQr) return;
  if (!qr_gerar(&q, texto)) { printf("[login] URL nao cabe num QR: %s\n", texto); return; }

  lado = q.lado + 2 * LG_QR_MARGEM;
  px = (unsigned char *)malloc((size_t)lado * lado * 3);
  if (!px) return;
  memset(px, 255, (size_t)lado * lado * 3);   // fundo claro, inclusive a margem
  for (y = 0; y < q.lado; y++)
    for (x = 0; x < q.lado; x++)
      if (qr_modulo(&q, x, y)) {
        size_t i = ((size_t)(y + LG_QR_MARGEM) * lado + (x + LG_QR_MARGEM)) * 3;
        px[i] = px[i + 1] = px[i + 2] = 0;
      }

  if (!texQr) glGenTextures(1, &texQr);
  glBindTexture(GL_TEXTURE_2D, texQr);
  // NEAREST, nao LINEAR: um modulo borrado com o vizinho e o jeito mais rapido
  // de tornar o simbolo ilegivel numa camera de celular.
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, lado, lado, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  free(px);
  n = snprintf(qrDe, sizeof qrDe, "%s", texto);
  (void)n;
}

void login_iniciar(void) {
  animBotao = 0.0f;
  pulso = 0.0f;
  // Pedir o codigo JA, sem esperar o OK: a pessoa que acabou de instalar o app
  // nao tem nada para decidir nesta tela, e um botao "entrar" antes do codigo
  // so acrescenta um toque e uns segundos de espera depois dele.
  if (!sessao_logada()) sessao_login_comecar();
}

void login_evento(const SDL_Event *e) {
  if (e->type != SDL_KEYDOWN) return;
  { SDL_Keycode k = e->key.keysym.sym;
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
      // OK so faz sentido quando ha o que refazer. Com o codigo na tela ele nao
      // faz nada de proposito: reiniciar o fluxo aqui trocaria o codigo que a
      // pessoa acabou de digitar no celular.
      if (sessao_estado() == SES_ERRO || sessao_estado() == SES_DESLOGADO)
        sessao_login_comecar();
    } }
}

void login_atualizar(float dt, Uint32 agora) {
  sessao_passo((unsigned)agora);
  animBotao = anim_mola(animBotao, 1.0f, dt, NV_MOLA_FOCO);
  pulso += dt;
}

static void linhaCentrada(TxtEstilo est, const char *s, int r, int g, int b,
                          float y, float alpha) {
  TxtLinha l = txt_linha_corta(est, s, r, g, b, 255, LG_BLOCO_W);
  txt_desenhar_alpha(l, (NV_TELA_W - l.w) * 0.5f, y, alpha);
}

void login_desenhar(Uint32 agora) {
  SesEstado st = sessao_estado();
  float y;
  (void)agora;

  { GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
    gfx_cor(tela, 0.0f, NV_COR_FUNDO_R, NV_COR_FUNDO_G, NV_COR_FUNDO_B, 1.0f); }

  y = 118.0f;
  linhaCentrada(TXT_TITULO1, "Entrar na sua conta", 255, 255, 255, y, 1.0f);
  y += 118.0f;

  if (!nuvem_pronta()) {
    // Este caso e de COMPILACAO, nao do usuario: o pacote saiu sem a
    // configuracao do servidor. Dizer "erro ao entrar" mandaria a pessoa tentar
    // de novo para sempre contra algo que nunca vai funcionar.
    linhaCentrada(TXT_HEADLINE, "Este pacote foi montado sem servidor.",
                  236, 108, 108, y, 1.0f);
    linhaCentrada(TXT_BODY,
                  "Quem gerou o .ipk precisa informar a URL e a chave do projeto.",
                  176, 178, 186, y + 62.0f, 1.0f);
    return;
  }

  switch (st) {
    case SES_PEDINDO:
      linhaCentrada(TXT_HEADLINE, "Preparando o código…", 210, 212, 220, y, 1.0f);
      break;

    case SES_AGUARDANDO: {
      const char *url = sessao_url_login();
      linhaCentrada(TXT_BODY, "Aponte a câmera do celular para o código:",
                    176, 178, 186, y, 1.0f);
      y += 62.0f;

      gerarTexQr(url);
      if (texQr) {
        // Moldura clara um pouco maior que o simbolo: sobre o fundo escuro da
        // tela, a zona de silencio da textura sozinha ja bastaria, mas a
        // moldura arredondada faz o bloco ler como um cartao e nao como um
        // buraco branco no meio da tela.
        GfxRect moldura = { (NV_TELA_W - LG_QR_LADO - 32.0f) * 0.5f, y - 16.0f,
                            LG_QR_LADO + 32.0f, LG_QR_LADO + 32.0f };
        GfxRect r = { (NV_TELA_W - LG_QR_LADO) * 0.5f, y, LG_QR_LADO, LG_QR_LADO };
        gfx_cor(moldura, 0.06f, 1.0f, 1.0f, 1.0f, 1.0f);
        gfx_tex_aspect_atual = 0.0f;   // 1:1, sem recorte
        gfx_rect(r, texQr, GFX_SNAP, 0, 0.0f, 0.0f, 0.0f, 0, 0, 0, 1.0f);
      } else {
        linhaCentrada(TXT_HEADLINE, "não consegui desenhar o código",
                      236, 108, 108, y + 100.0f, 1.0f);
      }
      y += LG_QR_LADO + 42.0f;

      // O endereco em texto e a saida de emergencia de quem nao tem camera —
      // nao e o caminho principal, e por isso vem em corpo pequeno.
      if (url[0]) linhaCentrada(TXT_CAPTION, url, 150, 152, 160, y, 1.0f);
      y += 46.0f;

      // Sinal de vida. Sem ele a tela fica parada por minutos e parece travada
      // — e a pessoa reinicia o app no meio do login. Respiracao lenta (ciclo
      // de 2s), nao piscada: piscar em texto de espera le como alerta.
      { float a = 0.5f + 0.5f * sinf(pulso * 3.14159f);
        linhaCentrada(TXT_CAPTION, "Aguardando a autorização…",
                      150, 152, 160, y, 0.45f + 0.40f * a); }
      break;
    }

    case SES_TROCANDO:
      linhaCentrada(TXT_HEADLINE, "Autorizado. Entrando…", 210, 212, 220, y, 1.0f);
      break;

    case SES_LOGADO:
      linhaCentrada(TXT_HEADLINE, "Pronto.", 210, 212, 220, y, 1.0f);
      break;

    case SES_ERRO:
    case SES_DESLOGADO:
    default: {
      const char *msg = sessao_erro();
      linhaCentrada(TXT_HEADLINE, msg[0] ? msg : "Não consegui falar com o servidor.",
                    236, 108, 108, y, 1.0f);
      y += 96.0f;
      { GfxRect pill = { (NV_TELA_W - LG_PILL_W) * 0.5f, y, LG_PILL_W, LG_PILL_H };
        TxtLinha t;
        gfx_cor(pill, NV_RAIO_PILL, 1.0f, 1.0f, 1.0f, 0.92f * animBotao);
        t = txt_linha(TXT_BODY, "Tentar de novo", 24, 24, 26, 255);
        txt_desenhar_alpha(t, (NV_TELA_W - t.w) * 0.5f,
                           y + (LG_PILL_H - t.h) * 0.5f, animBotao); }
      break;
    }
  }
}

int login_concluido(void) { return sessao_logada(); }
