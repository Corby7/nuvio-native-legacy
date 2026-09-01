#include "streams.h"
#include "rede.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include <stdio.h>
#include <string.h>

#define FOLHA_H       720.0f
#define FOLHA_LINHA    88.0f
#define FOLHA_PAD      56.0f

static Stream lista[STREAM_MAX];
static int n = 0;

static int aberta = 0, foco = 0, escolha = -1;
static float anim = 0.0f, rolagem = 0.0f;

static const char *containerDa(const Stream *s) {
  if (s->mp4 || strstr(s->url, ".mp4") || strstr(s->rotulo, ".mp4")) return "MP4";
  if (strstr(s->url, ".mkv") || strstr(s->rotulo, ".mkv")) return "MKV";
  if (strstr(s->url, ".m3u8") || strstr(s->rotulo, "HLS")) return "HLS";
  return "ARQUIVO";
}

// CONJUNTO DE EXEMPLO. Nao ha addon ligado ainda: isto existe para a ordenacao,
// a folha E AGORA A REPRODUCAO poderem ser exercitadas de ponta a ponta. A
// ordem aqui e proposital — o preferido NAO e o primeiro, senao a regra de
// escolha nunca seria testada.
//
// As URLs apontam para o servidor de teste da maquina de desenvolvimento. Sao
// o MESMO arquivo de proposito: o que se testa aqui e a cadeia (botao -> regra
// -> pipeline -> plano de video), nao a diferenca entre as fontes. Quem ligar
// os addons troca isto por stream_definir_lista com as fontes de verdade e
// apaga este bloco inteiro.
//
// ATENCAO ao trocar por arquivos proprios: fonte SEM FAIXA DE AUDIO nao toca
// nesta TV. O relogio do pipeline e regido pelo audio e o currentTime congela
// em ~266ms, sem erro nenhum, com load e play retornando sucesso.
#define NV_TESTE_URL "http://192.168.1.181:8899/longo.mp4"
static const Stream EXEMPLO[] = {
  { "Fonte A  ·  1080p",           "Fonte A", NV_TESTE_URL, 1080, 0, 0, 1, 2400 },
  { "Fonte B  ·  4K HDR10",        "Fonte B", NV_TESTE_URL, 2160, 0, 1, 1, 8100 },
  { "Fonte C  ·  4K Dolby Vision", "Fonte C", NV_TESTE_URL, 2160, 1, 1, 1, 9600 },
  { "Fonte D  ·  4K Dolby Vision", "Fonte D", NV_TESTE_URL, 2160, 1, 1, 0, 9200 },
  { "Fonte E  ·  720p",            "Fonte E", NV_TESTE_URL,  720, 0, 0, 1,  900 },
};

static Uint32 recebidaEm;

Uint32 stream_idade_ms(void) {
  return recebidaEm ? SDL_GetTicks() - recebidaEm : 0xFFFFFFFFu;
}

void stream_definir_lista(const Stream *l, int qtd) {
  recebidaEm = SDL_GetTicks();
  n = qtd > STREAM_MAX ? STREAM_MAX : (qtd < 0 ? 0 : qtd);
  if (l && n) memcpy(lista, l, sizeof(Stream) * (size_t)n);
  foco = 0;
}

int stream_n(void) {
  // Sem lista definida, cai no exemplo — assim a tela nunca aparece vazia
  // enquanto os addons nao estao ligados.
  if (!n) stream_definir_lista(EXEMPLO, (int)(sizeof EXEMPLO / sizeof *EXEMPLO));
  return n;
}

const Stream *stream_item(int i) {
  if (!stream_n()) return NULL;
  return &lista[((i % n) + n) % n];
}

// Pontuacao da regra do dono, do mais forte para o mais fraco:
//   MP4 4K Dolby Vision  >  4K Dolby Vision (qualquer container)
//   >  4K  >  Dolby Vision  >  resolucao  >  ordem de chegada
//
// Somar pesos em vez de comparar campo a campo deixa a regra num lugar so e
// legivel: mudar a preferencia e mexer num numero, nao reescrever um encadeado
// de ifs onde a ordem das comparacoes vira a regra escondida.
static long pontos(const Stream *s) {
  long p = 0;
  if (s->mp4 && s->altura >= 2160 && s->dolbyVision) p += 100000;
  if (s->altura >= 2160 && s->dolbyVision)           p +=  50000;
  if (s->altura >= 2160)                             p +=  20000;
  if (s->dolbyVision)                                p +=  10000;
  if (s->dolbyAtmos)                                 p +=   2000;
  p += s->altura;
  return p;
}

// Endereco de aviso e nao de conteudo. Estes dois foram MEDIDOS no aparelho:
// o AIOStreams manda para slate.m3u8/slate.mp4 ("This playback link couldn't be
// verified") quando o link expirou, e o Debridio para downloading.mp4 quando o
// arquivo ainda nao esta em cache no Real-Debrid. Os dois sao MP4 validos de
// ~120s que TOCAM NORMALMENTE — nao ha erro para detectar, so o endereco.
static int enderecoDeAviso(const char *u) {
  return strstr(u, "downloading.mp4") || strstr(u, "/slate") ||
         strstr(u, "slate.mp4") || strstr(u, "slate.m3u8") ? 1 : 0;
}

int stream_primeira_boa(int tentativas) {
  int usados[STREAM_MAX], nu = 0, k;
  int total = stream_n();
  if (total < 1) return -1;
  if (tentativas < 1) tentativas = 1;
  for (k = 0; k < tentativas && nu < total; k++) {
    int melhor = -1;
    long maiorP = 0;
    int i, j;
    // Melhor ainda nao tentado, pela mesma regra do automatico.
    for (i = 0; i < total; i++) {
      int visto = 0;
      for (j = 0; j < nu; j++) if (usados[j] == i) { visto = 1; break; }
      if (visto) continue;
      { long p = pontos(&lista[i]);
        if (melhor < 0 || p > maiorP) { melhor = i; maiorP = p; } }
    }
    if (melhor < 0) break;
    usados[nu++] = melhor;
    { char fim[900];
      if (!lista[melhor].url[0]) continue;
      if (!rede_url_final(lista[melhor].url, 20, fim, sizeof fim)) {
        printf("[fonte] %d nao resolveu\n", melhor);
        continue;
      }
      if (enderecoDeAviso(fim)) {
        printf("[fonte] %d e aviso (%.60s), tentando a proxima\n", melhor, fim);
        continue;
      }
      printf("[fonte] %d ok -> %.60s\n", melhor, fim);
      return melhor; }
  }
  return -1;
}

int stream_automatico(void) {
  if (!stream_n()) return -1;
  int melhor = 0;
  long maior = pontos(&lista[0]);
  for (int i = 1; i < n; i++) {
    long p = pontos(&lista[i]);
    // `>` e nao `>=`: em empate fica o PRIMEIRO da lista, que e a ordem em que
    // o addon devolveu — e ele costuma saber algo que a pontuacao nao ve.
    if (p > maior) { maior = p; melhor = i; }
  }
  return melhor;
}

void stream_folha_abrir(void) {
  stream_n();
  aberta = 1;
  escolha = -1;
  int a = stream_automatico();
  // Abre com o foco no que o automatico tocaria: quem abriu a lista quer
  // COMPARAR com essa escolha, nao procurar onde ela esta.
  foco = a >= 0 ? a : 0;
  rolagem = 0.0f;
}

int stream_folha_aberta(void) { return aberta; }

void stream_folha_evento(const SDL_Event *e) {
  if (!aberta || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE ||
      k == SDLK_DELETE) { aberta = 0; return; }
  if (k == SDLK_DOWN && foco < n - 1) foco++;
  else if (k == SDLK_UP && foco > 0)  foco--;
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { escolha = foco; aberta = 0; }
}

void stream_folha_atualizar(float dt, Uint32 agora) {
  (void)agora;
  anim = anim_mola(anim, aberta ? 1.0f : 0.0f, dt, NV_MOLA_TELA);
  // Mantem o foco no miolo da janela e revela os itens seguintes. Antes a
  // lista desenhava sempre a partir do zero: o foco andava para itens que
  // existiam, mas estavam literalmente fora da folha.
  { const float area = FOLHA_H - 142.0f;
    const float max = n * FOLHA_LINHA > area ? n * FOLHA_LINHA - area : 0.0f;
    float alvo = foco * FOLHA_LINHA - (area - FOLHA_LINHA) * 0.5f;
    if (alvo < 0) alvo = 0;
    if (alvo > max) alvo = max;
    rolagem = anim_mola(rolagem, alvo, dt, NV_MOLA_SCROLL); }
}

int stream_folha_escolheu(int *out) {
  if (escolha < 0) return 0;
  if (out) *out = escolha;
  escolha = -1;
  return 1;
}

void stream_folha_desenhar(Uint32 agora) {
  (void)agora;
  if (!aberta && anim < 0.004f) return;

  GfxRect tela = { 0, 0, NV_TELA_W, NV_TELA_H };
  gfx_cor(tela, 0.0f, 0, 0, 0, 0.62f * anim);

  // A folha sobe da base. Sobe, e nao aparece: a lista de fontes e uma gaveta
  // do que ja esta em cena, nao uma tela nova.
  float h = FOLHA_H;
  float y = NV_TELA_H - h * anim;
  GfxRect folha = { 0, y, NV_TELA_W, h };
  gfx_cor(folha, 0.0f, 0.086f, 0.090f, 0.098f, 0.985f * anim);

  TxtLinha tit = txt_linha(TXT_HEADLINE, "Fontes", 255, 255, 255, 255);
  txt_desenhar_alpha(tit, NV_MARGEM_X, y + 34.0f, anim);

  { char pos[48]; snprintf(pos, sizeof pos, "%d de %d", foco + 1, n);
    TxtLinha lp = txt_linha(TXT_CAPTION2, pos, 178, 180, 190, 255);
    txt_desenhar_alpha(lp, NV_TELA_W - NV_MARGEM_X - lp.w,
                       y + 42.0f, anim * 0.9f); }

  int aut = stream_automatico();
  float topo = y + 34.0f + tit.h + 22.0f;
  float areaH = FOLHA_H - (topo - y) - 22.0f;
  gfx_recorte(0, topo, NV_TELA_W, areaH);
  for (int i = 0; i < n; i++) {
    float ly = topo + i * FOLHA_LINHA - rolagem;
    if (ly + FOLHA_LINHA < topo || ly > topo + areaH) continue;
    int sel = (i == foco);
    GfxRect linha = { NV_MARGEM_X - 24.0f, ly, NV_TELA_W - (NV_MARGEM_X - 24.0f) * 2,
                      FOLHA_LINHA - 12.0f };
    if (sel) gfx_cor(linha, 0.14f, 0.93f, 0.93f, 0.95f, 0.95f * anim);

    int cor = sel ? 24 : 236;
    txt_bloco(TXT_BODY, lista[i].rotulo, cor, cor, cor,
              NV_MARGEM_X, ly + 9.0f, 1160.0f, NV_LD_BODY, anim, 1);

    // Formato vem PRIMEIRO e em caixa alta: o objetivo desta folha e permitir
    // achar um MP4 DV sem depender do nome enorme da release.
    char meta[160]; size_t u;
    snprintf(meta, sizeof meta, "%s", containerDa(&lista[i]));
#define META(fmt, ...) do { u = strlen(meta); snprintf(meta + u, sizeof meta - u, "  ·  " fmt, __VA_ARGS__); } while (0)
    if (lista[i].altura >= 2160) META("%s", "4K");
    else if (lista[i].altura > 0) META("%dp", lista[i].altura);
    if (lista[i].dolbyVision) META("%s", "DOLBY VISION");
    if (lista[i].dolbyAtmos) META("%s", "ATMOS");
    if (lista[i].tamanhoMB > 0) META("%.1f GB", lista[i].tamanhoMB / 1024.0);
#undef META
    { int c2 = sel ? 72 : 164;
      TxtLinha lm = txt_linha(TXT_CAPTION2, meta, c2, c2, c2 + 8, 255);
      txt_desenhar_alpha(lm, NV_MARGEM_X, ly + 49.0f, anim * 0.95f); }

    if (lista[i].provedor[0]) {
      int c3 = sel ? 80 : 152;
      TxtLinha lp = txt_linha(TXT_CAPTION2, lista[i].provedor, c3, c3, c3 + 6, 255);
      txt_desenhar_alpha(lp, NV_TELA_W - NV_MARGEM_X - lp.w,
                         ly + 13.0f, anim * 0.88f);
    }

    // Marca qual o automatico tocaria. Sem essa marca a lista nao explica por
    // que o app escolheu o que escolheu, e a escolha vira mistério.
    if (i == aut) {
      TxtLinha la = txt_linha(TXT_MINI, "AUTOMATICO", sel ? 70 : 150,
                              sel ? 70 : 150, sel ? 76 : 158, 255);
      txt_desenhar_alpha(la, NV_TELA_W - NV_MARGEM_X - la.w,
                         ly + 49.0f, anim * 0.9f);
    }
  }
  gfx_sem_recorte();

  // Affordance de scroll: deixa claro que ha conteudo acima/abaixo antes de o
  // usuario tentar adivinhar. A posicao numerica no cabecalho e o complemento.
  if (rolagem > 4.0f) {
    TxtLinha a = txt_linha(TXT_CAPTION2, "▲  mais fontes", 170, 172, 182, 255);
    txt_desenhar_alpha(a, (NV_TELA_W - a.w) * 0.5f, topo - 2.0f, anim * 0.8f);
  }
  if (rolagem + areaH < n * FOLHA_LINHA - 4.0f) {
    TxtLinha a = txt_linha(TXT_CAPTION2, "▼  mais fontes", 170, 172, 182, 255);
    txt_desenhar_alpha(a, (NV_TELA_W - a.w) * 0.5f,
                       y + FOLHA_H - a.h - 5.0f, anim * 0.8f);
  }
}
