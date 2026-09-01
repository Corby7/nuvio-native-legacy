// Bootstrap: janela, contexto GL, loop e telemetria. Toda a UI vive nos modulos.
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "gl_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "home.h"
#include "text.h"
#include "detail.h"
#include "app.h"
#include "video.h"
#include "addons.h"
#include "ajustes.h"
#include "descoberta.h"
#include "trakt.h"
#include "player.h"
#ifndef __APPLE__
#include <dlfcn.h>
#include <SDL2/SDL_syswm.h>
#endif
#include "layout.h"

// Captura de tela sob demanda. O framebuffer da TV nao pode ser lido nem como
// root ("Operation not permitted") e o servico de captura da LG responde erro,
// entao a unica forma de ver o que o app desenha e o proprio app se fotografar.
// Sem isso, cada ajuste visual depende de alguem apontar um celular para a TV.
//
// Protocolo: alguem cria /tmp/nuvio-shot-req; no proximo quadro o app grava
// /tmp/nuvio-shot.png e apaga o pedido.
// Teclas injetadas por arquivo, para conferir a UI sem alguem no sofa com o
// controle: escreva "down", "ok", "back"... em /tmp/nuvio-key e o app processa
// como se viesse do D-pad. Uma tecla por linha, o arquivo e consumido.
static SDL_Keycode codigoDaTecla(const char *nome) {
  if (!strcmp(nome, "up"))    return SDLK_UP;
  if (!strcmp(nome, "down"))  return SDLK_DOWN;
  if (!strcmp(nome, "left"))  return SDLK_LEFT;
  if (!strcmp(nome, "right")) return SDLK_RIGHT;
  if (!strcmp(nome, "ok"))    return SDLK_RETURN;
  if (!strcmp(nome, "back"))  return SDLK_AC_BACK;
  return 0;
}

// O arquivo e CONSUMIDO truncando, nunca apagando: /tmp tem sticky bit e os
// arquivos sao criados por root, entao o app (uid 5410) nao consegue remove-los.
// Enquanto isso nao foi visto, cada pedido era reprocessado a cada quadro —
// uma unica tecla "down" virava centenas e o foco corria ate o fim da pagina.
static void consome(const char *caminho) {
  FILE *f = fopen(caminho, "w");
  if (f) fclose(f);
}
static long tamanhoDe(const char *caminho) {
  FILE *f = fopen(caminho, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}

// KEYUP adiado de uma tecla segurada.
static Uint32 soltarEm = 0;
static SDL_Keycode soltarTecla = 0;

static void teclasInjetadas(void (*entregar)(const SDL_Event *)) {
  if (soltarEm && SDL_GetTicks() >= soltarEm) {
    SDL_Event up; SDL_zero(up);
    up.type = SDL_KEYUP; up.key.keysym.sym = soltarTecla;
    entregar(&up);
    soltarEm = 0;
  }
  if (tamanhoDe("/tmp/nuvio-key") <= 0) return;
  FILE *f = fopen("/tmp/nuvio-key", "r");
  if (!f) return;
  char linha[32];
  while (fgets(linha, sizeof linha, f)) {
    char *fim = linha + strlen(linha);
    while (fim > linha && (fim[-1] == '\n' || fim[-1] == '\r' || fim[-1] == ' ')) *--fim = 0;
    // "ok:hold" simula a pressao longa: o KEYUP dela fica agendado para depois
    // do limiar, em vez de vir junto. Sem isso nao da para exercitar por aqui
    // nada que dependa de segurar o botao.
    int segurar = 0;
    char *dp = strchr(linha, ':');
    if (dp && !strcmp(dp + 1, "hold")) { *dp = 0; segurar = 1; }

    SDL_Keycode k = codigoDaTecla(linha);
    if (!k) continue;
    SDL_Event e; SDL_zero(e);
    e.type = SDL_KEYDOWN; e.key.keysym.sym = k;
    entregar(&e);

    // O par KEYUP existe porque parte da interface so decide quando a tecla
    // SOBE — o toque curto contra a pressao longa do OK, por exemplo. Mandar
    // so o KEYDOWN deixava essas acoes mudas.
    if (segurar) { soltarEm = SDL_GetTicks() + NV_HOLD_MS + 120; soltarTecla = k; }
    else { e.type = SDL_KEYUP; entregar(&e); }
  }
  fclose(f);
  consome("/tmp/nuvio-key");
}

// Tamanho do buffer de onde a captura le. Definido no arranque, junto com o
// viewport.
static int capW = (int)NV_TELA_W, capH = (int)NV_TELA_H;

// Mesmo protocolo das outras ferramentas: escreva uma URL em /tmp/nuvio-video e
// o app toca. E o unico jeito de testar reproducao sem alguem no sofa — e o
// video nao pode ser conferido por captura, porque vive em outro plano.
static void videoSeSolicitado(void) {
  char url[1024];
  FILE *f;
  if (tamanhoDe("/tmp/nuvio-video") <= 0) return;
  f = fopen("/tmp/nuvio-video", "r");
  if (!f) return;
  if (fgets(url, sizeof url, f)) {
    char *fim = url + strlen(url);
    while (fim > url && (fim[-1] == '\n' || fim[-1] == '\r')) *--fim = 0;
    printf("[video] pedido: %s\n", url);
    fflush(stdout);
    if (url[0] == '-') video_parar();
    else { video_tocar(url); video_janela(0, 0, 1920, 1080); }
  }
  fclose(f);
  consome("/tmp/nuvio-video");
}

static void capturaSeSolicitado(void) {
  // Mesma armadilha do sticky bit: pedido vale enquanto tiver conteudo.
  if (tamanhoDe("/tmp/nuvio-shot-req") <= 0) return;
  consome("/tmp/nuvio-shot-req");

  // Le o DRAWABLE inteiro, nao 1920x1080 fixo: em tela retina o buffer e maior
  // que a janela, e ler o tamanho da janela captura so um quarto da imagem.
  int w = capW, h = capH;
  size_t n = (size_t)w * h * 4;
  unsigned char *px = malloc(n);
  if (!px) return;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);

  // BMP escrito a mao, em UM fwrite. SDL_SaveBMP converte pixel a pixel quando
  // as mascaras nao batem com o formato nativo, e nesta CPU isso leva segundos:
  // o arquivo ficava incompleto quando eu ia le-lo. Aqui a unica conversao e a
  // troca R<->B, feita no proprio buffer.
  for (size_t i = 0; i < n; i += 4) { unsigned char t2 = px[i]; px[i] = px[i+2]; px[i+2] = t2; }

  unsigned int tam = 54 + (unsigned int)n;
  unsigned char cab[54] = {0};
  cab[0] = 'B'; cab[1] = 'M';
  cab[2] = tam & 255; cab[3] = (tam >> 8) & 255; cab[4] = (tam >> 16) & 255; cab[5] = (tam >> 24) & 255;
  cab[10] = 54; cab[14] = 40;
  cab[18] = w & 255; cab[19] = (w >> 8) & 255;
  // altura POSITIVA = linhas de baixo para cima, que e exatamente a ordem em
  // que o glReadPixels devolve. Assim nao ha inversao a fazer.
  cab[22] = h & 255; cab[23] = (h >> 8) & 255;
  cab[26] = 1; cab[28] = 32;
  cab[34] = n & 255; cab[35] = (n >> 8) & 255; cab[36] = (n >> 16) & 255; cab[37] = (n >> 24) & 255;

  // grava num temporario e so entao renomeia: quem le nunca pega arquivo pela metade
  FILE *f = fopen("/tmp/.nuvio-shot.tmp", "wb");
  if (f) {
    fwrite(cab, 1, 54, f);
    fwrite(px, 1, n, f);
    fclose(f);
    rename("/tmp/.nuvio-shot.tmp", "/tmp/nuvio-shot.bmp");
    printf("captura: /tmp/nuvio-shot.bmp (%u bytes)\n", tam);
  }
  free(px);
}

int main(int argc, char **argv) {
  // Sem a identidade do app, o SDL do webOS registra a surface como "(null)" e
  // o compositor NAO exibe a janela — o app roda a 60fps desenhando para
  // ninguem. Medido: "Invalid appId specified OR Unsupported Application Type".
#ifndef __APPLE__
  setenv("APPID", "space.nuvio.native.legacy", 0);
  setenv("LS2_APPID", "space.nuvio.native.legacy", 0);
  setenv("SDL_VIDEODRIVER", "wayland", 0);
#endif
  // Lancado pelo SAM, stdout e stderr vao para /dev/null — toda a telemetria
  // (FPS, texturas, teclas) estava sendo descartada em silencio. Log em arquivo
  // e a unica forma de ler qualquer coisa de um app nativo em execucao normal.
#ifndef __APPLE__
  freopen("/tmp/nuvio.log", "w", stdout);
  freopen("/tmp/nuvio.log", "a", stderr);
#endif
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 1);

  // O SAM lanca o app passando o JSON de launch como argv[1], entao so tratamos
  // argv[1] como caminho quando NAO for JSON.
  char dirBuf[512];
  const char *dirArte = NULL;
  if (argc > 1 && argv[1][0] != '{') dirArte = argv[1];
  if (!dirArte) {
    char *base = SDL_GetBasePath();
    if (base) { snprintf(dirBuf, sizeof dirBuf, "%sart", base); SDL_free(base); dirArte = dirBuf; }
    else dirArte = "/tmp/art";
  }

  // O compositor do webOS engole o BACK e abre a barra de apps — a menos que a
  // surface declare que o app quer a tecla. Quem faz essa declaracao e o
  // backend Wayland do SDL da LG, atraves deste hint, e ele so e lido na
  // CRIACAO da janela: setar depois nao adianta.
  //
  // Com o hint ligado, o Back chega como um scancode proprio do webOS (482), e
  // nao como SDLK_AC_BACK nem como o 461 dos apps web. Foi por isso que o
  // registro de todos os eventos SDL nao mostrava nada: a tecla nunca era
  // entregue, e o codigo que ela usa tambem nao era o que eu procurava.
  SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");

  if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
  IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);

#ifdef __APPLE__
  // Perfil de compatibilidade: e o unico do macOS que ainda aceita GLSL 1.20 e
  // as funcoes fixas que o GLES2 tem como core.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  // Canal alpha no framebuffer. Sem ele a superficie nao tem como ficar
  // transparente, e o plano de video do aparelho — que fica ATRAS da janela e
  // so aparece pelo alpha — nunca poderia ser revelado.
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN;
#endif
  // 4K NAO E POSSIVEL NESTE APARELHO — MEDIDO, nao presumido.
  //
  // A TV e 4K, e a ideia (do dono) era renderizar em 3840x2160 e desenhar tudo
  // em dobro: o texto pararia de ser rasterizado a 1080p e ampliado pelo
  // painel, que e o borrao que aparece ao lado do app web.
  //
  // Foram tentados os dois caminhos, na TV, com o contador de quadro do proprio
  // app gravando em /tmp/nuvio-fps.txt:
  //   1. SDL_CreateWindow com 3840x2160  -> drawable=1920x1080
  //   2. appinfo.json "resolution": "3840x2160" -> drawable=1920x1080
  // O compositor do webOS 4.10 fixa a superficie do app nativo em 1080p e
  // ignora os dois pedidos, em silencio. Nao ha o que otimizar aqui: a saida
  // seria o painel receber 1080p e ampliar, que e o que ja acontece.
  //
  // Base para comparacao futura, medida nesta tela (home, sem rolar):
  //   drawable=1920x1080 FPS=50.0 pior=21ms janks=0
  //
  // txt_iniciar continua recebendo a escala do drawable: no aparelho ela e 1 e
  // nao muda nada, no Mac (retina) ela e 2 e a previa deixa de mentir.
  SDL_Window *win = SDL_CreateWindow("Nuvio", SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED,
                                     (int)NV_TELA_W, (int)NV_TELA_H, flags);
  if (!win) { printf("janela: %s\n", SDL_GetError()); return 1; }
  // App de TV nao tem ponteiro: o cursor por cima da interface polui a leitura
  // e some sozinho no aparelho, mas nao no Mac.
  SDL_ShowCursor(SDL_DISABLE);
#ifndef __APPLE__
  // Declara a superficie NAO-opaca. Por padrao o compositor trata a janela como
  // opaca e descarta o canal alpha inteiro — o furo do gfx_furo existiria no
  // framebuffer e mesmo assim nada apareceria atras dele.
  //
  // Duas armadilhas medidas neste aparelho, ambas silenciosas:
  // 1. o SDL daqui escreve um SDL_SysWMinfo MAIOR que o header declara, entao a
  //    struct vai num buffer folgado e nao numa variavel do tamanho "certo";
  // 2. o `version` tem de vir de SDL_GetVersion(); preenchido a mao o SDL
  //    recusa em silencio e a unica pista e a tela preta.
  {
    static char infoBuf[512];
    SDL_SysWMinfo *info = (SDL_SysWMinfo *)infoBuf;
    SDL_GetVersion(&info->version);
    if (SDL_GetWindowWMInfo(win, info)) {
      // O SDL_config.h do SDK vem com SDL_VIDEO_DRIVER_WAYLAND desligado, entao
      // o campo info.wl nem existe no header — mas o SDL do aparelho E wayland.
      // Ler por deslocamento evita depender de um header que descreve outra
      // compilacao: version ocupa 3 bytes (alinhado a 4), subsystem vem em 4, e
      // a uniao comeca em 8. Para wayland ela e {display, surface, ...}.
      int sub = *(int *)(infoBuf + 4);
      void **campos = (void **)(infoBuf + 8);
      void *sup = campos[1];
      void *wl = dlopen("libwayland-client.so.0", RTLD_NOW);
      void (*marshal)(void *, unsigned, ...) =
          wl ? (void (*)(void *, unsigned, ...))dlsym(wl, "wl_proxy_marshal") : NULL;
      printf("syswm sub=%d display=%p surface=%p\n", sub, campos[0], sup);
      // Opcode 4 de wl_surface e set_opaque_region; NULL = "nada e opaco".
      // Sem commit de proposito: o commit vem do proximo SwapWindow.
      if (marshal && sup) { marshal(sup, 4, NULL); printf("superficie nao-opaca\n"); }
      else printf("sem wayland: video nao vai aparecer\n");
    }
  }
#endif
  SDL_GLContext ctx = SDL_GL_CreateContext(win);
#ifdef __APPLE__
  // Sem vsync no Mac. O SDL2 do Homebrew virou uma camada sobre o SDL3
  // (sdl2-compat), e nela o SwapWindow fica preso esperando um sinal de vsync
  // que nunca chega quando a janela nao esta em primeiro plano — o app trava no
  // primeiro quadro. No aparelho o SDL2 e o de verdade e o vsync fica ligado,
  // que e o que mantem os 60fps estaveis la.
  SDL_GL_SetSwapInterval(0);
#else
  SDL_GL_SetSwapInterval(1);
#endif
  // O tamanho REAL do buffer importa mais que o tamanho pedido: esta TV e 4K, e
  // se o compositor entregar uma superficie 3840x2160 cada camada de tela cheia
  // custa quatro vezes o que a conta de 1080p diz.
  int dw = 0, dh = 0, jw = 0, jh = 0;
  SDL_GL_GetDrawableSize(win, &dw, &dh);
  SDL_GetWindowSize(win, &jw, &jh);
  printf("GPU: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  printf("janela=%dx%d drawable=%dx%d\n", jw, jh, dw, dh);
  // Pedir SDL_GL_ALPHA_SIZE nao garante receber: o EGL escolhe a config mais
  // proxima e pode entregar 0 bits de alpha em silencio. Com 0 aqui, o furo da
  // superficie e impossivel e o plano de video NUNCA vai aparecer, por mais
  // certo que esteja o lado do ACB.
  { int a = -1, r = -1, g = -1, b = -1;
    SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
    SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
    SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
    SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
    printf("framebuffer R%d G%d B%d A%d%s\n", r, g, b, a,
           a > 0 ? "" : "  <<< SEM ALPHA: video nao tem como aparecer"); }

  // Em tela retina o drawable e maior que a janela; sem ajustar o viewport, o
  // desenho ocupa um quarto da tela.
  SDL_GL_GetDrawableSize(win, &dw, &dh);
  glViewport(0, 0, dw, dh);
  gfx_tamanho_alvo(dw, dh);
  capW = dw; capH = dh;

  if (!gfx_iniciar()) return 1;
  // fonts/ fica ao lado de art/: derruba o ultimo componente do caminho da arte
  char dirRec[512];
  snprintf(dirRec, sizeof dirRec, "%s", dirArte);
  char *barra = strrchr(dirRec, '/');
  if (barra) *barra = 0;
  txt_iniciar(dirRec, (float)dw / NV_TELA_W);
  tex_iniciar(96);
  if (!app_iniciar(dirArte)) return 1;
  // A configuracao de addons mora junto da arte. Ausente, o app segue com a
  // lista de exemplo — nunca fica sem nada para mostrar.
  addons_carregar(dirArte);
  ajustes_dir(dirArte);
  { // As imagens vindas de URL ficam ao lado da arte do pacote. Uma vez
    // baixadas valem para sempre: arte de filme nao muda.
    char c[600];
    snprintf(c, sizeof c, "%s/cache", dirArte);
    tex_cache_dir(c); }
  // Catalogo da rede. O do pacote ja esta carregado e continua na tela ate a
  // resposta chegar — abrir vazio enquanto busca seria pior que mostrar o de
  // ontem por dois segundos.
  trakt_carregar(dirArte);
  desc_tmdb(dirArte);
  desc_iniciar();
  // Metade da resolucao: o snapshot so aparece escurecido e nas bordas.
  int temSnap = gfx_snap_iniciar((int)NV_TELA_W / 2, (int)NV_TELA_H / 2);
  int snapValido = 0;
  // Alvo minusculo de proposito: e ele esticado que vira o desfoque do fundo.
  // 480x270: com o gaussiano de duas passadas, o que importa nao e o alvo ser
  // minusculo (isso e que produzia blocos ao esticar) e sim o desfoque ser de
  // verdade. Esticado 4x, nenhuma borda de texel aparece.
  gfx_borrao_iniciar(480, 270);

  Uint32 ultQuadro = SDL_GetTicks(), ultRelato = ultQuadro;
  double txtMsQuadro = 0, piorTxtMs = 0;
  int    txtNQuadro = 0, piorTxtN = 0;
  int quadros = 0, janks = 0; double pior = 0;

  while (!app_quer_sair()) {
    SDL_Event e;
    // Enquanto o detalhe existe ele fica com o teclado inteiro: a home
    // continua desenhada por baixo, mas nao deve reagir ao D-pad.
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_WINDOWEVENT) continue;
      // O BACK do webOS chega com scancode proprio (482), nao como AC_BACK, e
      // com KEYDOWN e KEYUP quase juntos — so o KEYDOWN conta. Isto ja tinha
      // sido resolvido uma vez e voltou a quebrar quando limpei os remendos
      // antigos: o tratamento saiu junto.
      if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == NV_SCANCODE_BACK) {
        SDL_Event back; SDL_zero(back);
        back.type = SDL_KEYDOWN;
        back.key.keysym.sym = SDLK_AC_BACK;
        app_evento(&back);
        continue;
      }
      app_evento(&e);
    }
    teclasInjetadas(app_evento);

    Uint32 agora = SDL_GetTicks();
    float dt = (agora - ultQuadro) / 1000.0f; ultQuadro = agora;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    double dtms = dt * 1000.0;
    if (quadros > 20) {
      if (dtms > pior) { pior = dtms; piorTxtMs = txtMsQuadro; piorTxtN = txtNQuadro; }
      if (dtms > 33.0) janks++;
    }
    // zera os contadores do quadro que comeca agora; o que foi medido acima
    // pertence ao quadro anterior, que e o que acabou de custar dtms
    txtMsQuadro = txt_ms; txtNQuadro = txt_rasterizadas;
    txt_ms = 0.0; txt_rasterizadas = 0;

    // Sobe no maximo 2 texturas por quadro: o upload e barato, mas dois ja
    // bastam para preencher a tela rapido sem estourar o orcamento do quadro.
    // 1 por quadro, nao 2. Cada envio e um glTexImage2D de ~2 MB; dois no mesmo
    // quadro somavam mais de 20 ms e apareciam como tranco ao entrar numa
    // fileira nova. Um por quadro enche a tela em meio segundo, que ninguem ve.
    tex_bombear(1);
    app_atualizar(dt, agora);

    glClearColor(NV_COR_FUNDO_R, NV_COR_FUNDO_G, NV_COR_FUNDO_B, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    txt_novo_quadro();
    app_desenhar(agora);
    videoSeSolicitado();
    capturaSeSolicitado();
    SDL_GL_SwapWindow(win);
    quadros++;

    if (agora - ultRelato >= 3000) {
      int itens, pend; long bytes;
      tex_estatisticas(&itens, &pend, &bytes);
      printf("FPS=%.1f pior=%.0fms janks=%d | pior-quadro: texto %.1fms em %d linhas"
             " | texturas=%d pend=%d %.1fMB\n",
             quadros * 1000.0 / (double)(agora - ultRelato), pior, janks,
             piorTxtMs, piorTxtN, itens, pend, bytes / 1048576.0);
      fflush(stdout);
      // A MESMA linha vai para um arquivo. No aparelho a saida padrao do app
      // lancado pelo applicationManager nao chega a lugar nenhum que se possa
      // ler, e rodar o binario a mao nao funciona (sem a identidade do app o
      // compositor recusa a superficie e ele morre em silencio). Sem isto nao
      // ha como MEDIR quadro no aparelho — so olhar e achar.
      { FILE *fp = fopen("/tmp/nuvio-fps.txt", "w");
        if (fp) {
          fprintf(fp, "drawable=%dx%d FPS=%.1f pior=%.0fms janks=%d"
                  " texto=%.1fms/%d texturas=%d %.1fMB\n", dw, dh,
                  quadros * 1000.0 / (double)(agora - ultRelato), pior, janks,
                  piorTxtMs, piorTxtN, itens, bytes / 1048576.0);
          fclose(fp);
        } }
      quadros = 0; ultRelato = agora; pior = 0; janks = 0; piorTxtMs = 0; piorTxtN = 0;
    }
  }

  gfx_borrao_encerrar();
  gfx_snap_encerrar();
  app_encerrar();
  tex_encerrar();
  txt_encerrar();
  gfx_encerrar();
  SDL_GL_DeleteContext(ctx);
  SDL_DestroyWindow(win);
  IMG_Quit();
  SDL_Quit();
  printf("fim\n");
  return 0;
}
