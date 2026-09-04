// Perfil e Stats: relatorio editorial escuro, pensado para leitura a 3 metros.
//
// A composicao evita uma grade de cards identicos. O resumo e tipografico, o
// streak e um calendario, generos viram uma faixa proporcional e os titulos
// mais vistos ganham paineis largos com arte. O acento violeta pertence aos
// dados, enquanto o foco continua branco como no restante do native legacy.
#include "profile.h"
#include "anim.h"
#include "gfx.h"
#include "layout.h"
#include "text.h"
#include "tex_cache.h"
#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PF_X             settings_content_x()
#define PF_W            (NV_TELA_W-PF_X-104.0f)
#define PF_TOP           62.0f
#define PF_SUMMARY_Y       92.0f
#define PF_HIGHLIGHTS_Y   500.0f
#define PF_ACT_Y       1120.0f
#define PF_GENRES_Y    1660.0f
#define PF_DOC_H        2180.0f
#define PF_SECTIONS          4
#define PF_CARD_W        ((PF_W-PF_CARD_GAP)*.5f)
#define PF_CARD_H        112.0f
#define PF_CARD_GAP       28.0f
#define PF_WARNING_H        54.0f
#define PF_WARNING_Y       (NV_TELA_H-PF_WARNING_H-12.0f)
#define PF_CONTENT_H    (PF_WARNING_Y-12.0f)

static const float SECTION_Y[PF_SECTIONS] = {
  PF_SUMMARY_Y, PF_HIGHLIGHTS_Y, PF_ACT_Y, PF_GENRES_Y
};
static const uint32_t PALETTE[PROFILE_MAX_GENRES] = {
  0xA84BD6, 0x3C9FE8, 0xC57BE3, 0x5CB8F2,
  0x71338E, 0x235B79, 0xD6A1E8, 0x777780
};

static ProfileData data;
static int is_open, sair, loading, temData;
static int temIdentity;
static int section, item, chosen = -1;
static int day, requestUpdate;
static int side, complete, sideFocus;
static char error[160];
#define PF_LOADING PROFILE_STATE_LOADING
#define PF_UPDATING PROFILE_STATE_UPDATING
#define PF_READY PROFILE_STATE_READY
#define PF_STALE PROFILE_STATE_STALE
#define PF_ERROR PROFILE_STATE_ERROR
static ProfileState state=PROFILE_STATE_LOADING;
static float entry, scroll, scrollTarget, velScroll;
static float focusSec[PF_SECTIONS], focusItem[PROFILE_MAX_HIGHLIGHTS];

static int limit(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float fmax0(float v) { return v > 0.0f ? v : 0.0f; }
static void rgb(uint32_t c, float *r, float *g, float *b) {
  *r = ((c >> 16) & 255) / 255.0f;
  *g = ((c >> 8) & 255) / 255.0f;
  *b = (c & 255) / 255.0f;
}
static void text(TxtStyle e, const char *s, int color, float x, float y, float a) {
  txt_draw_alpha(txt_line(e, s ? s : "", color, color, color, 255), x, y - scroll, a);
}
static void textC(TxtStyle e, const char *s, int r, int g, int b,
                   float x, float y, float a) {
  txt_draw_alpha(txt_line(e, s ? s : "", r, g, b, 255), x, y - scroll, a);
}
static void number(char *s, size_t n, int v) { snprintf(s, n, "%d", v < 0 ? 0 : v); }
static void formatDuration(char *s, size_t n, int minutes) {
  if (minutes < 0) minutes = 0;
  if (minutes < 60) snprintf(s, n, "%d min", minutes);
  else snprintf(s, n, "%dh %02dmin", minutes / 60, minutes % 60);
}
static void ring(GfxRect r, float radius, float a) {
  float smaller = r.w < r.h ? r.w : r.h;
  gfx_rect(r, 0, GFX_RING, 0, NV_RING_FOCUS/smaller, 0, radius, 0.96f, 0.96f, 0.98f, a);
}
static int visible(float y, float h) { return y+h-scroll>=0 && y-scroll<NV_TELA_H; }
static void trim(TxtStyle e,const char *s,int c,float x,float y,float w,float a) {
  txt_draw_alpha(txt_line_trim(e,s,c,c,c,255,w),x,y-scroll,a);
}

int profile_start(void) {
  memset(&data, 0, sizeof(data));
  is_open = sair = temData = temIdentity = loading = 0;
  state = PF_LOADING;
  section = item = 0; chosen = -1;
  day = requestUpdate = 0; error[0] = 0;
  entry = scroll = scrollTarget = velScroll = 0;
  memset(focusSec, 0, sizeof(focusSec));
  memset(focusItem, 0, sizeof(focusItem));
  return 1;
}
void profile_shutdown(void) { profile_start(); }
void profile_open(void) {
  side = complete = 0;
  is_open = 1; sair = 0; chosen = -1; section = item = 0;
  scroll = scrollTarget = velScroll = 0;
  requestUpdate = 0;
}
void profile_open_side(void) { profile_open(); side=1; sideFocus=0; }
int profile_side(void) { return side && (is_open || entry>.002f); }
int profile_requested_complete(void) { int v=complete;complete=0;return v; }
void profile_close(void) { is_open = 0; sair = 1; }
int profile_is_open(void) { return is_open; }
int profile_wants_exit(void) { int q = sair; sair = 0; return q; }
void profile_set_loading(int v) {
  loading = !!v;
  if(loading) state=temData?PF_UPDATING:PF_LOADING;
}
void profile_set_error(const char *m) {
  snprintf(error,sizeof(error),"%s",m&&m[0]?m:"Could not update the history.");
  loading=0; state=temData?PF_STALE:PF_ERROR;
}
void profile_set_state(ProfileState new, const char *m) {
  if (m && m[0]) snprintf(error, sizeof error, "%s", m);
  else if (new == PROFILE_STATE_READY || new == PROFILE_STATE_SEM_ACTIVITY) error[0] = 0;
  loading = new == PROFILE_STATE_LOADING || new == PROFILE_STATE_UPDATING;
  state = new;
  if ((new == PROFILE_STATE_PRIVATE || new == PROFILE_STATE_DISCONNECTED || new == PROFILE_STATE_UNAVAILABLE) && temData)
    state = PROFILE_STATE_STALE;
}
ProfileState profile_state(void) { return state; }
int profile_requested_update(void) { int p=requestUpdate; requestUpdate=0; return p; }

void profile_set_data(const ProfileData *d) {
  if (!d) {
    memset(&data,0,sizeof(data));temData=temIdentity=loading=0;
    section=item=day=0;scroll=scrollTarget=velScroll=0;
    chosen=-1;error[0]=0;state=PROFILE_STATE_LOADING;return;
  }
  data = *d;
  // O produtor pode preencher buffers fixos ate o ultimo byte. Fechar todos
  // aqui mantem as chamadas de texto e de textura seguras mesmo com payload
  // truncado vindo da rede.
  data.name[sizeof(data.name)-1] = 0;
  data.user[sizeof(data.user)-1] = 0;
  data.avatar[sizeof(data.avatar)-1] = 0;
  data.period[sizeof(data.period)-1] = 0;
  data.warning[sizeof(data.warning)-1] = 0;
  if(data.minutes<0)data.minutes=0;
  if(data.plays<0)data.plays=0;
  if(data.movies<0)data.movies=0;
  if(data.episodes<0)data.episodes=0;
  // Calendario mensal: no maximo 31 dias, mesmo que o array tenha 42 slots.
  data.nDays = limit(data.nDays, 0, 31);
  data.firstDayWeek = limit(data.firstDayWeek, 0, 6);
  data.nGenres = limit(data.nGenres, 0, PROFILE_MAX_GENRES);
  data.nHighlights = limit(data.nHighlights, 0, PROFILE_MAX_HIGHLIGHTS);
  for (int i=0; i<data.nGenres; i++) {
    data.genres[i].name[sizeof(data.genres[i].name)-1] = 0;
    if(data.genres[i].count<0)data.genres[i].count=0;
  }
  for (int i=0; i<data.nHighlights; i++) {
    ProfileHighlight *p=&data.highlights[i];
    p->id[sizeof(p->id)-1]=0; p->title[sizeof(p->title)-1]=0;
    p->detail[sizeof(p->detail)-1]=0; p->poster[sizeof(p->poster)-1]=0;
    p->backdrop[sizeof(p->backdrop)-1]=0;
  }
  temIdentity = data.name[0] || data.user[0] || data.avatar[0];
  temData = temIdentity || data.minutes > 0 || data.plays > 0 ||
             data.movies > 0 || data.episodes > 0 || data.nHighlights > 0;
  loading = 0;
  error[0]=0; state=temData?(data.plays||data.nHighlights?PF_READY:PROFILE_STATE_SEM_ACTIVITY):PF_ERROR; chosen=-1;
  day=limit(day,0,data.nDays?data.nDays-1:0);
  if(!temData){section=0;scroll=scrollTarget=velScroll=0;}
  if (item >= data.nHighlights) item = data.nHighlights ? data.nHighlights - 1 : 0;
}

int profile_item_selected(ProfileHighlight *output) {
  if (chosen < 0 || chosen >= data.nHighlights) return 0;
  if (output) *output = data.highlights[chosen];
  chosen = -1;
  return 1;
}

void profile_event(const SDL_Event *e) {
  if (!is_open || !e || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  if(side) {
    if(k==SDLK_ESCAPE || k==SDLK_AC_BACK || k==SDLK_BACKSPACE || k==SDLK_LEFT) {profile_close();return;}
    if(k==SDLK_UP && sideFocus>0)sideFocus--;
    if(k==SDLK_DOWN && sideFocus<2)sideFocus++;
    if(k==SDLK_RETURN || k==SDLK_KP_ENTER) {
      if(sideFocus==0)profile_close();
      else if(sideFocus==1){if(!loading)requestUpdate=1;}
      else {complete=1;is_open=0;}
    }
    return;
  }
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE || k == SDLK_DELETE) {
    profile_close(); return;
  }
  if(k==SDLK_r || ((!temData || error[0]) &&
     (k==SDLK_RETURN || k==SDLK_KP_ENTER))) {
    if(!loading)requestUpdate=1;
    return;
  }
  if (!temData) return;
  // O calendario recebe o D-pad real, com continuidade entre semanas. Sair
  // pela primeira/ultima semana devolve a navegacao para as secoes.
  if(section==2 && data.nDays>0) {
    if(k==SDLK_LEFT && day>0){day--;return;}
    if(k==SDLK_RIGHT && day+1<data.nDays){day++;return;}
    if(k==SDLK_UP && day>=7){day-=7;return;}
    if(k==SDLK_DOWN && day+7<data.nDays){day+=7;return;}
  }
  if(section==1) {
    if(k==SDLK_LEFT && item%2){item--;return;}
    if(k==SDLK_RIGHT && !(item%2) && item+1<data.nHighlights){item++;return;}
    if(k==SDLK_UP && item>=2){item-=2;return;}
    if(k==SDLK_DOWN && item+2<data.nHighlights){item+=2;return;}
  }
  if (k == SDLK_LEFT && (section!=1 || item==0)) { profile_close(); return; }
  if (k == SDLK_UP && section > 0) section--;
  else if (k == SDLK_DOWN && section < PF_SECTIONS - 1) {
    section++;
    if(section==2)day=0;
  }
  else if (section == 1 && (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE)) {
    if (data.nHighlights > 0) chosen = item;
  }
}

void profile_update(float dt, Uint32 now) {
  (void)now;
  int reduced=settings_animations_reduced();
  entry = anim_mola(entry, is_open ? 1.0f : 0.0f, dt, NV_MOLA_SCREEN);
  if (!is_open && entry < 0.002f) entry = 0;
  float target = SECTION_Y[section] - (section == 0 ? PF_SUMMARY_Y : 120.0f);
  float maxScroll = fmax0(PF_DOC_H - NV_TELA_H + 54.0f);
  scrollTarget = anim_clamp(target, 0, maxScroll);
  scroll = anim_mola2(&velScroll, scroll, scrollTarget, dt, NV_MOLA2_SCROLL);
  if(reduced) { entry=is_open?1:0;scroll=scrollTarget;velScroll=0; }
  for (int i = 0; i < PF_SECTIONS; i++)
    focusSec[i] = anim_mola(focusSec[i], i == section ? 1.0f : 0.0f, dt, NV_MOLA_FOCUS);
  for (int i = 0; i < PROFILE_MAX_HIGHLIGHTS; i++)
    focusItem[i] = anim_mola(focusItem[i], section == 1 && i == item ? 1.0f : 0.0f,
                            dt, NV_MOLA_FOCUS);
  if(reduced) {
    for(int i=0;i<PF_SECTIONS;i++)focusSec[i]=(i==section);
    for(int i=0;i<PROFILE_MAX_HIGHLIGHTS;i++)focusItem[i]=(section==1&&i==item);
  }
}

static void titleSection(const char *s, int which, float y, float a) {
  float f = focusSec[which];
  GfxRect dot = { PF_X, y + 14 - scroll, 14, 14 };
  gfx_color(dot, .5f, .54f + .12f*f, .24f + .08f*f, .72f + .16f*f, a);
  if (f > .02f)
    ring((GfxRect){dot.x-5,dot.y-5,dot.w+10,dot.h+10},.5f,a*f);
  text(TXT_TITLE3, s, 244, PF_X + 28, y, a);
  if (f > .02f) {
    const char *help=which==1?"Arrows: titles  ·  OK: details":
       which==2?"Arrows: days  ·  Back: menu":"Up/down: sections  ·  Back: menu";
    TxtLine d = txt_line(TXT_MINI, help, 185,185,194,255);
    txt_draw_alpha(d, PF_X + PF_W - d.w, y + 11 - scroll, a * f * .72f);
  }
}

static void drawLoading(Uint32 now, float a) {
  float pulse = settings_animations_reduced()?.55f:.48f+.12f*sinf((float)now*.004f);
  titleSection("Profile and Stats", 0, PF_TOP, a);
  gfx_color((GfxRect){PF_X,144-scroll,510,40},.18f,.18f,.18f,.20f,pulse*a);
  gfx_color((GfxRect){PF_X,214-scroll,930,92},.08f,.18f,.18f,.20f,pulse*a);
  for (int i=0;i<4;i++)
    gfx_color((GfxRect){PF_X+i*274,342-scroll,230,58},.15f,.18f,.18f,.20f,pulse*a);
  gfx_color((GfxRect){PF_X,PF_ACT_Y+58-scroll,PF_W*.57f,432},.035f,.18f,.18f,.20f,pulse*a);
  gfx_color((GfxRect){PF_X,PF_GENRES_Y+58-scroll,PF_W,176},.05f,.18f,.18f,.20f,pulse*a);
  for (int i=0;i<PROFILE_MAX_HIGHLIGHTS;i++) {
    int col=i%2,lin=i/2;
    gfx_color((GfxRect){PF_X+col*(PF_CARD_W+PF_CARD_GAP),
                      PF_HIGHLIGHTS_Y+62+lin*(PF_CARD_H+PF_CARD_GAP)-scroll,
                      PF_CARD_W,PF_CARD_H},.035f,.18f,.18f,.20f,pulse*a);
  }
}

static void drawEmpty(float a) {
  const char *title = "No plays in this period";
  const char *body = "Connect Trakt and watch a film or episode. Your summary uses only the history available.";
  if (state == PROFILE_STATE_PRIVATE) {
    title = "Private profile or history not shared";
    body = "Trakt did not release a public history for this account.";
  } else if (state == PROFILE_STATE_DISCONNECTED) {
    title = "Trakt disconnected";
    body = "Link Trakt to load identity, recent work and statistics.";
  } else if (state == PROFILE_STATE_UNAVAILABLE || state == PROFILE_STATE_ERROR) {
    title = "Profile unavailable";
    body = error[0] ? error : "Could not confirm this summary right now.";
  }
  titleSection("Profile and Stats", 0, PF_TOP, a);
  trim(TXT_TITLE2, title, 244, PF_X, 248, PF_W, a);
  txt_block(TXT_BODY,
    body,
    177,179,187, PF_X, 326-scroll, 780, NV_LD_BODY, a, 3);
  GfxRect btn={PF_X,452-scroll,330,72};
  gfx_color(btn,.24f,NV_COLOR_FOCUS_R,NV_COLOR_FOCUS_G,NV_COLOR_FOCUS_B,a);
  ring(btn,.24f,a);
  text(TXT_DET_BUTTON,"OK · Try again",240,PF_X+24,472,a);
  text(TXT_CAPTION,"Back returns to the menu",182,PF_X,554,a);
}

static void drawSummary(float a) {
  if(!visible(PF_SUMMARY_Y,400))return;
  char b[64], t[64];
  float identityX=PF_X+180.0f, identityW=PF_W*.40f;
  float periodY = PF_SUMMARY_Y + 64.0f;
  float usageY = PF_SUMMARY_Y + 102.0f;
  titleSection("Profile and Stats", 0, PF_SUMMARY_Y, a);
  { GfxRect av={PF_X,PF_SUMMARY_Y+62,140,140};
    GLuint tx=data.avatar[0]?tex_get_width(data.avatar,180):0;
    gfx_color(av,.5f,.14f,.16f,.19f,a);
    if(tx){gfx_tex_aspect_current=tex_aspect(data.avatar);gfx_rect(av,tx,GFX_AVATAR,0,0,0,0,1,1,1,a);gfx_tex_aspect_current=0;}
    else gfx_icon((GfxRect){PF_X+38,PF_SUMMARY_Y+100,64,64},"menu_profile",.82f,.83f,.86f,a);
  }
  if (data.name[0]) {
    trim(TXT_TITLE2,data.name,246,identityX,PF_SUMMARY_Y+62,identityW,a);
    if (data.user[0]) {
      snprintf(b,sizeof(b),"@%s",data.user);
      trim(TXT_CAPTION,b,183,identityX,PF_SUMMARY_Y+122,identityW,a);
      periodY = PF_SUMMARY_Y + 170.0f;
    } else periodY = PF_SUMMARY_Y + 128.0f;
    usageY = periodY + 44.0f;
  } else if (data.user[0]) {
    snprintf(b,sizeof(b),"@%s",data.user);
    trim(TXT_TITLE2,b,246,identityX,PF_SUMMARY_Y+62,identityW,a);
    periodY = PF_SUMMARY_Y + 128.0f;
    usageY = periodY + 44.0f;
  }
  textC(TXT_CAPTION, data.period[0] ? data.period : "Period not given",
         184,112,220, identityX, periodY, a);
  if (data.minutes > 0) {
    formatDuration(t,sizeof(t),data.minutes);
    snprintf(b,sizeof(b),"%s watched",t);
  } else snprintf(b,sizeof(b),"%d plays recorded",data.plays);
  trim(TXT_TITLE1,b,246,identityX,usageY,identityW,a);
  if(data.warning[0])trim(TXT_MINI,data.warning,183,identityX,PF_SUMMARY_Y+292,identityW,a);

  const char *rot[4]={"PLAYS","FILMS","EPISODES","ACTIVE DAYS"};
  int val[4]={data.plays,data.movies,data.episodes,data.daysActiveMonth};
  float x=PF_X+PF_W*.58f;
  for(int i=0;i<4;i++) {
    float xx=x+(i%2)*PF_W*.22f, yy=PF_SUMMARY_Y+62+(i/2)*104;
    number(b,sizeof(b),val[i]); text(TXT_TITLE2,b,244,xx,yy,a);
    textC(TXT_MINI,rot[i],166,168,178,xx,yy+62,a);
  }
  if(data.plays>0 && data.minutes>0) {
    formatDuration(t,sizeof(t),(int)((double)data.minutes/data.plays+.5));
    snprintf(b,sizeof(b),"%s per play",t);
    trim(TXT_CAPTION,b,193,x,PF_SUMMARY_Y+292,PF_W*.42f,a);
  }
  // As contagens representam eventos de reproducao, nao titulos unicos.
  double total=(double)data.movies+data.episodes;
  if(total>0) {
    float w=PF_W*.50f, movies=w*(float)(data.movies/total);
    gfx_color((GfxRect){PF_X,PF_SUMMARY_Y+316-scroll,w,10},.4f,.23f,.60f,.82f,a);
    if(movies>0)gfx_color((GfxRect){PF_X,PF_SUMMARY_Y+316-scroll,movies,10},.4f,.65f,.30f,.83f,a);
    snprintf(b,sizeof(b),"Films %.0f%%  ·  Episodes %.0f%%",100*data.movies/total,100*data.episodes/total);
    trim(TXT_CAPTION,b,201,PF_X,PF_SUMMARY_Y+340,w,a);
  }
}

static void drawActivity(float a) {
  if(!visible(PF_ACT_Y,500))return;
  titleSection("Activity rhythm",2,PF_ACT_Y,a);
  GfxRect panel={PF_X,PF_ACT_Y+58-scroll,PF_W*.57f,432};
  gfx_color(panel,.045f,.075f,.073f,.085f,.98f*a);
  static const char *days[7]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  float x0=PF_X+32, y0=PF_ACT_Y+110-scroll, cel=44, gap=10;
  for(int c=0;c<7;c++) {
    TxtLine l=txt_line(TXT_MINI,days[c],145,147,156,255);
    txt_draw_alpha(l,x0+c*(cel+gap)+(cel-l.w)*.5f,y0-28,a);
  }
  int max=0,best=0,last=-1;
  for(int i=0;i<data.nDays;i++) {
    if(data.activity[i]) last=i;
    if(data.activity[i]>max){max=data.activity[i];best=i;}
  }
  for(int i=0;i<data.nDays;i++) {
    int p=data.firstDayWeek+i, col=p%7, lin=p/7;
    float q=max?(float)data.activity[i]/max:0.0f;
    GfxRect c={x0+col*(cel+gap),y0+lin*(cel+gap),cel,cel};
    if(q<=0) gfx_color(c,.16f,.12f,.12f,.14f,.78f*a);
    else gfx_color(c,.16f,.38f+.23f*q,.13f+.13f*q,.52f+.31f*q,a);
    char nd[8];snprintf(nd,sizeof(nd),"%d",i+1);
    TxtLine l=txt_line(TXT_MINI,nd,242,242,246,255);
    txt_draw_alpha(l,c.x+(c.w-l.w)*.5f,c.y+(c.h-l.h)*.5f,a);
    if(section==2 && i==day)ring((GfxRect){c.x-3,c.y-3,c.w+6,c.h+6},.20f,a);
  }
  char b[96];
  if(data.nDays)snprintf(b,sizeof(b),"Day %d: %u plays",day+1,data.activity[day]);
  else snprintf(b,sizeof(b),"Daily activity unavailable");
  trim(TXT_CAPTION,b,226,PF_X+32,PF_ACT_Y+450,panel.w-64,a);
  // Legenda separada do calendario: leitura numerica continua acessivel sem
  // depender de distinguir cinco tons de violeta na tela.
  float lx=PF_X+454;
  text(TXT_CAPTION,"Plays",205,lx,PF_ACT_Y+106,a);
  for(int i=0;i<5;i++) {
    float q=i/4.0f;
    gfx_color((GfxRect){lx+i*38,PF_ACT_Y+150-scroll,28,28},.18f,
             i?.38f+.23f*q:.12f,i?.13f+.13f*q:.12f,i?.52f+.31f*q:.14f,a);
  }
  snprintf(b,sizeof(b),"0 a %d por dia",max);
  trim(TXT_MINI,b,184,lx,PF_ACT_Y+196,panel.w-480,a);
  if(data.activity[best]){
    snprintf(b,sizeof(b),"Peak: %d plays on day %d",max,best+1);
    trim(TXT_CAPTION,b,219,lx,PF_ACT_Y+276,panel.w-480,a);
  }
  if(last>=0) {
    snprintf(b,sizeof(b),"Last activity: day %d",last+1);
    trim(TXT_CAPTION,b,193,lx,PF_ACT_Y+326,panel.w-480,a);
  }
  float rx=PF_X+PF_W*.63f;
  number(b,sizeof(b),data.streakCurrent); textC(TXT_TITLE1,b,190,111,224,rx,PF_ACT_Y+76,a);
  trim(TXT_CALLOUT,data.streakComplete?"day streak":"day streak this month",220,rx,PF_ACT_Y+148,PF_W*.37f,a);
  snprintf(b,sizeof(b),"%d of %d active days this month",data.daysActiveMonth,data.nDays);
  trim(TXT_BODY,b,203,rx,PF_ACT_Y+218,PF_W*.37f,a);
  if(data.yearComplete){
    snprintf(b,sizeof(b),"%d active days this year",data.daysActiveYear);
    trim(TXT_CAPTION,b,185,rx,PF_ACT_Y+278,PF_W*.37f,a);
  } else trim(TXT_CAPTION,"Coverage: selected period",185,rx,PF_ACT_Y+278,PF_W*.37f,a);
}

static void drawGenres(float a) {
  if(!visible(PF_GENRES_Y,280))return;
  titleSection("Your genre map",3,PF_GENRES_Y,a);
  if(!data.nGenres) { text(TXT_BODY,"Genres appear as the history grows.",170,
                              PF_X,PF_GENRES_Y+78,a); return; }
  double total=0; for(int i=0;i<data.nGenres;i++) total+=data.genres[i].count;
  if(total<1) total=1;
  float x=PF_X,y=PF_GENRES_Y+92-scroll;
  for(int i=0;i<data.nGenres;i++) {
    float w=PF_W*((float)data.genres[i].count/total),r,g,b;
    rgb(data.genres[i].color?data.genres[i].color:PALETTE[i],&r,&g,&b);
    if(w>2)gfx_color((GfxRect){x,y,w-2,30},.12f,r,g,b,a); x+=w;
  }
  x=PF_X;
  for(int i=0;i<data.nGenres;i++) {
    float w=PF_W/4,xx=PF_X+(i%4)*w,yy=PF_GENRES_Y+148+(i/4)*52;
    float r,g,b;rgb(data.genres[i].color?data.genres[i].color:PALETTE[i],&r,&g,&b);
    gfx_color((GfxRect){xx,yy+7-scroll,14,14},.5f,r,g,b,a);
    char rot[64];snprintf(rot,sizeof(rot),"%s · %d",data.genres[i].name,data.genres[i].count);
    trim(TXT_CAPTION,rot,219,xx+26,yy,w-42,a);
  }
}

static void drawHighlights(float a) {
  if(!visible(PF_HIGHLIGHTS_Y,520))return;
  titleSection("Most watched",1,PF_HIGHLIGHTS_Y,a);
  if(!data.nHighlights) { text(TXT_BODY,"No highlights in this period.",170,
                                PF_X,PF_HIGHLIGHTS_Y+78,a); return; }
  gfx_crop(PF_X-12,0,PF_W+24,PF_CONTENT_H);
  for(int i=0;i<data.nHighlights;i++) {
    int col=i%2,lin=i/2;
    float f=focusItem[i], x=PF_X+col*(PF_CARD_W+PF_CARD_GAP);
    float docY=PF_HIGHLIGHTS_Y+62+lin*(PF_CARD_H+PF_CARD_GAP);
    float y=docY-scroll;
    if(y+PF_CARD_H<0 || y>PF_CONTENT_H) continue;
    float lift=f*NV_FOCUS_LIFT;
    GfxRect r={x,y-lift,PF_CARD_W,PF_CARD_H};
    if(f>.02f) {
      GfxRect s={r.x-18,r.y+10,r.w+36,r.h+26};
      gfx_rect(s,0,GFX_SHADOW,f,0,0,.05f,0,0,0,NV_SHADOW_ALFA*a*f);
    }
    gfx_color(r,.045f,.045f,.075f,.085f,a);
    const char *art=data.highlights[i].backdrop[0]?data.highlights[i].backdrop:data.highlights[i].poster;
    GfxRect mini={r.x+18,r.y+11,168,90};
    GLuint tx=art[0]?tex_get_width(art,mini.w):0;
    if(tx){gfx_tex_aspect_current=tex_aspect(art);gfx_rect(mini,tx,GFX_CARD,f,0,0,.045f,1,1,1,a);gfx_tex_aspect_current=0;}
    else gfx_color(mini,.045f,.14f,.14f,.16f,a);
    char rank[12],meta[80],line[112]; snprintf(rank,sizeof(rank),"#%d",i+1);
    textC(TXT_CALLOUT,rank,189,112,220,r.x+204,docY-lift+15,a*.90f);
    trim(TXT_TITLE3,data.highlights[i].title,246,r.x+250,docY-lift+8,r.w-270,a);
    trim(TXT_CAPTION,data.highlights[i].detail,216,r.x+204,docY-lift+66,r.w-222,a);
    if(data.highlights[i].minutes>0) {
      formatDuration(meta,sizeof(meta),data.highlights[i].minutes);
      snprintf(line,sizeof(line),"%d plays  ·  %s",data.highlights[i].plays,meta);
    } else snprintf(line,sizeof(line),"%d plays",data.highlights[i].plays);
    trim(TXT_CAPTION,line,194,r.x+204,docY-lift+91,r.w-222,a);
    if(section==1 && item==i)ring((GfxRect){r.x-NV_RING_FOCUS,r.y-NV_RING_FOCUS,
                              r.w+NV_RING_FOCUS*2,r.h+NV_RING_FOCUS*2},.047f,a);
  }
  gfx_sem_crop();
}

void profile_draw(Uint32 now) {
  if (entry <= .002f) return;
  float a=entry;
  if(side) {
    float x=1120+(1-a)*800;
    gfx_color((GfxRect){0,0,1920,1080},0,.015f,.018f,.025f,.65f*a);
    gfx_color((GfxRect){x,24,776,1032},.035f,.08f,.075f,.09f,a);
    txt_draw_alpha(txt_line(TXT_TITLE3,"Your activity",244,242,248,255),x+44,62,a);
    const char *labels[]={"Close","Refresh","See full profile"};
    for(int i=0;i<3;i++) {
      GfxRect b={x+44,i==0?130:878+(i-1)*76,688,60};
      int f=sideFocus==i;
      gfx_color(b,.18f,f?.92f:.14f,f?.91f:.13f,f?.96f:.16f,a);
      txt_draw_alpha(txt_line(TXT_BODY,labels[i],f?25:236,f?24:234,f?30:242,255),b.x+24,b.y+14,a);
    }
    const char *msg=error[0]?error:loading?"Loading history…":data.period;
    txt_draw_alpha(txt_line_trim(TXT_BODY,msg,193,186,202,255,688),x+44,220,a);
    if(!loading && !error[0] && !temData)
      txt_draw_alpha(txt_line(TXT_BODY,"No activity in this period",221,217,228,255),x+44,270,a);
    if(temData) {
      const char *sem[]={"D","S","T","Q","Q","S","S"};
      for(int c=0;c<7;c++)txt_draw_alpha(txt_line(TXT_CAPTION,sem[c],185,177,197,255),x+63+c*94,280,a);
      unsigned max=0;for(int d=0;d<data.nDays;d++)if(data.activity[d]>max)max=data.activity[d];
      for(int d=0;d<data.nDays;d++) {
        int slot=d+data.firstDayWeek;float v=max?(float)data.activity[d]/max:0.0f;
        GfxRect b={x+44+(slot%7)*94,320+(slot/7)*70,82,58};
        gfx_color(b,.14f,.14f+v*.43f,.12f+v*.08f,.18f+v*.51f,a);
        char n[8];snprintf(n,sizeof n,"%d",d+1);
        txt_draw_alpha(txt_line(TXT_CAPTION,n,243,239,249,255),b.x+24,b.y+16,a);
      }
      char b[128],duration[32];formatDuration(duration,sizeof duration,data.minutes);
      snprintf(b,sizeof b,"%s · %d plays",duration,data.plays);
      txt_draw_alpha(txt_line_trim(TXT_BODY,b,244,240,248,255,688),x+44,755,a);
      snprintf(b,sizeof b,"%d active days · %d day streak this month",data.daysActiveMonth,data.streakCurrent);
      txt_draw_alpha(txt_line_trim(TXT_CAPTION,b,193,181,210,255,688),x+44,801,a);
      if(data.partial)txt_draw_alpha(txt_line(TXT_MINI,"Partial history · details in the profile",180,170,192,255),x+44,840,a);
    }
    return;
  }
  gfx_color((GfxRect){0,0,NV_TELA_W,NV_TELA_H},0,
          NV_COLOR_BACKGROUND_R,NV_COLOR_BACKGROUND_G,NV_COLOR_BACKGROUND_B,a);
  gfx_crop(0,0,NV_TELA_W,PF_CONTENT_H);
  if(loading && !temData) drawLoading(now,a);
  else if(!temData) drawEmpty(a);
  else {
    drawSummary(a); drawActivity(a); drawGenres(a); drawHighlights(a);
  }
  gfx_sem_crop();
  char warningBuf[320];
  const char *warning=NULL;
  if(error[0]){
    if(state==PF_STALE)snprintf(warningBuf,sizeof warningBuf,"Update unavailable · showing the last summary received. %s",error);
    else snprintf(warningBuf,sizeof warningBuf,"%s",error);
    warning=warningBuf;
  } else if(loading)warning="Updating history without interrupting what is already shown…";
  else warning=data.warning[0]?data.warning:data.partial?"Partial history: the totals count only the records loaded.":NULL;
  if(warning){
    gfx_color((GfxRect){PF_X,PF_WARNING_Y,PF_W,PF_WARNING_H},.15f,.13f,.11f,.16f,.97f*a);
    TxtLine l=txt_line_trim(TXT_CAPTION,warning,226,217,235,255,PF_W-40);
    txt_draw_alpha(l,PF_X+20,PF_WARNING_Y+14,a);
  }
}
