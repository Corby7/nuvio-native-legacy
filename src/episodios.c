#include "episodios.h"
#include "catalogo.h"
#include "descoberta.h"
#include "extras.h"
#include "gfx.h"
#include "tex_cache.h"
#include "text.h"
#include "layout.h"
#include "anim.h"
#include <stdio.h>

#define EP_W 720.0f
#define EP_ROW 172.0f
#define EP_TOP 216.0f
static int aberto, titulo, atualT, atualE, temporada, foco, grupo;
static int pedidoT, pedidoE;
static float anim, scroll;
static int localizarAtual;

static int nTemporadas(void) {
  const CatItem *c = cat_item(titulo);
  return c && c->nTemporadas > 0 ? c->nTemporadas : 1;
}
static int numTemporada(int i) {
  const CatItem *c = cat_item(titulo);
  return c && c->nTemporadas > 0 ? c->temporadas[i] : atualT;
}
static const CatEp *epLinha(int linha) {
  int n = cat_n_episodios(titulo);
  for (int i = 0, j = 0; i < n; i++) {
    const CatEp *ep = cat_episodio(titulo, i);
    if (ep && ep->temporada == numTemporada(temporada) && j++ == linha) return ep;
  }
  return NULL;
}
static int nLinhas(void) {
  int n = 0;
  for (int i=0;i<cat_n_episodios(titulo);i++) {
    const CatEp *e=cat_episodio(titulo,i);
    if(e && e->temporada==numTemporada(temporada)) n++;
  }
  return n;
}
void episodios_abrir(int idx, int t, int e) {
  titulo = idx; atualT = t; atualE = e; aberto = 1;
  temporada = foco = 0; grupo = 1; pedidoE = 0; scroll = 0;
  localizarAtual = 1;
  for (int i = 0; i < nTemporadas(); i++) if (numTemporada(i) == t) temporada = i;
  for (int i = 0; i < nLinhas(); i++) if (epLinha(i)->episodio == e) foco = i;
  desc_episodios(titulo, t);
}
int episodios_aberto(void) { return aberto; }
void episodios_fechar(void) { aberto = 0; }
int episodios_escolheu(int *t, int *e) {
  if (!pedidoE) return 0;
  *t = pedidoT; *e = pedidoE; pedidoE = 0; return 1;
}
void episodios_evento(const SDL_Event *ev) {
  if (!aberto || ev->type != SDL_KEYDOWN) return;
  SDL_Keycode k = ev->key.keysym.sym;
  if(k==SDLK_r) { desc_episodios(titulo,numTemporada(temporada)); return; }
  if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_DELETE || k == SDLK_AC_BACK) {
    aberto = 0; return;
  }
  int nt = nTemporadas(), n = nLinhas();
  if (k == SDLK_UP) { if (grupo == 1 && foco > 0) foco--; else grupo--; }
  if (k == SDLK_DOWN) { if (grupo < 1) grupo++; else if (foco < n - 1) foco++; }
  if (grupo < -1) grupo = -1;
  if (grupo == 0 && (k == SDLK_LEFT || k == SDLK_RIGHT)) {
    int nova = temporada + (k == SDLK_RIGHT ? 1 : -1);
    if (nova >= 0 && nova < nt) {
      temporada = nova; foco = 0; scroll = 0;
      localizarAtual = 0;
      desc_episodios(titulo, numTemporada(temporada));
    }
  }
  if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    if (grupo == -1) aberto = 0;
    else if (grupo == 0) {
      grupo = 1;
      if (!n) desc_episodios(titulo,numTemporada(temporada));
    }
    else {
      const CatEp *ep = epLinha(foco);
      if (ep) {
        if (ep->temporada != atualT || ep->episodio != atualE) {
          pedidoT = ep->temporada; pedidoE = ep->episodio;
        }
        aberto = 0;
      }
    }
  }
}
void episodios_atualizar(float dt) {
  anim = anim_mola(anim, aberto ? 1 : 0, dt, NV_MOLA_TELA);
  if(!aberto && anim<.005f) return;
  desc_episodios_pendente();
  int n = nLinhas();
  if(localizarAtual && n) {
    for(int i=0;i<n;i++) if(epLinha(i)->episodio==atualE) foco=i;
    localizarAtual=0;
  }
  if (foco >= n) foco = n > 0 ? n - 1 : 0;
  float area = NV_TELA_H - EP_TOP - 36;
  float max = n * EP_ROW - area;
  float alvo = scroll;
  if(foco*EP_ROW<scroll) alvo=foco*EP_ROW;
  if((foco+1)*EP_ROW>scroll+area) alvo=(foco+1)*EP_ROW-area;
  if (alvo > max) alvo = max;
  if (alvo < 0) alvo = 0;
  scroll = anim_mola(scroll, alvo, dt, NV_MOLA_SCROLL);
}
void episodios_desenhar(void) {
  if (anim < .005f) return;
  float x = NV_TELA_W - EP_W + (1 - anim) * EP_W;
  gfx_cor((GfxRect){0,0,NV_TELA_W,NV_TELA_H},0,.02f,.02f,.025f,.35f*anim);
  gfx_cor((GfxRect){x,0,EP_W,NV_TELA_H},.025f,.095f,.095f,.10f,anim);
  txt_desenhar_alpha(txt_linha(TXT_PAINEL_TITULO,"Episódios",240,241,243,255),x+40,44,anim);
  gfx_cor((GfxRect){x+EP_W-146,44,110,50},.3f,grupo==-1?.94f:.14f,grupo==-1?.94f:.14f,grupo==-1?.95f:.15f,anim);
  int cor = grupo == -1 ? 25 : 230;
  txt_desenhar_alpha(txt_linha(TXT_PG_ROTULO,"Fechar",cor,cor,cor,255),x+EP_W-130,55,anim);
  gfx_recorte(x+36,120,EP_W-72,64);
  int primeira = temporada > 1 ? temporada - 1 : 0;
  for (int i = primeira; i < nTemporadas() && i < primeira+3; i++) {
    float tx = x+40+(i-primeira)*212;
    int sel = i == temporada;
    gfx_cor((GfxRect){tx,126,196,52},.5f,sel?.94f:.14f,sel?.94f:.14f,sel?.95f:.15f,anim);
    char s[48]; snprintf(s,sizeof s,"Temporada %d",numTemporada(i));
    int b=sel?24:210;
    TxtLinha l=txt_linha(TXT_PG_ROTULO,s,b,b,b,255);
    txt_desenhar_alpha(l,tx+(196-l.w)*.5f,138,anim);
    if (sel && grupo==0) gfx_cor((GfxRect){tx+30,184,136,2},0,.94f,.94f,.95f,anim);
  }
  gfx_sem_recorte();
  gfx_recorte(x+36,EP_TOP,EP_W-72,NV_TELA_H-EP_TOP-32);
  int n=nLinhas();
  for (int i=0;i<n;i++) {
    float y=EP_TOP+i*EP_ROW-scroll;
    if (y+EP_ROW<EP_TOP || y>NV_TELA_H-32) continue;
    const CatEp *ep=epLinha(i);
    int sel=grupo==1 && i==foco;
    GfxRect r={x+40,y,EP_W-80,EP_ROW-14};
    if(sel) gfx_cor(r,.13f,.94f,.94f,.95f,anim);
    r.x+=2; r.y+=2; r.w-=4; r.h-=4;
    gfx_cor(r,.12f,.135f,.135f,.14f,anim);
    const CatItem *ci=cat_item(titulo);
    const char *arte=ep->thumb[0]?ep->thumb:(ci?ci->backdrop:"");
    GLuint tex=tex_obter_larg(arte,184);
    GfxRect tr={x+54,y+14,184,130};
    gfx_cor(tr,.10f,.19f,.19f,.20f,anim);
    if(tex){gfx_tex_aspect_atual=tex_aspecto(arte);gfx_rect(tr,tex,GFX_CARD,0,0,0,.10f,0,0,0,anim);gfx_tex_aspect_atual=0;}
    char num[40];snprintf(num,sizeof num,"T%dE%d",ep->temporada,ep->episodio);
    gfx_cor((GfxRect){tr.x+8,tr.y+92,72,30},.15f,.025f,.025f,.03f,.9f*anim);
    txt_desenhar_alpha(txt_linha(TXT_MINI,num,240,240,242,255),tr.x+15,tr.y+97,anim);
    float tx=x+260, w=EP_W-310;
    txt_desenhar_alpha(txt_linha_corta(TXT_PAINEL_ITEM,ep->nome[0]?ep->nome:num,242,243,245,255,w),tx,y+16,anim);
    int atual=ep->temporada==atualT && ep->episodio==atualE;
    int visto=extras_ep_visto(ep->temporada,ep->episodio);
    char estado[96];
    if(atual) snprintf(estado,sizeof estado,"Reproduzindo agora");
    else if(visto) snprintf(estado,sizeof estado,"✓ Assistido%s%s",ep->duracao[0]?" · ":"",ep->duracao);
    else snprintf(estado,sizeof estado,"%s%s%s",ep->data,ep->data[0]&&ep->duracao[0]?" · ":"",ep->duracao);
    txt_desenhar_alpha(txt_linha_corta(TXT_PG_FIM,estado,atual?236:180,atual?237:182,atual?240:188,255,w),tx,y+48,anim);
    txt_bloco(TXT_PG_FIM,ep->sinopse,186,188,194,tx,y+78,w,25,anim,3);
  }
  if(!n) txt_bloco(TXT_PG_FIM,desc_episodios_carregando(titulo)?
    "Carregando episódios…":"Episódios indisponíveis. Selecione a temporada e pressione OK para tentar novamente.",
    196,198,204,x+56,EP_TOP+40,EP_W-112,28,anim,4);
  gfx_sem_recorte();
  if(n) {
    char contador[48];snprintf(contador,sizeof contador,"%d de %d episódios",foco+1,n);
    txt_desenhar_alpha(txt_linha(TXT_MINI,contador,166,168,174,255),x+40,NV_TELA_H-26,anim);
  }
}
