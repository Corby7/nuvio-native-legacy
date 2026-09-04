#include "streams.h"
#include "badges.h"
#include <pthread.h>
#include "net.h"
#include "gfx.h"
#include "text.h"
#include "anim.h"
#include "layout.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "addons.h"
#include "mark.h"

#define SHEET_W       720.0f
#define SHEET_LINE   228.0f
#define SHEET_TOP    272.0f

static Stream *list;
static int n = 0;
static int current = -1, reload;
static char context[320];
void stream_set_current(int i) { current = i >= 0 && i < n ? i : -1; }
int stream_current(void) { return current; }
void stream_sheet_context(const char *s) { snprintf(context, sizeof context, "%s", s ? s : ""); }
int stream_sheet_reload(void) { int r = reload; reload = 0; return r; }

static int is_open = 0, focus = 0, choice = -1;
static float anim = 0.0f, scroll = 0.0f;
// Linha do realce, em unidades de ITEM (2.4 = entre o terceiro e o quarto). O
// realce escorrega entre as linhas em vez de saltar: com o salto seco a folha
// parecia trocar de conteudo a cada tecla, e num D-pad e a continuidade do
// realce que diz "ainda e a mesma lista, voce so andou".


static const char *containerOf(const Stream *s) {
  if (s->mp4 || strstr(s->url, ".mp4") || strstr(s->label, ".mp4")) return "MP4";
  if (strstr(s->url, ".mkv") || strstr(s->file, ".mkv") || strstr(s->description, ".mkv")) return "MKV";
  if (strstr(s->url, ".m3u8") || strstr(s->label, "HLS")) return "HLS";
  return "FILE";
}

static Uint32 receivedIn;

Uint32 stream_age_ms(void) {
  return receivedIn ? SDL_GetTicks() - receivedIn : 0xFFFFFFFFu;
}

void stream_set_list(const Stream *l, int count) {
  receivedIn = SDL_GetTicks();
  Stream *new = l && count > 0 ? malloc(sizeof(Stream) * (size_t)count) : NULL;
  if (l && count > 0 && !new) return;
  if (new) memcpy(new, l, sizeof(Stream) * (size_t)count);
  free(list); list = new; n = new ? count : 0; current = -1;
  focus = 0;
}

int stream_n(void) {
  return n;
}

const Stream *stream_item(int i) {
  return i >= 0 && i < n ? &list[i] : NULL;
}

// Pontuacao da regra do dono, do mais forte para o mais fraco:
//   MP4 4K Dolby Vision  >  4K Dolby Vision (qualquer container)
//   >  4K  >  Dolby Vision  >  resolucao  >  ordem de chegada
//
// Somar pesos em vez de comparar campo a campo deixa a regra num lugar so e
// legivel: mudar a preferencia e mexer num numero, nao reescrever um encadeado
// de ifs onde a ordem das comparacoes vira a regra escondida.
static long points(const Stream *s) {
  long p = 0;
  // DOLBY VISION SO VALE PONTO EM MP4 — e isto e medida, nao teoria.
  //
  // Marcado no aparelho do dono (LG C9, webOS 4.10) tocando um MKV que o addon
  // anunciava como DV:
  //   hdr do pipeline: HDR10 (fonte DV=1)
  // A TV REBAIXOU para HDR10. E o comportamento ja relatado para Matroska —
  // webOS aciona DV nativo em MP4 e cai para HDR10 em MKV — agora confirmado
  // aqui em vez de citado.
  //
  // O que isso significa na pratica: num perfil 5 a camada base NAO e
  // compativel com HDR10 (e IPT-PQ), entao decodifica-la como HDR10 produz
  // exatamente as cores lavadas que o dono relatou. Preferir a versao DV em
  // MKV era escolher, de proposito, o arquivo que fica PIOR nesta TV.
  //
  // Nao ha como consertar a decodificacao pelo caminho da URI: o Kodi so
  // resolve descartando a camada de realce e reescrevendo o RPU, o que exige
  // demuxar e alimentar o pipeline por buffer — outro projeto, ja registrado em
  // video.c. O que ESTA ao alcance e parar de premiar a fonte que nao serve.
  if (s->mp4 && s->height >= 2160 && s->dolbyVision) p += 100000;
  if (s->height >= 2160)                             p +=  20000;
  if (s->mp4 && s->dolbyVision)                      p +=  10000;
  if (s->dolbyAtmos)                                 p +=   2000;
  p += s->height;
  return p;
}

// Endereco de aviso e nao de conteudo. Estes dois foram MEDIDOS no aparelho:
// o AIOStreams manda para slate.m3u8/slate.mp4 ("This playback link couldn't be
// verified") quando o link expirou, e o Debridio para downloading.mp4 quando o
// arquivo ainda nao esta em cache no Real-Debrid. Os dois sao MP4 validos de
// ~120s que TOCAM NORMALMENTE — nao ha erro para detectar, so o endereco.
static int addressOfWarning(const char *u) {
  return strstr(u, "downloading.mp4") || strstr(u, "/slate") ||
         strstr(u, "slate.mp4") || strstr(u, "slate.m3u8") ? 1 : 0;
}

// VERIFICACAO DAS CANDIDATAS EM PARALELO.
//
// Eram ate 8 rede_url_final EM SERIE, 20 s cada — a segunda metade dos 16,5 s
// medidos entre abrir o titulo e ter fonte. E desperdicio duplo: a maioria das
// tentativas RESOLVE, entao esperar a 1a terminar para so entao comecar a 2a so
// tem valor quando a 1a falha.
//
// A REGRA DE ESCOLHA NAO MUDA: continua sendo "a de maior pontuacao que
// resolve". Os fios verificam as N melhores de uma vez e o resultado e lido NA
// ORDEM DE PONTUACAO, entao a fonte escolhida e exatamente a mesma que a versao
// em serie escolheria — so que sem esperar as anteriores falharem uma a uma.
#define SEE_THREADS 4

typedef struct { int idx; int ok; } Check;
static Check *checks;
static int nChecks, nextCheck;
static pthread_mutex_t seeLock = PTHREAD_MUTEX_INITIALIZER;

static void *threadVerify(void *u) {
  (void)u;
  for (;;) {
    int mine, i;
    char end[900];
    pthread_mutex_lock(&seeLock);
    if (nextCheck >= nChecks) { pthread_mutex_unlock(&seeLock); return NULL; }
    mine = nextCheck++;
    pthread_mutex_unlock(&seeLock);
    i = checks[mine].idx;
    if (!list[i].url[0]) continue;
    // 10 s e nao 20: em paralelo o timeout deixa de ser somado, mas continua
    // sendo o tempo que o dono espera pela mais lenta.
    if (!net_url_final(list[i].url, 10, end, sizeof end)) {
      printf("[source] %d did not resolve\n", i);
      continue;
    }
    if (addressOfWarning(end)) {
      printf("[source] %d is a warning (%.60s)\n", i, end);
      continue;
    }
    checks[mine].ok = 1;
  }
}

int stream_first_good(int attempts) {
  int *used, nu = 0;
  int total = stream_n();
  int chosen = -1;
  if (total < 1) return -1;
  if (attempts < 1) attempts = 1;
  if (attempts > total) attempts = total;
  used = calloc((size_t)attempts, sizeof *used);
  if (!used) return -1;

  // Seleciona as `tentativas` melhores, EM ORDEM DE PONTUACAO — a mesma ordem
  // que o laco em serie percorria.
  while (nu < attempts) {
    int best = -1, i, j;
    long largerP = 0;
    for (i = 0; i < total; i++) {
      int watched = 0;
      for (j = 0; j < nu; j++) if (used[j] == i) { watched = 1; break; }
      if (watched) continue;
      { long p = points(&list[i]);
        if (best < 0 || p > largerP) { best = i; largerP = p; } }
    }
    if (best < 0) break;
    used[nu++] = best;
  }
  if (nu < 1) { free(used); return -1; }

  mark("source: check start");
  checks = calloc((size_t)nu, sizeof(Check));
  if (!checks) { free(used); return -1; }
  { int q;
    for (q = 0; q < nu; q++) checks[q].idx = used[q];
    nChecks = nu; nextCheck = 0;
    { pthread_t threads[SEE_THREADS];
      int created = 0;
      for (q = 0; q < SEE_THREADS && q < nu; q++)
        if (pthread_create(&threads[created], NULL, threadVerify, NULL) == 0) created++;
      if (!created) threadVerify(NULL);   // sem fios: em serie, mesmo resultado
      for (q = 0; q < created; q++) pthread_join(threads[q], NULL);
    }
    // Primeira que passou, na ordem de pontuacao.
    for (q = 0; q < nu; q++)
      if (checks[q].ok) { chosen = checks[q].idx; break; }
  }
  mark(chosen >= 0 ? "source: check ok" : "source: check returned nothing");
  free(checks); checks = NULL; nChecks = 0; free(used);
  if (chosen >= 0) printf("[source] %d ok\n", chosen);
  return chosen;
}

int stream_automatic(void) {
  if (!stream_n()) return -1;
  int best = 0;
  long larger = points(&list[0]);
  for (int i = 1; i < n; i++) {
    long p = points(&list[i]);
    // `>` e nao `>=`: em empate fica o PRIMEIRO da lista, que e a ordem em que
    // o addon devolveu — e ele costuma saber algo que a pontuacao nao ve.
    if (p > larger) { larger = p; best = i; }
  }
  return best;
}


static int group, filter;
static char providers[13][96];
static int nProviders;

static void updateProviders(void) {
  nProviders = 1;
  snprintf(providers[0],sizeof providers[0],"All");
  for (int i=0;i<n;i++) {
    int j;
    for(j=1;j<nProviders;j++) if(!strcmp(providers[j],list[i].provider)) break;
    if(j==nProviders && nProviders<13)
      snprintf(providers[nProviders++],96,"%s",list[i].provider);
  }
  if(filter>=nProviders) filter=0;
}
static int filtered(int line) {
  for(int i=0,j=0;i<n;i++)
    if(!filter || !strcmp(list[i].provider,providers[filter]))
      if(j++==line) return i;
  return -1;
}
static int nFiltered(void) {
  int k=0;
  for(int i=0;i<n;i++) if(!filter || !strcmp(list[i].provider,providers[filter])) k++;
  return k;
}
void stream_sheet_open(void) {
  is_open=1; choice=-1; focus=0; group=1; filter=0; reload=0;
  updateProviders();
  if(current>=0) focus=current;
  scroll=0;
}
int stream_sheet_is_open(void) { return is_open; }
void stream_sheet_event(const SDL_Event *e) {
  if(!is_open || e->type!=SDL_KEYDOWN) return;
  SDL_Keycode k=e->key.keysym.sym;
  if(k==SDLK_ESCAPE || k==SDLK_AC_BACK || k==SDLK_BACKSPACE || k==SDLK_DELETE) {is_open=0;return;}
  if(k==SDLK_r) {reload=1;return;}
  int nf=nFiltered();
  if(k==SDLK_UP) {if(group==1 && focus>0) focus--; else if(group>-1) group--;}
  if(k==SDLK_DOWN) {if(group<1) group++; else if(focus<nf-1) focus++;}
  if(group==0 && (k==SDLK_LEFT || k==SDLK_RIGHT)) {
    filter+=k==SDLK_RIGHT?1:-1;
    if(filter<0) filter=0;
    if(filter>=nProviders) filter=nProviders-1;
    focus=0;scroll=0;
  }
  if(group==-1 && (k==SDLK_LEFT || k==SDLK_RIGHT)) focus=k==SDLK_LEFT?0:1;
  if(k==SDLK_RETURN || k==SDLK_KP_ENTER) {
    if(group==-1) {if(focus==0) reload=1;else is_open=0;}
    else if(group==0) {group=1;focus=0;}
    else {choice=filtered(focus);if(choice>=0) is_open=0;}
  }
}
void stream_sheet_update(float dt, Uint32 now) {
  (void)now;
  anim=anim_spring(anim,is_open?1:0,dt,NV_SPRING_SCREEN);
  updateProviders();
  int nf=nFiltered();
  if(group==1 && focus>=nf) focus=nf>0?nf-1:0;
  float area=NV_SCREEN_H-SHEET_TOP-32;
  float max=nf*SHEET_LINE-area;
  float target=focus*SHEET_LINE-(area-SHEET_LINE)*.5f;
  if(target>max) target=max;
  if(target<0) target=0;
  scroll=anim_spring(scroll,target,dt,NV_SPRING_SCROLL);
}
int stream_sheet_chose(int *out) {
  if(choice<0) return 0;
  if(out) *out=choice;
  choice=-1;return 1;
}
void stream_sheet_draw(Uint32 now) {
  (void)now;
  if(anim<.005f) return;
  float x=NV_SCREEN_W-SHEET_W+(1-anim)*SHEET_W;
  gfx_color((GfxRect){0,0,NV_SCREEN_W,NV_SCREEN_H},0,.02f,.02f,.025f,.35f*anim);
  gfx_color((GfxRect){x,0,SHEET_W,NV_SCREEN_H},.025f,.095f,.095f,.10f,anim);
  txt_draw_alpha(txt_line(TXT_PANEL_TITLE,"Sources",240,241,243,255),x+40,44,anim);
  for(int i=0;i<2;i++) {
    float bx=x+SHEET_W-284+i*128;
    int sel=group==-1 && focus==i;
    gfx_color((GfxRect){bx,44,120,50},.3f,sel?.94f:.14f,sel?.94f:.14f,sel?.95f:.15f,anim);
    int c=sel?24:224;
    TxtLine l=txt_line(TXT_PG_END,i?"Close":"Reload",c,c,c,255);
    txt_draw_alpha(l,bx+(120-l.w)*.5f,58,anim);
  }
  txt_draw_alpha(txt_line_trim(TXT_PG_END,context,184,187,193,255,SHEET_W-80),x+40,126,anim);
  gfx_crop(x+40,180,SHEET_W-80,62);
  int start=filter>1?filter-1:0;
  float tx=x+40;
  for(int i=start;i<nProviders && i<start+3;i++) {
    float w=i?232:108;int sel=i==filter,c=sel?24:202;
    gfx_color((GfxRect){tx,182,w,50},.5f,sel?.94f:.14f,sel?.94f:.14f,sel?.95f:.15f,anim);
    TxtLine l=txt_line_trim(TXT_PG_END,providers[i],c,c,c,255,w-24);
    txt_draw_alpha(l,tx+(w-l.w)*.5f,196,anim);
    if(sel && group==0) gfx_color((GfxRect){tx+16,237,w-32,2},0,.94f,.94f,.95f,anim);
    tx+=w+12;
  }
  gfx_no_crop();
  gfx_crop(x+36,SHEET_TOP,SHEET_W-72,NV_SCREEN_H-SHEET_TOP-32);
  int nf=nFiltered();
  for(int row=0;row<nf;row++) {
    float y=SHEET_TOP+row*SHEET_LINE-scroll;
    if(y+SHEET_LINE<SHEET_TOP || y>NV_SCREEN_H-32) continue;
    int i=filtered(row),sel=group==1 && focus==row;
    const Stream *s=&list[i];
    GfxRect r={x+40,y,SHEET_W-80,SHEET_LINE-14};
    if(sel) gfx_color(r,.10f,.94f,.94f,.95f,anim);
    r.x+=2;r.y+=2;r.w-=4;r.h-=4;
    gfx_color(r,.09f,.135f,.135f,.14f,anim);
    float lx=x+62,w=SHEET_W-124;
    char name[sizeof s->label],description[sizeof s->description];
    snprintf(name,sizeof name,"%s",s->label);snprintf(description,sizeof description,"%s",s->description);
    // SDL_ttf nao interpreta quebras de linha; nao renderizar glifos .notdef.
    for(char *p=name;*p;p++)if((unsigned char)*p<32)*p=' ';
    for(char *p=description;*p;p++)if((unsigned char)*p<32)*p=' ';
    txt_draw_alpha(txt_line_trim(TXT_PANEL_ITEM,name,240,241,243,255,w),lx,y+16,anim);
    txt_draw_alpha(txt_line_trim(TXT_PG_END,i==current?"Now playing":s->provider,175,178,185,255,w),lx,y+46,anim);
    txt_block(TXT_PG_END,description,194,197,202,lx,y+76,w,25,anim,2);
    char meta[192],which[24]="";
    if(s->height) snprintf(which,sizeof which," · %dp",s->height);
    snprintf(meta,sizeof meta,"%s%s%s%s",containerOf(s),which,s->dolbyVision?" · Dolby Vision":"",s->dolbyAtmos?" · Atmos":"");
    if(s->sizeMB) {size_t p=strlen(meta);snprintf(meta+p,sizeof meta-p," · %.1f GB",s->sizeMB/1024.0);}
    txt_draw_alpha(txt_line_trim(TXT_MINI,meta,224,226,232,255,w),lx,y+140,anim);
    badges_draw(s->badges,lx,y+171,w,26,anim);
  }
  if(!nf) {
    const char *s=addons_state()==ADD_SEARCHING?"Fetching sources from the addons…":"No direct source available. Use Reload to try again.";
    txt_block(TXT_PG_END,s,196,199,204,x+56,SHEET_TOP+40,SHEET_W-112,28,anim,3);
  }
  gfx_no_crop();
}
