#include "social.h"
#include "trakt.h"
#include "net.h"
#include "js.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "layout.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SOCIAL_LOADING SOCIAL_STATE_LOADING
#define SOCIAL_UPDATING SOCIAL_STATE_UPDATING
#define SOCIAL_READY SOCIAL_STATE_READY
#define SOCIAL_STALE SOCIAL_STATE_STALE
#define SOCIAL_NO_ACTIVITY SOCIAL_STATE_NO_ACTIVITY
#define SOCIAL_PRIVATE SOCIAL_STATE_PRIVATE
#define SOCIAL_DISCONNECTED SOCIAL_STATE_DISCONNECTED
#define SOCIAL_UNAVAILABLE SOCIAL_STATE_UNAVAILABLE
typedef struct {
  char person[96], action[64], title[160], detail[160], timeStr[32];
  char imdb[16], poster[512];
} Activity;
typedef struct {
  unsigned generation; CatItem person; char bio[640], local[160];
  SocialState state; int n; Activity activities[20];
} SocialData;
typedef struct { unsigned generation; CatItem person; } SocialTask;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SocialData data, ready;
static unsigned generation;
static int hasReady, sair, selected, chosen = -1;

static int slugValid(const char *s) {
  return s && s[0] && strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == strlen(s);
}
static int fieldInside(const char *start, const char *end, const char *key,
                       char *dst, size_t size) {
  char search[48];
  const char *p;
  snprintf(search, sizeof search, "\"%s\"", key);
  p = strstr(start, search);
  return p && p < end && js_text(p, end, key, dst, size);
}
static time_t isoToTime(const char *iso) {
  int y, m, d, h, mi, s; struct tm t;
  if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &h, &mi, &s) != 6) return (time_t)-1;
  memset(&t, 0, sizeof t); t.tm_year=y-1900; t.tm_mon=m-1; t.tm_mday=d;
  t.tm_hour=h; t.tm_min=mi; t.tm_sec=s; return timegm(&t);
}
static void timeLocal(const char *iso, char *dst, size_t size) {
  time_t stamp=isoToTime(iso); struct tm local;
  if (stamp==(time_t)-1 || !localtime_r(&stamp,&local)) { dst[0]=0; return; }
  strftime(dst,size,"%d/%m/%Y · %H:%M",&local);
}
static const char *labelAction(const char *action, const char *timestamp) {
  if (!strcmp(action,"watching")) {
    time_t stamp=isoToTime(timestamp);
    if (stamp!=(time_t)-1 && difftime(time(NULL),stamp)<=10.0*60.0) return "watching now";
    return "watched";
  }
  if (!strcmp(action,"watch")||!strcmp(action,"watched")||!strcmp(action,"scrobble")) return "watched";
  if (!strcmp(action,"rating")||!strcmp(action,"rated")) return "rated";
  if (!strcmp(action,"checkin")||!strcmp(action,"check-in")) return "checked in";
  return "recent activity";
}
static int parseActivities(const char *body, SocialData *d, const char *actionDflt) {
  const char *p=strchr(body?body:"",'['); int n=0; p=p?p+1:NULL;
  while (p&&*p&&n<20) {
    const char *f,*obj,*ep; Activity *a; char action[32]="",timestamp[40]="",imdb[24]="";
    while(*p&&(unsigned char)*p<=' ')p++; if(*p!='{')break; f=js_end(p);
    a=&d->activities[n]; memset(a,0,sizeof *a);
    snprintf(a->person,sizeof a->person,"%s",d->person.socialName[0]?d->person.socialName:d->person.socialSlug);
    if(!fieldInside(p,f,"action",action,sizeof action)||!action[0])snprintf(action,sizeof action,"%s",actionDflt?actionDflt:"watch");
    if(!fieldInside(p,f,"timestamp",timestamp,sizeof timestamp))fieldInside(p,f,"watched_at",timestamp,sizeof timestamp);
    snprintf(a->action,sizeof a->action,"%s",labelAction(action,timestamp)); timeLocal(timestamp,a->timeStr,sizeof a->timeStr);
    obj=strstr(p,"\"show\""); if(!obj||obj>=f)obj=strstr(p,"\"movie\"");
    if(!obj||obj>=f){p=js_next(f);continue;} obj=strchr(obj,'{'); if(!obj||obj>=f){p=js_next(f);continue;}
    {const char *fo=js_end(obj);js_text(obj,fo,"title",a->title,sizeof a->title);js_text(obj,fo,"imdb",imdb,sizeof imdb);}
    ep=strstr(p,"\"episode\"");
    if(ep&&ep<f){const char *eo=strchr(ep,'{'),*ef=eo?js_end(eo):NULL;int t=eo?(int)js_num(eo,ef,"season",0):0,e=eo?(int)js_num(eo,ef,"number",0):0;char name[100]="";
      if(eo&&ef)js_text(eo,ef,"title",name,sizeof name);snprintf(a->detail,sizeof a->detail,"T%dE%d%s%s",t,e,name[0]?" · ":"",name);
    } else snprintf(a->detail,sizeof a->detail,"Film");
    if(!imdb[0]){p=js_next(f);continue;} snprintf(a->imdb,sizeof a->imdb,"%s",imdb);
    snprintf(a->poster,sizeof a->poster,"https://images.metahub.space/poster/medium/%s/img",imdb); n++; p=js_next(f);
  }
  return n;
}
static void *load(void *arg) {
  SocialTask *t=arg; SocialData *d=calloc(1,sizeof *d); const char *header[4]; char auth[200],key[140],url[512],*body;
  if(!d){free(t);return NULL;} d->generation=t->generation;d->person=t->person;d->state=SOCIAL_UNAVAILABLE;
  if(!slugValid(d->person.socialSlug)||!trakt_headers(header,auth,sizeof auth,key,sizeof key))d->state=!trakt_active()?SOCIAL_DISCONNECTED:SOCIAL_UNAVAILABLE;
  else {
    snprintf(url,sizeof url,"https://api.trakt.tv/users/%s?extended=full",d->person.socialSlug); body=net_download_com(url,10,header);
    if(!body)d->state=SOCIAL_UNAVAILABLE;
    else {js_text(body,NULL,"name",d->person.socialName,sizeof d->person.socialName);js_text(body,NULL,"about",d->bio,sizeof d->bio);js_text(body,NULL,"location",d->local,sizeof d->local);
      {const char *av=strstr(body,"\"avatar\"");if(av)js_text(av,NULL,"full",d->person.socialAvatar,sizeof d->person.socialAvatar);}free(body);
      snprintf(url,sizeof url,"https://api.trakt.tv/users/%s/activities?limit=20&extended=full",d->person.socialSlug);body=net_download_com(url,12,header);
      if(body&&strchr(body,'[')){d->n=parseActivities(body,d,"watch");d->state=d->n?SOCIAL_READY:SOCIAL_NO_ACTIVITY;free(body);}
      else {free(body);snprintf(url,sizeof url,"https://api.trakt.tv/users/%s/history?limit=20&extended=full",d->person.socialSlug);body=net_download_com(url,12,header);
        if(body&&strchr(body,'[')){d->n=parseActivities(body,d,"watch");d->state=d->n?SOCIAL_READY:SOCIAL_NO_ACTIVITY;}else d->state=SOCIAL_PRIVATE;free(body);}
    }
  }
  pthread_mutex_lock(&lock);if(d->generation==generation){ready=*d;hasReady=1;}pthread_mutex_unlock(&lock);free(d);free(t);return NULL;
}
static void openInternal(const CatItem *person, int preserve) {
  SocialTask *t; pthread_t thread; CatItem copy; memset(&copy,0,sizeof copy);if(person)copy=*person;
  SocialData previous=data;
  pthread_mutex_lock(&lock);generation++;hasReady=0;
  if(preserve)data=previous;else data=(SocialData){0};
  data.generation=generation;data.person=copy;data.state=preserve?SOCIAL_UPDATING:SOCIAL_LOADING;pthread_mutex_unlock(&lock);
  sair=0;selected=0;chosen=-1;t=malloc(sizeof *t);if(!t){data.state=SOCIAL_UNAVAILABLE;return;}t->generation=generation;t->person=copy;
  if(pthread_create(&thread,NULL,load,t)==0)pthread_detach(thread);else{free(t);data.state=SOCIAL_UNAVAILABLE;}
}
void social_open(const CatItem *person) { openInternal(person,0); }
int social_wants_exit(void){int v=sair;sair=0;return v;}
SocialState social_state(void){return data.state;}
void social_event(const SDL_Event *e){SDL_Keycode k;if(!e||e->type!=SDL_KEYDOWN)return;k=e->key.keysym.sym;
  if(k==SDLK_ESCAPE||k==SDLK_AC_BACK||k==SDLK_BACKSPACE||k==SDLK_LEFT){sair=1;return;}if(k==SDLK_UP&&selected>0)selected--;if(k==SDLK_DOWN&&selected+1<data.n)selected++;
  if(k==SDLK_r && data.state!=SOCIAL_LOADING && data.state!=SOCIAL_UPDATING){openInternal(&data.person,1);return;}
  if(k==SDLK_RETURN||k==SDLK_KP_ENTER){if(data.state==SOCIAL_PRIVATE||data.state==SOCIAL_UNAVAILABLE||data.state==SOCIAL_DISCONNECTED||data.state==SOCIAL_STALE){openInternal(&data.person,1);return;}if((data.state==SOCIAL_READY||data.state==SOCIAL_UPDATING)&&selected>=0&&selected<data.n&&data.activities[selected].imdb[0])chosen=selected;}
}
int social_item_selected(SocialItemSelected *output){if(chosen<0||chosen>=data.n)return 0;if(output){snprintf(output->imdb,sizeof output->imdb,"%s",data.activities[chosen].imdb);snprintf(output->title,sizeof output->title,"%s",data.activities[chosen].title);}chosen=-1;return 1;}
void social_update(float dt,Uint32 now){(void)dt;(void)now;pthread_mutex_lock(&lock);if(hasReady){SocialData new=ready;if((new.state==SOCIAL_PRIVATE||new.state==SOCIAL_UNAVAILABLE||new.state==SOCIAL_DISCONNECTED)&&data.n>0){new.n=data.n;memcpy(new.activities,data.activities,sizeof new.activities);new.state=SOCIAL_STALE;}data=new;hasReady=0;if(selected>=data.n)selected=data.n?data.n-1:0;}pthread_mutex_unlock(&lock);}
static void text(TxtStyle style,const char *s,float x,float y,float w,int color){txt_draw(txt_line_trim(style,s?s:"",color,color,color,255,w),x,y);}
static const char *stateText(SocialState state){switch(state){case SOCIAL_LOADING:return "Loading activity…";case SOCIAL_UPDATING:return "Updating · previous activity kept";case SOCIAL_STALE:return "Update unavailable · showing previous activity";case SOCIAL_NO_ACTIVITY:return "No public activity for now";case SOCIAL_PRIVATE:return "Activity private or not shared";case SOCIAL_DISCONNECTED:return "Trakt disconnected";case SOCIAL_UNAVAILABLE:return "Service unavailable";default:return "Recent activity";}}
void social_draw(Uint32 now){(void)now;gfx_color((GfxRect){0,0,NV_SCREEN_W,NV_SCREEN_H},0,NV_COLOR_BACKGROUND_R,NV_COLOR_BACKGROUND_G,NV_COLOR_BACKGROUND_B,1);text(TXT_CAPTION,"AMONG FRIENDS",96,56,550,179);
  {GfxRect av={96,132,176,176};GLuint tex=data.person.socialAvatar[0]?tex_get_width(data.person.socialAvatar,220):0;gfx_color(av,.5f,.15f,.16f,.18f,1);if(tex){gfx_tex_aspect_current=tex_aspect(data.person.socialAvatar);gfx_rect(av,tex,GFX_AVATAR,0,0,0,0,1,1,1,1);gfx_tex_aspect_current=0;}else gfx_icon((GfxRect){148,184,72,72},"menu_profile",.8f,.81f,.83f,1);}
  if(data.person.socialName[0])text(TXT_TITLE2,data.person.socialName,96,346,480,245);else if(data.state==SOCIAL_UNAVAILABLE||data.state==SOCIAL_DISCONNECTED)text(TXT_TITLE2,"Profile unavailable",96,346,480,245);if(data.person.socialSlug[0]){char u[160];snprintf(u,sizeof u,"@%s",data.person.socialSlug);text(TXT_CALLOUT,u,96,416,480,179);}if(data.local[0])text(TXT_CAPTION,data.local,96,470,480,179);if(data.bio[0])txt_block(TXT_CAPTION,data.bio,210,210,210,96,524,480,32,1,8);
  text(TXT_CAPTION,"Trakt · public profile",96,900,480,179);text(TXT_CAPTION,"← Back",96,970,480,235);text(TXT_TITLE3,"Recent activity",656,64,1150,245);text(TXT_CAPTION,"Person · action · title · time",656,122,1150,179);
  if(data.state==SOCIAL_LOADING){for(int i=0;i<4;i++)gfx_color((GfxRect){656,210+i*170,1120,134},.06f,.12f,.125f,.14f,1);text(TXT_CAPTION,stateText(data.state),680,248,1050,210);return;}
  if(data.state!=SOCIAL_READY&&data.state!=SOCIAL_UPDATING&&data.state!=SOCIAL_STALE){text(TXT_TITLE3,stateText(data.state),656,236,1120,235);if(data.state==SOCIAL_PRIVATE||data.state==SOCIAL_UNAVAILABLE||data.state==SOCIAL_DISCONNECTED)text(TXT_CALLOUT,"OK or R · try again",656,310,1120,235);return;}
  { int start = selected > 3 ? selected - 3 : 0;
    for (int i = start; i < data.n && i < start + 5; i++) {
      Activity *a=&data.activities[i]; float y=196+(i-start)*154;
      if(i==selected)gfx_color((GfxRect){640,y-12,1160,144},.07f,.18f,.185f,.20f,1);
      if(a->poster[0]){GLuint p=tex_get_width(a->poster,96);if(p){gfx_tex_aspect_current=tex_aspect(a->poster);gfx_rect((GfxRect){656,y,96,134},p,GFX_CARD,0,0,0,.055f,0,0,0,1);gfx_tex_aspect_current=0;}}
      text(TXT_MINI,a->person,772,y+4,430,179);text(TXT_CALLOUT,a->action,772,y+36,430,235);text(TXT_CAPTION,a->title,1210,y+4,500,245);text(TXT_CAPTION,a->detail,1210,y+42,500,179);if(a->timeStr[0])text(TXT_MINI,a->timeStr,1210,y+82,500,179);
    }
  }
  text(TXT_MINI,data.state==SOCIAL_STALE?"Update unavailable · showing previous activity · OK: try again":"OK · open title   ·   Back · close",656,1000,1120,190);
}
