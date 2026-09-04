// Bootstrap: janela, contexto GL, loop e telemetria. Toda a UI vive nos modulos.
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "gl_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "gfx.h"
#include "text.h"
#include "mark.h"
#include "net.h"
#include "tex_cache.h"
#include "home.h"
#include "text.h"
#include "detail.h"
#include "data.h"
#include "cloud.h"
#include "session.h"
#include "profiles.h"
#include "sync.h"
#include "traktauth.h"
#include "simklauth.h"
#include "app.h"
#include "video.h"
#include "addons.h"
#include "settings.h"
#include "discover.h"
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
static SDL_Keycode codeOfKey(const char *name) {
  if (!strcmp(name, "up"))    return SDLK_UP;
  if (!strcmp(name, "down"))  return SDLK_DOWN;
  if (!strcmp(name, "left"))  return SDLK_LEFT;
  if (!strcmp(name, "right")) return SDLK_RIGHT;
  if (!strcmp(name, "ok"))    return SDLK_RETURN;
  if (!strcmp(name, "back"))  return SDLK_AC_BACK;
  return 0;
}

// O arquivo e CONSUMIDO truncando, nunca apagando: /tmp tem sticky bit e os
// arquivos sao criados por root, entao o app (uid 5410) nao consegue remove-los.
// Enquanto isso nao foi visto, cada pedido era reprocessado a cada quadro —
// uma unica tecla "down" virava centenas e o foco corria ate o fim da pagina.
static void consume(const char *path) {
  FILE *f = fopen(path, "w");
  if (f) fclose(f);
}

// O pedido e NOVO? Guarda contra o arquivo que nao da para consumir.
//
// `consome` esvazia abrindo com "w" em vez de apagar, justamente por causa do
// sticky bit do /tmp. Mas isso tambem falha quando o arquivo pertence a OUTRO
// usuario: um pedido criado por ssh como root fica 644, e o app (uid 5152) nao
// pode nem apagar nem truncar. O pedido entao vale para sempre.
//
// MEDIDO na TV do dono, e fui eu que causei: um /tmp/nuvio-shot-req esquecido
// como root fez o app capturar a tela inteira (glReadPixels de 1920x1080 mais
// 8 MB gravados) EM TODO QUADRO por horas — `aux` foi de 0,0 para 100,7 ms e o
// app caiu de 60 para 9 fps. O sintoma que chegou foi "a interface ta lerda".
//
// Comparar a data de modificacao resolve sem depender de escrita: um pedido que
// nao mudou desde o ultimo atendimento nao e um pedido novo.
// A data so e consultada quando o consumo FALHA, e nao sempre: ela tem
// resolucao de um segundo, e duas rajadas de tecla no mesmo segundo seriam
// tratadas como a mesma. No caminho normal (arquivo do proprio app) o consumo
// funciona e nada disto entra em jogo.
static int requestNew(const char *path, time_t *blocked) {
  struct stat st;
  if (stat(path, &st) != 0 || st.st_size <= 0) return 0;
  // Pedido que ja foi atendido e nao pode ser esvaziado: ignora enquanto nao
  // mudar. Sem isto ele vale para sempre e o trabalho e refeito por quadro.
  if (*blocked && st.st_mtime == *blocked) return 0;
  *blocked = 0;
  return 1;
}

// Esvazia e confere. Devolve 0 quando NAO conseguiu — dono diferente, sticky
// bit — e nesse caso marca o pedido para ser ignorado ate a data mudar.
static int consumeOuBlocks(const char *path, time_t *blocked) {
  struct stat st;
  consume(path);
  if (stat(path, &st) == 0 && st.st_size > 0) {
    *blocked = st.st_mtime;
    printf("[main] %s cannot be consumed (different owner); ignoring\n",
           path);
    fflush(stdout);
    return 0;
  }
  return 1;
}
// stat() e nao fopen+fseek: esta sondagem roda para TRES arquivos em TODO
// quadro, e cada fopen paga alocacao de FILE e dois syscalls a mais so para
// descobrir o tamanho. O stat responde a mesma pergunta com um syscall.
static long sizeOf(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

// KEYUP adiado de uma tecla segurada.
static Uint32 releaseIn = 0;
static SDL_Keycode releaseKey = 0;

static void keysInjected(void (*deliver)(const SDL_Event *)) {
  if (releaseIn && SDL_GetTicks() >= releaseIn) {
    SDL_Event up; SDL_zero(up);
    up.type = SDL_KEYUP; up.key.keysym.sym = releaseKey;
    deliver(&up);
    releaseIn = 0;
  }
  static time_t blockedKey;
  if (!requestNew("/tmp/nuvio-key", &blockedKey)) return;
  FILE *f = fopen("/tmp/nuvio-key", "r");
  if (!f) return;
  char line[32];
  while (fgets(line, sizeof line, f)) {
    char *end = line + strlen(line);
    while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
    // "ok:hold" simula a pressao longa: o KEYUP dela fica agendado para depois
    // do limiar, em vez de vir junto. Sem isso nao da para exercitar por aqui
    // nada que dependa de segurar o botao.
    int hold = 0;
    char *dp = strchr(line, ':');
    if (dp && !strcmp(dp + 1, "hold")) { *dp = 0; hold = 1; }

    SDL_Keycode k = codeOfKey(line);
    if (!k) continue;
    SDL_Event e; SDL_zero(e);
    e.type = SDL_KEYDOWN; e.key.keysym.sym = k;
    deliver(&e);

    // O par KEYUP existe porque parte da interface so decide quando a tecla
    // SOBE — o toque curto contra a pressao longa do OK, por exemplo. Mandar
    // so o KEYDOWN deixava essas acoes mudas.
    if (hold) { releaseIn = SDL_GetTicks() + NV_HOLD_MS + 120; releaseKey = k; }
    else { e.type = SDL_KEYUP; deliver(&e); }
  }
  fclose(f);
  consumeOuBlocks("/tmp/nuvio-key", &blockedKey);
}

// Tamanho do buffer de onde a captura le. Definido no arranque, junto com o
// viewport.
static int capW = (int)NV_TELA_W, capH = (int)NV_TELA_H;

// Mesmo protocolo das outras ferramentas: escreva uma URL em /tmp/nuvio-video e
// o app toca. E o unico jeito de testar reproducao sem alguem no sofa — e o
// video nao pode ser conferido por captura, porque vive em outro plano.
static void videoIfRequested(void) {
  static time_t blocked;
  char url[1024];
  FILE *f;
  if (!requestNew("/tmp/nuvio-video", &blocked)) return;
  f = fopen("/tmp/nuvio-video", "r");
  if (!f) return;
  if (fgets(url, sizeof url, f)) {
    char *end = url + strlen(url);
    while (end > url && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
    printf("[video] request: %s\n", url);
    fflush(stdout);
    if (url[0] == '-') video_stop();
    else { video_play(url); video_window(0, 0, 1920, 1080); }
  }
  fclose(f);
  consumeOuBlocks("/tmp/nuvio-video", &blocked);
}

static void captureIfRequested(void) {
  static time_t blocked;
  if (!requestNew("/tmp/nuvio-shot-req", &blocked)) return;
  consumeOuBlocks("/tmp/nuvio-shot-req", &blocked);

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

  unsigned int size = 54 + (unsigned int)n;
  unsigned char header[54] = {0};
  header[0] = 'B'; header[1] = 'M';
  header[2] = size & 255; header[3] = (size >> 8) & 255; header[4] = (size >> 16) & 255; header[5] = (size >> 24) & 255;
  header[10] = 54; header[14] = 40;
  header[18] = w & 255; header[19] = (w >> 8) & 255;
  // altura POSITIVA = linhas de baixo para cima, que e exatamente a ordem em
  // que o glReadPixels devolve. Assim nao ha inversao a fazer.
  header[22] = h & 255; header[23] = (h >> 8) & 255;
  header[26] = 1; header[28] = 32;
  header[34] = n & 255; header[35] = (n >> 8) & 255; header[36] = (n >> 16) & 255; header[37] = (n >> 24) & 255;

  // grava num temporario e so entao renomeia: quem le nunca pega arquivo pela metade
  FILE *f = fopen("/tmp/.nuvio-shot.tmp", "wb");
  if (f) {
    fwrite(header, 1, 54, f);
    fwrite(px, 1, n, f);
    fclose(f);
    rename("/tmp/.nuvio-shot.tmp", "/tmp/nuvio-shot.bmp");
    printf("capture: /tmp/nuvio-shot.bmp (%u bytes)\n", size);
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
  const char *dirArt = NULL;
  if (argc > 1 && argv[1][0] != '{') dirArt = argv[1];
  if (!dirArt) {
    char *base = SDL_GetBasePath();
    if (base) { snprintf(dirBuf, sizeof dirBuf, "%sart", base); SDL_free(base); dirArt = dirBuf; }
    else dirArt = "/tmp/art";
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
  if (!win) { printf("window: %s\n", SDL_GetError()); return 1; }
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
      void **fields = (void **)(infoBuf + 8);
      void *sup = fields[1];
      void *wl = dlopen("libwayland-client.so.0", RTLD_NOW);
      void (*marshal)(void *, unsigned, ...) =
          wl ? (void (*)(void *, unsigned, ...))dlsym(wl, "wl_proxy_marshal") : NULL;
      printf("syswm sub=%d display=%p surface=%p\n", sub, fields[0], sup);
      // Opcode 4 de wl_surface e set_opaque_region; NULL = "nada e opaco".
      // Sem commit de proposito: o commit vem do proximo SwapWindow.
      if (marshal && sup) { marshal(sup, 4, NULL); printf("non-opaque surface\n"); }
      else printf("no wayland: video will not appear\n");
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
  printf("window=%dx%d drawable=%dx%d\n", jw, jh, dw, dh);
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
           a > 0 ? "" : "  <<< NO ALPHA: video has no way to show through"); }

  // Em tela retina o drawable e maior que a janela; sem ajustar o viewport, o
  // desenho ocupa um quarto da tela.
  SDL_GL_GetDrawableSize(win, &dw, &dh);
  glViewport(0, 0, dw, dh);
  gfx_size_target(dw, dh);
  capW = dw; capH = dh;

  // O relogio dos marcos comeca AQUI e nao no topo do main: o que vem antes e
  // parse de argumento e SDL_Init, que nao dependem de nada nosso.
  mark_start();
  // ANTES de tex_iniciar e de app_iniciar, que sao quem cria os fios de rede.
  net_prepare();
  mark("gfx_start");
  if (!gfx_start()) return 1;
  // fonts/ fica ao lado de art/: derruba o ultimo componente do caminho da arte
  char dirRec[512];
  snprintf(dirRec, sizeof dirRec, "%s", dirArt);
  char *bar = strrchr(dirRec, '/');
  if (bar) *bar = 0;
  txt_start(dirRec, (float)dw / NV_TELA_W);
  // A MESMA escala vai para o cache de texturas: e ela que decide o teto de
  // decodificacao de cada arte a partir da largura com que o card a desenha.
  // Sem isto todo card decodificava com o teto unico de 640 e o cache batia no
  // orcamento com ~40 texturas.
  tex_scale((float)dw / NV_TELA_W);
  mark("fonts+tex ready");
  // 192 slots, nao 96. O teto de slots so faz sentido junto com o tamanho de
  // cada textura: com o teto unico de 640 cada uma custava 2,4 MB e 96 slots ja
  // estouravam o orcamento de 96 MB (medido: `texturas=40 pend=32 92.3MB` com a
  // home rolando — o cache despejava o que ainda estava na tela). Com o teto
  // por uso a mesma arte custa ~500 KB na TV, e 192 slots cabem com folga.
  //
  // Isso tambem dobra o teto de itens EM VOO, que e nMax/3 em slotLivre: a
  // fileira que entra na tela pede tudo de uma vez em vez de pedir aos poucos.
  tex_start(192);
  // A conta vem ANTES da UI: app_iniciar decide entre abrir na home e abrir no
  // login, e para decidir ele precisa saber se ha sessao gravada.
  data_start(dirArt);
  cloud_configure(dirArt);
  session_start();
  profiles_load_active();
  // Vinculos feitos NESTA TV. Vem antes de trakt_carregar (que le o arquivo do
  // pacote) para o vinculo do usuario ganhar do arquivo de quem montou — e num
  // pacote distribuivel esse arquivo nem existe.
  traktauth_load();
  simklauth_load();
  if (!app_start(dirArt)) return 1;
  // Progresso e dado DO USUARIO: sai da pasta do pacote, que e a mesma para
  // todo mundo que usar o aparelho, e passa para a pasta da instalacao.
  if (data_dir()[0]) cat_dir_writing(data_dir());
  // A configuracao de addons mora junto da arte. Ausente, o app segue com a
  // lista de exemplo — nunca fica sem nada para mostrar.
  addons_load(dirArt);
  // Ajustes tambem sao do USUARIO, nao do pacote.
  settings_dir(data_dir()[0] ? data_dir() : dirArt);
  { // As imagens vindas de URL ficam ao lado da arte do pacote. Uma vez
    // baixadas valem para sempre: arte de filme nao muda.
    char c[600];
    snprintf(c, sizeof c, "%s/cache", dirArt);
    tex_cache_dir(c); }
  // Os icones da interface saem de art/icones (SVG do app web rasterizados).
  gfx_icons_dir(dirArt);
  // Catalogo da rede. O do pacote ja esta carregado e continua na tela ate a
  // resposta chegar — abrir vazio enquanto busca seria pior que mostrar o de
  // ontem por dois segundos.
  trakt_load(dirArt);
  disc_tmdb(dirArt);
  disc_start();
  // Metade da resolucao: o snapshot so aparece escurecido e nas bordas.
  int temSnap = gfx_snap_start((int)NV_TELA_W / 2, (int)NV_TELA_H / 2);
  int snapValid = 0;
  // Alvo minusculo de proposito: e ele esticado que vira o desfoque do fundo.
  // 480x270: com o gaussiano de duas passadas, o que importa nao e o alvo ser
  // minusculo (isso e que produzia blocos ao esticar) e sim o desfoque ser de
  // verdade. Esticado 4x, nenhuma borda de texel aparece.
  gfx_blur_start(480, 270);

  Uint32 lastReport = SDL_GetTicks();
  double txtMsFrame = 0, worstTxtMs = 0;
  int    txtNFrame = 0, worstTxtN = 0;
  int frames = 0, janks = 0; double worst = 0;

  // TELEMETRIA POR FASE. O quadro pior custava 22ms num alvo de 20ms e nao
  // havia como saber ONDE. Os relogios sao de CPU (SDL_GetPerformanceCounter)
  // e NAO ha glFinish em lugar nenhum: glFinish esconde o jank, porque
  // distribui o custo de GPU igualmente por todos os quadros em vez de deixar
  // o atraso aparecer onde ele nasce. Aqui, `des` e o custo de SUBMETER o
  // desenho (CPU) e `swap` absorve a espera do vsync MAIS o que a GPU ainda
  // devia — um quadro pesado de GPU aparece como swap grande, um quadro pesado
  // de CPU aparece na fase que o causou.
  double perFreq = (double)SDL_GetPerformanceFrequency();
  Uint64 lastFrame = SDL_GetPerformanceCounter();
  double fEv=0, fPump=0, fUpd=0, fDes=0, fSwap=0, fAux=0, fColor=0;
  double pEv=0, pPump=0, pUpd=0, pDes=0, pSwap=0, pAux=0, pColor=0;
  // Dentro de `des`: quanto e travessia de GL e quanto e busca no cache.
  double fFill=0, pFill=0; int fNFull=0, pNFull=0;
  double fGfxMs=0, fTexMs=0, fOutMs=0; int fNRect=0, fNProgress=0, fNBind=0, fNSearch=0, fNOut=0;
  double pGfxMs=0, pTexMs=0, pOutMs=0; int pNRect=0, pNProgress=0, pNBind=0, pNSearch=0, pNOut=0;
#define NV_T0() (SDL_GetPerformanceCounter())
#define NV_DT(a) ((SDL_GetPerformanceCounter() - (a)) * 1000.0 / perFreq)

  while (!app_wants_exit()) {
    SDL_Event e;
    Uint64 tEv = NV_T0();
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
        app_event(&back);
        continue;
      }
      app_event(&e);
    }
    keysInjected(app_event);
    fEv = NV_DT(tEv);

    Uint32 now = SDL_GetTicks();
    // dt VEM DO RELOGIO DE ALTA RESOLUCAO, nao de SDL_GetTicks.
    //
    // SDL_GetTicks conta em MILISSEGUNDOS INTEIROS. No Mac o app roda sem vsync
    // a ~1300 fps, entao quase todo quadro dura menos de 1 ms e a subtracao dava
    // ZERO — e o piso `if (dt <= 0) dt = 1/60` entregava 16,7 ms SINTETICOS para
    // um quadro de 0,8 ms de relogio real. Toda animacao avancava ~20x mais
    // rapido que o relogio: medido, um fade de 330 ms terminava em 92 ms.
    //
    // Na TV o vsync escondia o defeito (dt real, sempre >= 20 ms), mas o efeito
    // pratico era pior que um bug de Mac: QUALQUER calibracao de animacao feita
    // na previa perseguia um numero que a TV nunca ia reproduzir, e a medida de
    // pior quadro no Mac tambem saia distorcida.
    //
    // O clamp continua, mas so como TETO: voltar de suspensao entrega um dt de
    // varios segundos e uma animacao daria um salto. Piso nao existe mais —
    // quadro curto tem de ser um dt curto.
    Uint64 cFrame = SDL_GetPerformanceCounter();
    double dtms = (double)(cFrame - lastFrame) * 1000.0 / perFreq;
    lastFrame = cFrame;
    if (dtms > 100.0) dtms = 100.0;
    if (dtms < 0.0) dtms = 0.0;
    float dt = (float)(dtms / 1000.0);
    if (frames > 20) {
      if (dtms > worst) { worst = dtms; worstTxtMs = txtMsFrame; worstTxtN = txtNFrame;
                         pEv=fEv; pPump=fPump; pUpd=fUpd; pDes=fDes; pSwap=fSwap; pAux=fAux; pColor=fColor;
                         pGfxMs=fGfxMs; pTexMs=fTexMs; pNRect=fNRect; pNProgress=fNProgress;
                         pNBind=fNBind; pNSearch=fNSearch; pOutMs=fOutMs; pNOut=fNOut; pFill=fFill; pNFull=fNFull; }
      if (dtms > 33.0) janks++;
    }
    // zera os contadores do quadro que comeca agora; o que foi medido acima
    // pertence ao quadro anterior, que e o que acabou de custar dtms
    txtMsFrame = txt_ms; txtNFrame = txt_rasterized;
    txt_ms = 0.0; txt_rasterized = 0;

    // TRES por quadro. O limite de 1 vinha de quando TODA arte era decodificada
    // com o teto unico de 640: cada glTexImage2D custava ~2 MB e dois no mesmo
    // quadro passavam de 20 ms, aparecendo como tranco ao entrar numa fileira.
    //
    // Esse argumento caiu junto com o teto unico: agora cada arte e decodificada
    // pela largura com que e desenhada (tex_obter_larg), e na TV um poster sai a
    // ~500 KB em vez de 2,4 MB. Tres envios pequenos somam menos que o UNICO
    // envio grande de antes, e a fileira que entra na tela deixa de aparecer aos
    // pedacos.
    Uint64 t0 = NV_T0();
    tex_pump(3);
    fPump = NV_DT(t0);
    t0 = NV_T0();
    app_update(dt, now);
    fUpd = NV_DT(t0);

    // RECORTE DESLIGADO ANTES DO CLEAR. glClear respeita o scissor test: se
    // qualquer tela terminar o quadro com um recorte ativo, o clear seguinte
    // limpa SO aquele retangulo e o resto da tela guarda o quadro anterior.
    // Hoje todos os chamadores equilibram recorte/sem_recorte, mas isso e uma
    // invariante que ninguem verifica — e o sintoma seria justamente uma faixa
    // com conteudo velho, dificil de atribuir a causa. Uma chamada por quadro.
    t0 = NV_T0();
    gfx_new_frame();
    tex_new_frame();
    gfx_sem_crop();
    glClearColor(NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    fColor = NV_DT(t0);
    t0 = NV_T0();
    txt_new_frame();
    app_draw(now);
    fDes = NV_DT(t0);
    fGfxMs = gfx_ms_rect; fTexMs = tex_ms_search;
    fNRect = gfx_n_rect; fNProgress = gfx_n_progress; fNBind = gfx_n_bind; fNSearch = tex_n_search;
    fOutMs = gfx_ms_others; fNOut = gfx_n_others;
    fFill = gfx_fill; fNFull = gfx_n_full;
    t0 = NV_T0();
    videoIfRequested();
    captureIfRequested();
    fAux = NV_DT(t0);
    t0 = NV_T0();
    SDL_GL_SwapWindow(win);
    fSwap = NV_DT(t0);
    // PRIMEIRO PIXEL. E o numero que responde "quanto tempo ate a TV mostrar
    // alguma coisa", que nenhuma metrica de quadro dava.
    //
    // Bandeira PROPRIA e nao `if (!quadros)`: `quadros` zera a cada relatorio
    // de 3 s, entao aquilo carimbaria "primeiro quadro" tres vezes por minuto.
    { static int jaStamped;
      if (!jaStamped) { jaStamped = 1; mark("first frame on screen"); } }
    frames++;

    if (now - lastReport >= 3000) {
      int items, pending; long bytes;
      tex_stats(&items, &pending, &bytes);
      printf("FPS=%.1f worst=%.1fms janks=%d | worst frame: text %.1fms in %d lines"
             " | textures=%d pending=%d %.1fMB | evictions=%d\n",
             frames * 1000.0 / (double)(now - lastReport), worst, janks,
             worstTxtMs, worstTxtN, items, pending, bytes / 1048576.0, txt_evictions);
      fflush(stdout);
      // A MESMA linha vai para um arquivo. No aparelho a saida padrao do app
      // lancado pelo applicationManager nao chega a lugar nenhum que se possa
      // ler, e rodar o binario a mao nao funciona (sem a identidade do app o
      // compositor recusa a superficie e ele morre em silencio). Sem isto nao
      // ha como MEDIR quadro no aparelho — so olhar e achar.
      { FILE *fp = fopen("/tmp/nuvio-fps.txt", "w");
        if (fp) {
          fprintf(fp, "drawable=%dx%d FPS=%.1f worst=%.1fms janks=%d"
                  " text=%.1fms/%d textures=%d %.1fMB"
                  " | worst: ev=%.1f pump=%.1f upd=%.1f clr=%.1f draw=%.1f aux=%.1f swap=%.1f"
                  " | des: gfx=%.1f/%d(p%d,b%d) tex=%.2f/%d out=%.1f/%d fill=%.2fx(cheias=%d)"
                  " | evictions=%d\n",
                  dw, dh,
                  frames * 1000.0 / (double)(now - lastReport), worst, janks,
                  worstTxtMs, worstTxtN, items, bytes / 1048576.0,
                  pEv, pPump, pUpd, pColor, pDes, pAux, pSwap,
                  pGfxMs, pNRect, pNProgress, pNBind, pTexMs, pNSearch, pOutMs, pNOut, pFill, pNFull,
                  txt_evictions);
          fclose(fp);
        } }
      frames = 0; lastReport = now; worst = 0; janks = 0; worstTxtMs = 0; worstTxtN = 0;
      txt_evictions = 0;
      pEv=pPump=pUpd=pDes=pSwap=pAux=pColor=0;
      pGfxMs=pTexMs=pOutMs=0; pNRect=pNProgress=pNBind=pNSearch=pNOut=0; pFill=0; pNFull=0;
    }
  }

  gfx_blur_shutdown();
  gfx_snap_shutdown();
  app_shutdown();
  tex_shutdown();
  txt_shutdown();
  gfx_shutdown();
  SDL_GL_DeleteContext(ctx);
  SDL_DestroyWindow(win);
  IMG_Quit();
  SDL_Quit();
  printf("fim\n");
  return 0;
}
