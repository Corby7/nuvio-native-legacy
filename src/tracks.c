#include "tracks.h"
#include "player.h"
#include "video.h"
#include "addons.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include "subtitle.h"
#include <stdio.h>
#include <string.h>

// 1400 e nao 1180: com a terceira coluna, "Muito pequena" e "Escuro 100%" nao
// cabiam no espaco do valor e saiam cortados. A folha de AUDIO, que tem uma
// coluna so, usa uma fracao disto — ver faixas_desenhar.
#define FX_WIDTH   1400.0f
#define FX_LINE   106.0f


// 3 e nao 2: a folha de legenda tem a LISTA e o ESTILO, e FX_COL_ESTILO e o
// indice 2. Com dois slots a coluna de estilo escrevia fora do vetor.
static int is_open, column, focus[3];
// ROLAGEM POR COLUNA, em LINHAS (nao em pixels): a folha desenhava todas as
// faixas a partir do topo e o painel tem altura limitada — com muitas legendas
// as ultimas caiam fora do painel e da tela. O foco chegava nelas, os olhos
// nao. Guardar quantas linhas foram roladas e o suficiente porque a altura da
// linha e fixa.
static int scroll[3];
// Quantas linhas cabem no painel. Calculada no desenho (depende da altura
// escolhida ali) e lida pelo tratamento de tecla, que roda antes.
static int visible = 8;
static void adjustScroll(void);
static float anim;
// Qual legenda EXTERNA (OpenSubtitles) esta valendo, em indice da lista
// combinada — ou -1 quando a ativa e embutida ou nao ha nenhuma.
//
// Isto vive aqui e nao no video.c porque o pipeline nao devolve essa
// informacao: video_legenda_externa manda o setSubtitleSource com a URL e o
// legAtual do video.c fica intocado, apontando para a legenda EMBUTIDA de
// antes. Sem esta variavel, escolher uma legenda do OpenSubtitles fazia a
// marca de "ativa" ficar em outra linha (ou em "Desativada") e a folha
// reabria com o foco no lugar errado — a legenda certa tocava, so a folha
// mentia sobre qual era.
static int subExternal = -1;

// Chamada quando uma sessao de reproducao nova comeca: a legenda externa e da
// sessao, nao do aparelho. Sem isto o titulo seguinte abriria a folha marcando
// como ativa uma legenda que nao foi escolhida para ele.
void tracks_reset(void) { subExternal = -1; is_open = 0; subtitle_off(); }

// FOLHAS SEPARADAS: 0 = so AUDIO, 1 = LEGENDA (lista + estilo).
//
// Elas eram UMA folha com as duas colunas lado a lado, por decisao minha: "duas
// telas obrigariam a sair e voltar para conferir o par". O dono pediu separado,
// e a referencia lhe da razao — a TCL tem um overlay proprio de legenda
// (SubtitleSelectionOverlay), com o seletor de estilo dentro dele. Comparar o
// par audio+legenda ao mesmo tempo era um caso que eu supus e ninguem pediu.
static int mode;
// Coluna dentro da folha de LEGENDA: 0 = lista, 1 = estilo.
#define FX_COL_STYLE 2
#define FX_N_STYLE   9

static int nLines(int col);

void tracks_open(void) { tracks_open_em(0); }

// Abre JA NA COLUNA que o botao pediu. O player tem um icone de audio e um de
// legenda, e os dois abriam esta folha do mesmo jeito, com o foco no audio:
// apertar "legendas" e cair no audio faz os dois botoes parecerem o mesmo
// botao — foi exatamente o que o dono relatou. O painel continua sendo UM so,
// com as duas colunas lado a lado (comparar o par escolhido e o motivo dele
// existir); o que muda e onde o foco comeca.
void tracks_open_em(int col) {
  int n;
  is_open = 1;
  mode = (col == 1) ? 1 : 0;
  column = mode;                 // audio -> col 0; legenda -> col 1
  focus[0] = video_audio_current();
  // A legenda pode estar desligada (-1); a primeira linha da coluna e sempre
  // "Desativada", entao o indice da lista e deslocado em um.
  focus[1] = (subExternal >= 0 ? subExternal : video_subtitle_current()) + 1;
  // Clamp nas duas colunas. A lista de legendas CRESCE durante a sessao (as do
  // OpenSubtitles chegam depois) e a de audio so existe apos o sourceInfo:
  // guardar um indice de antes e reabrir sem conferir poe o foco fora do vetor.
  { int c; for (c = 0; c < 3; c++) {
      n = nLines(c);
      if (focus[c] >= n) focus[c] = n > 0 ? n - 1 : 0;
      if (focus[c] < 0)  focus[c] = 0;
    scroll[c] = 0;
    } }
}

int tracks_is_open(void) { return is_open; }

static int nSubtitles(void) {
  int n = video_n_subtitle() + addons_n_subtitles();
  return n;
}

static int nLines(int col) {
  if (col == FX_COL_STYLE) return FX_N_STYLE;
  if (col == 0) { int n = video_n_audio(); return n; }
  return nSubtitles() + 1;   // +1 pela linha "Desativada"
}

// --- COLUNA DE ESTILO --------------------------------------------------------
//
// Oito linhas "rotulo: valor". OK cicla o valor e aplica NA HORA — as
// personalizacoes abaixo usam somente metodos presentes no firmware. A ultima
// linha restaura o conjunto inteiro sem exigir dezenas de toques no controle.
static const char *const ST_ROT[FX_N_STYLE] = {
  "Size", "OpenSubtitles source", "Cor", "Opacity", "Background", "Position", "Border", "Delay",
  "Restore default"
};
static const char *const ST_BACKGROUND[5] = { "None", "Dark 25%", "Dark 50%",
                                          "Dark 75%", "Dark 100%" };
static const char *const ST_BORDER[3] = { "None", "Contorno", "Shadow" };
static const char *const ST_OPACITY[4]  = { "100%", "75%", "50%", "25%" };

static void valueStyle(int line, char *dst, size_t size) {
  const VideoSubtitleStyle *e = player_sub_style();
  switch (line) {
    case 0: snprintf(dst, size, "%d%%", e->size); break;
    case 1: snprintf(dst, size, "%s", TXT_FAMILIES_PT[e->family >= 0 && e->family < TXT_FAMILY_N ? e->family : 0]); break;
    case 2: snprintf(dst, size, "%s", VIDEO_SUB_COLORS_PT[e->color % VIDEO_SUB_NCORES]); break;
    case 3: snprintf(dst, size, "%s", ST_OPACITY[e->opacity > 3 ? 3 : e->opacity]); break;
    case 4: snprintf(dst, size, "%s", ST_BACKGROUND[e->background > 4 ? 4 : e->background]); break;
    // O uMS aceita -3..4; a folha mostra 1..8 porque "posicao -3" nao diz nada
    // a quem esta olhando a tela.
    case 5: snprintf(dst, size, "%d de 8", e->position + 1); break;
    case 6: snprintf(dst, size, "%s", ST_BORDER[e->border > 2 ? 2 : e->border]); break;
    case 7: {
      int a = e->delayMs;
      if (!a) snprintf(dst, size, "0 s");
      else    snprintf(dst, size, "%+.2f s", a / 1000.0f);
      break; }
    default: snprintf(dst, size, "Apply"); break;
  }
}

static void cycleStyle(int line) {
  VideoSubtitleStyle *e = player_sub_style();
  switch (line) {
    case 0: e->size += 10; if (e->size > 200) e->size = 50; break;
    case 1: e->family = (e->family + 1) % TXT_FAMILY_N; break;
    case 2: e->color     = (e->color + 1) % VIDEO_SUB_NCORES; break;
    case 3: e->opacity = (e->opacity + 1) % 4; break;
    case 4: e->background   = (e->background + 1) % 5; break;
    case 5: e->position = (e->position + 1) % 8; break;
    case 6: e->border   = (e->border + 1) % 3; break;
    // -5 s a +5 s de 250 em 250 ms, voltando ao inicio. Passo menor exigiria
    // dezenas de toques para sair do lugar num controle remoto.
    case 7:
      e->delayMs += 250;
      if (e->delayMs > 5000) e->delayMs = -5000;
      break;
    default:
      *e = (VideoSubtitleStyle){ 120, 0, 0, 3, 1, 0, 0, TXT_FAMILY_INTER };
      break;
  }
  player_sub_style_changed();
}

// Rotulo da linha `i` da coluna de legenda. Ate video_n_legenda() sao as
// embutidas; depois vem as do OpenSubtitles.
static const char *labelSubtitle(int i, const char **brand) {
  int emb = video_n_subtitle();
  *brand = NULL;
  if (i < emb) {
    const VideoTrack *f = video_subtitle(i);
    return f ? f->label : "";
  }
  { const Subtitle *l = addons_subtitle(i - emb);
    if (!l) return "";
    *brand = "OpenSubtitles";
    return l->label; }
}

static void apply(void) {
  if (column == 0) {
    video_choose_audio(focus[0]);
  } else {
    int i = focus[1] - 1;
    int emb = video_n_subtitle();
    if (i < 0)        { video_choose_subtitle(-1); subtitle_off(); subExternal = -1; }
    else if (i < emb) { video_choose_subtitle(i);  subtitle_off(); subExternal = -1; }
    else {
      const Subtitle *l = addons_subtitle(i - emb);
      // So marca como ativa se houve o que aplicar: sem a URL o uMS nao recebe
      // nada, e a folha diria "ativa" sobre uma legenda que nunca subiu.
      if (l) {
        /* A fonte e os 16 tamanhos agora sao nossos, nao do firmware webOS. */
        video_choose_subtitle(-1); subtitle_load(l->url); subExternal = i;
      }
    }
  }
}

void tracks_event(const SDL_Event *e) {
  SDL_Keycode k;
  if (!is_open || e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) { is_open = 0; return; }
  // Esquerda/direita andam entre a LISTA e o ESTILO, e so na folha de legenda.
  // Na de audio nao ha para onde ir — antes elas pulavam para a coluna de
  // legenda, que e justamente o que fazia os dois botoes do player parecerem o
  // mesmo botao.
  if (k == SDLK_LEFT)  { if (mode && column == FX_COL_STYLE) column = 1; return; }
  if (k == SDLK_RIGHT) { if (mode && column == 1) column = FX_COL_STYLE; return; }
  if (k == SDLK_UP)    { if (focus[column] > 0) focus[column]--; adjustScroll(); return; }
  if (k == SDLK_DOWN)  { if (focus[column] < nLines(column) - 1) focus[column]++;
                         adjustScroll(); return; }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    // Na coluna de ESTILO o OK CICLA o valor e aplica na hora, sem fechar: o
    // dono precisa VER a legenda mudar para escolher, e fechar a folha a cada
    // toque tiraria a lista de baixo dos olhos dele.
    if (column == FX_COL_STYLE) { cycleStyle(focus[FX_COL_STYLE]); return; }
    apply(); is_open = 0; return;
  }
}

void tracks_update(float dt, Uint32 now) {
  (void)now;
  anim = anim_mola(anim, is_open ? 1.0f : 0.0f, dt, NV_MOLA_SCREEN);
}

// Traz a linha focada para dentro da janela visivel, mexendo o MINIMO: so
// quando o foco passa de uma das bordas. Rolar sempre para centralizar faria a
// lista inteira andar a cada tecla, que num D-pad e desorientador.
static void adjustScroll(void) {
  int n = nLines(column), f = focus[column], *r = &scroll[column];
  if (visible < 1) return;
  if (f < *r) *r = f;
  else if (f >= *r + visible) *r = f - visible + 1;
  if (*r > n - visible) *r = n - visible;
  if (*r < 0) *r = 0;
}

static void column_draw(int col, float x, float width, float y0, float a) {
  const char *title=col==FX_COL_STYLE?"Style":col?"Subtitles":"Audio tracks";
  txt_draw_alpha(txt_line(TXT_PG_LABEL,title,188,190,196,255),x,y0,a);
  int n=nLines(col), r=scroll[col], end=r+visible;
  if(end>n) end=n;
  for(int i=r;i<end;i++) {
    float y=y0+64+(i-r)*FX_LINE;
    int sel=col==column && i==focus[col];
    const char *brand=NULL,*rot;
    char value[48];
    if(col==FX_COL_STYLE) {
      valueStyle(i,value,sizeof value); rot=ST_ROT[i]; brand=value;
    } else if(!col) {
      const VideoTrack *f=video_audio(i);
      rot=f?f->label:""; brand=f?f->language:NULL;
    } else {
      rot=i==0?"None":labelSubtitle(i-1,&brand);
      if(i && !brand) brand="Incorporada";
    }
    if(sel) gfx_color((GfxRect){x-20,y-14,width+20,92},.18f,.95f,.95f,.96f,a);
    int c=sel?25:230, sub=sel?70:174;
    txt_draw_alpha(txt_line_trim(TXT_PANEL_ITEM,rot,c,c,c,255,width-72),x,y,a);
    if(brand && *brand)
      txt_draw_alpha(txt_line_trim(TXT_PG_END,brand,sub,sub,sub,255,width-72),x,y+34,a);
    int active=col==0?i==video_audio_current():
      col==1?(subExternal>=0?i-1==subExternal:i-1==video_subtitle_current()):0;
    if(active) txt_draw_alpha(txt_line(TXT_BODY,"✓",c,c,c,255),x+width-44,y+12,a);
  }
  if(!n) txt_block(TXT_PG_END,"No track available from this source.",178,180,186,x,y0+68,width,28,a,2);
  if(n>visible) {
    char num[48]; snprintf(num,sizeof num,"%d de %d",focus[col]+1,n);
    txt_draw_alpha(txt_line(TXT_MINI,num,174,176,182,255),x,y0+64+visible*FX_LINE,a);
  }
}

void tracks_draw(Uint32 now) {
  (void)now;
  if(anim<.01f) return;
  float a=anim;
  gfx_color((GfxRect){0,0,NV_TELA_W,NV_TELA_H},0,.025f,.025f,.03f,.88f*a);
  txt_draw_alpha(txt_line(TXT_PANEL_TITLE,mode?"Subtitles":"Audio",242,243,245,255),56,48,a);
  txt_draw_alpha(txt_line(TXT_PG_END,"Back to close",180,182,188,255),NV_TELA_W-250,60,a);
  visible=7;
  adjustScroll();
  if(!mode) column_draw(0,76,720,138,a);
  else {
    column_draw(1,76,990,138,a);
    column_draw(FX_COL_STYLE,1190,650,138,a);
  }
}
