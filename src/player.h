// Interface de reproducao nativa, seguindo o Nuvio oficial.
// O video e fornecido por video.c na LG; no Mac ha somente a interface.
#ifndef NV_PLAYER_H
#define NV_PLAYER_H

// VideoLegendaEstilo vem daqui: o estilo da legenda e guardado nas
// preferencias do player, mas quem o define e o modulo de video.
#include "video.h"
#include "catalog.h"
#include <SDL2/SDL.h>

// Abre a reproducao do titulo `indiceCatalogo` (indice circular, igual ao do
// catalogo). Titulo, logo, sinopse e arte saem dali.
//
// Com NULL, aguarda a consulta de fontes; nao simula uma reproducao.
void player_open(int indexCatalog, const char *url);
void player_set_episode(int season, int episode);
void player_episode_current(int *season, int *episode);
int player_index(void);
const char *player_line_episode(void);
int player_requested_sources(void);
int player_requested_next(int *season, int *episode);
const CatEp *player_next_episode(void);
void player_error_source(void);

// 1 quando ha video de verdade por tras desta sessao. O desenho usa isto para
// nao pintar a arte-chave por cima do plano de video.
int  player_com_video(void);
int  player_requested_tracks(void);   // CIMA no player abre audio/legendas

// 1 enquanto a fonte abre. A tela mostra a arte-chave e um indicador; sem isso
// o usuario aperta Reproduzir e encara uma tela parada sem saber se funcionou.
int  player_loading(void);
// Exposto para regressao de D-pad: BAIXO na fileira deve fechar a barra.
int  player_controls_visible(void);

// Liga a fonte numa sessao ja aberta. Existe porque o link so pode ser pedido
// no ultimo instante (ver stream_idade_ms), entao a tela abre antes de haver
// URL e o video entra quando chega.
void player_set_source(const char *url);

int  player_is_open(void);   // 1 enquanto a tela existe, inclusive durante o fade de saida
void player_event(const SDL_Event *e);
void player_update(float dt, Uint32 now);
void player_draw(Uint32 now);
int  player_wants_exit(void);  // 1 assim que o Back foi apertado
void player_shutdown(void);

// --- MODOS DE PROPORCAO -----------------------------------------------------
// Os OITO modos do app web, na mesma ordem e com os mesmos fatores
// (js/core/player/playerAspect.js). A ordem importa: e ela que o ciclo percorre,
// e trocar a ordem aqui muda o que o dono encontra ao apertar a tecla.
//
// POR QUE ZOOM, e nao object-fit: a barra preta de um filme widescreen esta
// EMBUTIDA no quadro. Um 2.39:1 entregue como 3840x2160 tem proporcao de quadro
// 1.778 — a mesma da tela — entao "encaixar" e "preencher" dao exatamente a
// mesma imagem e nenhum dos dois corta coisa alguma. Cortar exige AMPLIAR e
// deixar o excesso sair da tela.
//
// Os fatores sao 16/9 dividido pela proporcao do filme, nao numeros escolhidos
// a gosto:  2.35:1 -> 1.32,  2.39:1 -> 1.34,  2.76:1 -> 1.55. O ULTRA existe
// porque o CINEMA (1.34) ainda deixa barra visivel num 2.76:1 — observado na
// TV do dono, nao deduzido.
//
// No nativo o video NAO e um elemento HTML: e um plano de hardware atras da
// superficie GL, posicionado por video_janela(). Entao cada modo vira um
// RETANGULO, e o "excesso que sai da viewport" do web vira um retangulo com
// coordenadas negativas e tamanho maior que a tela.
typedef enum {
  PLR_ASPECT_ORIGINAL = 0,   // "Fit (Original)"  contain, sem zoom  — PADRAO
  PLR_ASPECT_CROP,           // "Crop"            cover
  PLR_ASPECT_STRETCH,        // "Stretch"         fill
  PLR_ASPECT_ZOOM_LIGHT,      // "Slight Zoom"     cover x 1.15
  PLR_ASPECT_ZOOM_CINEMA,    // "Cinema Zoom"     cover x 1.34
  PLR_ASPECT_ZOOM_ULTRA,     // "Ultra Zoom"      contain x 1.55
  PLR_ASPECT_FIT_HEIGHT,     // "Fit Height"      cover
  PLR_ASPECT_FIT_WIDTH,    // "Fit Width"       contain
  PLR_ASPECT_N
} PlrAspect;

// Fatores de zoom, iguais aos do resolveAspectScale do web.
#define PLR_ZOOM_LIGHT    1.15f
#define PLR_ZOOM_CINEMA  1.34f
#define PLR_ZOOM_ULTRA   1.55f
// Quanto tempo o aviso de troca de modo fica na tela. 1400ms e o setTimeout do
// showAspectToast do web.
#define PLR_TOAST_MS     1400u

int         player_aspect(void);              // modo atual (PlrAspecto)
const char *player_aspect_label(int mode);   // "Cinema Zoom", "Encaixar"...
void        player_aspect_set(int mode);  // aplica e grava
void        player_aspect_cycle(void);       // proximo modo + aviso na tela

// ESTILO DA LEGENDA, guardado em art/player.txt junto com o aspecto: e
// preferencia do APARELHO e nao do titulo. A folha de faixas edita a struct e
// chama player_leg_estilo_mudou(), que aplica no pipeline e grava.
VideoSubtitleStyle *player_sub_style(void);
void player_sub_style_changed(void);

#endif
