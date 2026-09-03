#include "faixas.h"
#include "player.h"
#include "video.h"
#include "addons.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include "legenda.h"
#include <stdio.h>
#include <string.h>

// 1400 e nao 1180: com a terceira coluna, "Muito pequena" e "Escuro 100%" nao
// cabiam no espaco do valor e saiam cortados. A folha de AUDIO, que tem uma
// coluna so, usa uma fracao disto — ver faixas_desenhar.
#define FX_LARG   1400.0f
#define FX_LINHA   106.0f


// 3 e nao 2: a folha de legenda tem a LISTA e o ESTILO, e FX_COL_ESTILO e o
// indice 2. Com dois slots a coluna de estilo escrevia fora do vetor.
static int aberta, coluna, foco[3];
// ROLAGEM POR COLUNA, em LINHAS (nao em pixels): a folha desenhava todas as
// faixas a partir do topo e o painel tem altura limitada — com muitas legendas
// as ultimas caiam fora do painel e da tela. O foco chegava nelas, os olhos
// nao. Guardar quantas linhas foram roladas e o suficiente porque a altura da
// linha e fixa.
static int rolagem[3];
// Quantas linhas cabem no painel. Calculada no desenho (depende da altura
// escolhida ali) e lida pelo tratamento de tecla, que roda antes.
static int visiveis = 8;
static void ajustarRolagem(void);
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
static int legExterna = -1;

// Chamada quando uma sessao de reproducao nova comeca: a legenda externa e da
// sessao, nao do aparelho. Sem isto o titulo seguinte abriria a folha marcando
// como ativa uma legenda que nao foi escolhida para ele.
void faixas_reiniciar(void) { legExterna = -1; aberta = 0; legenda_desligar(); }

// FOLHAS SEPARADAS: 0 = so AUDIO, 1 = LEGENDA (lista + estilo).
//
// Elas eram UMA folha com as duas colunas lado a lado, por decisao minha: "duas
// telas obrigariam a sair e voltar para conferir o par". O dono pediu separado,
// e a referencia lhe da razao — a TCL tem um overlay proprio de legenda
// (SubtitleSelectionOverlay), com o seletor de estilo dentro dele. Comparar o
// par audio+legenda ao mesmo tempo era um caso que eu supus e ninguem pediu.
static int modo;
// Coluna dentro da folha de LEGENDA: 0 = lista, 1 = estilo.
#define FX_COL_ESTILO 2
#define FX_N_ESTILO   9

static int nLinhas(int col);

void faixas_abrir(void) { faixas_abrir_em(0); }

// Abre JA NA COLUNA que o botao pediu. O player tem um icone de audio e um de
// legenda, e os dois abriam esta folha do mesmo jeito, com o foco no audio:
// apertar "legendas" e cair no audio faz os dois botoes parecerem o mesmo
// botao — foi exatamente o que o dono relatou. O painel continua sendo UM so,
// com as duas colunas lado a lado (comparar o par escolhido e o motivo dele
// existir); o que muda e onde o foco comeca.
void faixas_abrir_em(int col) {
  int n;
  aberta = 1;
  modo = (col == 1) ? 1 : 0;
  coluna = modo;                 // audio -> col 0; legenda -> col 1
  foco[0] = video_audio_atual();
  // A legenda pode estar desligada (-1); a primeira linha da coluna e sempre
  // "Desativada", entao o indice da lista e deslocado em um.
  foco[1] = (legExterna >= 0 ? legExterna : video_legenda_atual()) + 1;
  // Clamp nas duas colunas. A lista de legendas CRESCE durante a sessao (as do
  // OpenSubtitles chegam depois) e a de audio so existe apos o sourceInfo:
  // guardar um indice de antes e reabrir sem conferir poe o foco fora do vetor.
  { int c; for (c = 0; c < 3; c++) {
      n = nLinhas(c);
      if (foco[c] >= n) foco[c] = n > 0 ? n - 1 : 0;
      if (foco[c] < 0)  foco[c] = 0;
    rolagem[c] = 0;
    } }
}

int faixas_aberta(void) { return aberta; }

static int nLegendas(void) {
  int n = video_n_legenda() + addons_n_legendas();
  return n;
}

static int nLinhas(int col) {
  if (col == FX_COL_ESTILO) return FX_N_ESTILO;
  if (col == 0) { int n = video_n_audio(); return n; }
  return nLegendas() + 1;   // +1 pela linha "Desativada"
}

// --- COLUNA DE ESTILO --------------------------------------------------------
//
// Oito linhas "rotulo: valor". OK cicla o valor e aplica NA HORA — as
// personalizacoes abaixo usam somente metodos presentes no firmware. A ultima
// linha restaura o conjunto inteiro sem exigir dezenas de toques no controle.
static const char *const EST_ROT[FX_N_ESTILO] = {
  "Tamanho", "Fonte OpenSubtitles", "Cor", "Opacidade", "Fundo", "Posição", "Borda", "Atraso",
  "Restaurar padrão"
};
static const char *const EST_FUNDO[5] = { "Nenhum", "Escuro 25%", "Escuro 50%",
                                          "Escuro 75%", "Escuro 100%" };
static const char *const EST_BORDA[3] = { "Nenhuma", "Contorno", "Sombra" };
static const char *const EST_OPAC[4]  = { "100%", "75%", "50%", "25%" };

static void valorEstilo(int linha, char *dst, size_t tam) {
  const VideoLegendaEstilo *e = player_leg_estilo();
  switch (linha) {
    case 0: snprintf(dst, tam, "%d%%", e->tamanho); break;
    case 1: snprintf(dst, tam, "%s", TXT_FAMILIAS_PT[e->familia >= 0 && e->familia < TXT_FAMILIA_N ? e->familia : 0]); break;
    case 2: snprintf(dst, tam, "%s", VIDEO_LEG_CORES_PT[e->cor % VIDEO_LEG_NCORES]); break;
    case 3: snprintf(dst, tam, "%s", EST_OPAC[e->opacidade > 3 ? 3 : e->opacidade]); break;
    case 4: snprintf(dst, tam, "%s", EST_FUNDO[e->fundo > 4 ? 4 : e->fundo]); break;
    // O uMS aceita -3..4; a folha mostra 1..8 porque "posicao -3" nao diz nada
    // a quem esta olhando a tela.
    case 5: snprintf(dst, tam, "%d de 8", e->posicao + 1); break;
    case 6: snprintf(dst, tam, "%s", EST_BORDA[e->borda > 2 ? 2 : e->borda]); break;
    case 7: {
      int a = e->atrasoMs;
      if (!a) snprintf(dst, tam, "0 s");
      else    snprintf(dst, tam, "%+.2f s", a / 1000.0f);
      break; }
    default: snprintf(dst, tam, "Aplicar"); break;
  }
}

static void ciclarEstilo(int linha) {
  VideoLegendaEstilo *e = player_leg_estilo();
  switch (linha) {
    case 0: e->tamanho += 10; if (e->tamanho > 200) e->tamanho = 50; break;
    case 1: e->familia = (e->familia + 1) % TXT_FAMILIA_N; break;
    case 2: e->cor     = (e->cor + 1) % VIDEO_LEG_NCORES; break;
    case 3: e->opacidade = (e->opacidade + 1) % 4; break;
    case 4: e->fundo   = (e->fundo + 1) % 5; break;
    case 5: e->posicao = (e->posicao + 1) % 8; break;
    case 6: e->borda   = (e->borda + 1) % 3; break;
    // -5 s a +5 s de 250 em 250 ms, voltando ao inicio. Passo menor exigiria
    // dezenas de toques para sair do lugar num controle remoto.
    case 7:
      e->atrasoMs += 250;
      if (e->atrasoMs > 5000) e->atrasoMs = -5000;
      break;
    default:
      *e = (VideoLegendaEstilo){ 120, 0, 0, 3, 1, 0, 0, TXT_FAMILIA_INTER };
      break;
  }
  player_leg_estilo_mudou();
}

// Rotulo da linha `i` da coluna de legenda. Ate video_n_legenda() sao as
// embutidas; depois vem as do OpenSubtitles.
static const char *rotuloLegenda(int i, const char **marca) {
  int emb = video_n_legenda();
  *marca = NULL;
  if (i < emb) {
    const VideoFaixa *f = video_legenda(i);
    return f ? f->rotulo : "";
  }
  { const Legenda *l = addons_legenda(i - emb);
    if (!l) return "";
    *marca = "OpenSubtitles";
    return l->rotulo; }
}

static void aplicar(void) {
  if (coluna == 0) {
    video_escolher_audio(foco[0]);
  } else {
    int i = foco[1] - 1;
    int emb = video_n_legenda();
    if (i < 0)        { video_escolher_legenda(-1); legenda_desligar(); legExterna = -1; }
    else if (i < emb) { video_escolher_legenda(i);  legenda_desligar(); legExterna = -1; }
    else {
      const Legenda *l = addons_legenda(i - emb);
      // So marca como ativa se houve o que aplicar: sem a URL o uMS nao recebe
      // nada, e a folha diria "ativa" sobre uma legenda que nunca subiu.
      if (l) {
        /* A fonte e os 16 tamanhos agora sao nossos, nao do firmware webOS. */
        video_escolher_legenda(-1); legenda_carregar(l->url); legExterna = i;
      }
    }
  }
}

void faixas_evento(const SDL_Event *e) {
  SDL_Keycode k;
  if (!aberta || e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) { aberta = 0; return; }
  // Esquerda/direita andam entre a LISTA e o ESTILO, e so na folha de legenda.
  // Na de audio nao ha para onde ir — antes elas pulavam para a coluna de
  // legenda, que e justamente o que fazia os dois botoes do player parecerem o
  // mesmo botao.
  if (k == SDLK_LEFT)  { if (modo && coluna == FX_COL_ESTILO) coluna = 1; return; }
  if (k == SDLK_RIGHT) { if (modo && coluna == 1) coluna = FX_COL_ESTILO; return; }
  if (k == SDLK_UP)    { if (foco[coluna] > 0) foco[coluna]--; ajustarRolagem(); return; }
  if (k == SDLK_DOWN)  { if (foco[coluna] < nLinhas(coluna) - 1) foco[coluna]++;
                         ajustarRolagem(); return; }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    // Na coluna de ESTILO o OK CICLA o valor e aplica na hora, sem fechar: o
    // dono precisa VER a legenda mudar para escolher, e fechar a folha a cada
    // toque tiraria a lista de baixo dos olhos dele.
    if (coluna == FX_COL_ESTILO) { ciclarEstilo(foco[FX_COL_ESTILO]); return; }
    aplicar(); aberta = 0; return;
  }
}

void faixas_atualizar(float dt, Uint32 agora) {
  (void)agora;
  anim = anim_mola(anim, aberta ? 1.0f : 0.0f, dt, NV_MOLA_TELA);
}

// Traz a linha focada para dentro da janela visivel, mexendo o MINIMO: so
// quando o foco passa de uma das bordas. Rolar sempre para centralizar faria a
// lista inteira andar a cada tecla, que num D-pad e desorientador.
static void ajustarRolagem(void) {
  int n = nLinhas(coluna), f = foco[coluna], *r = &rolagem[coluna];
  if (visiveis < 1) return;
  if (f < *r) *r = f;
  else if (f >= *r + visiveis) *r = f - visiveis + 1;
  if (*r > n - visiveis) *r = n - visiveis;
  if (*r < 0) *r = 0;
}

static void coluna_desenhar(int col, float x, float larg, float y0, float a) {
  const char *titulo=col==FX_COL_ESTILO?"Estilo":col?"Legendas":"Faixas de áudio";
  txt_desenhar_alpha(txt_linha(TXT_PG_ROTULO,titulo,188,190,196,255),x,y0,a);
  int n=nLinhas(col), r=rolagem[col], fim=r+visiveis;
  if(fim>n) fim=n;
  for(int i=r;i<fim;i++) {
    float y=y0+64+(i-r)*FX_LINHA;
    int sel=col==coluna && i==foco[col];
    const char *marca=NULL,*rot;
    char valor[48];
    if(col==FX_COL_ESTILO) {
      valorEstilo(i,valor,sizeof valor); rot=EST_ROT[i]; marca=valor;
    } else if(!col) {
      const VideoFaixa *f=video_audio(i);
      rot=f?f->rotulo:""; marca=f?f->idioma:NULL;
    } else {
      rot=i==0?"Nenhuma":rotuloLegenda(i-1,&marca);
      if(i && !marca) marca="Incorporada";
    }
    if(sel) gfx_cor((GfxRect){x-20,y-14,larg+20,92},.18f,.95f,.95f,.96f,a);
    int c=sel?25:230, sub=sel?70:174;
    txt_desenhar_alpha(txt_linha_corta(TXT_PAINEL_ITEM,rot,c,c,c,255,larg-72),x,y,a);
    if(marca && *marca)
      txt_desenhar_alpha(txt_linha_corta(TXT_PG_FIM,marca,sub,sub,sub,255,larg-72),x,y+34,a);
    int ativo=col==0?i==video_audio_atual():
      col==1?(legExterna>=0?i-1==legExterna:i-1==video_legenda_atual()):0;
    if(ativo) txt_desenhar_alpha(txt_linha(TXT_BODY,"✓",c,c,c,255),x+larg-44,y+12,a);
  }
  if(!n) txt_bloco(TXT_PG_FIM,"Nenhuma faixa disponível nesta fonte.",178,180,186,x,y0+68,larg,28,a,2);
  if(n>visiveis) {
    char num[48]; snprintf(num,sizeof num,"%d de %d",foco[col]+1,n);
    txt_desenhar_alpha(txt_linha(TXT_MINI,num,174,176,182,255),x,y0+64+visiveis*FX_LINHA,a);
  }
}

void faixas_desenhar(Uint32 agora) {
  (void)agora;
  if(anim<.01f) return;
  float a=anim;
  gfx_cor((GfxRect){0,0,NV_TELA_W,NV_TELA_H},0,.025f,.025f,.03f,.88f*a);
  txt_desenhar_alpha(txt_linha(TXT_PAINEL_TITULO,modo?"Legendas":"Áudio",242,243,245,255),56,48,a);
  txt_desenhar_alpha(txt_linha(TXT_PG_FIM,"Voltar para fechar",180,182,188,255),NV_TELA_W-250,60,a);
  visiveis=7;
  ajustarRolagem();
  if(!modo) coluna_desenhar(0,76,720,138,a);
  else {
    coluna_desenhar(1,76,990,138,a);
    coluna_desenhar(FX_COL_ESTILO,1190,650,138,a);
  }
}
