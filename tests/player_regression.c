#include "streams.h"
#include "catalog.h"
#include "episodes.h"
#include "player.h"
#include "tracks.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "extras.h"
#include "trakt.h"
#include "addons.h"
#include "net.h"
#include "detail.h"
#include "home.h"
#include "menu.h"
#include "resume.h"
#include "subtitle.h"
#include "intro.h"
#include "profile.h"
#include "social.h"
#include <SDL2/SDL_image.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int cat_history_state_item(int index_);
extern void cat_history_set_id(const char *imdb, const char *kind, int watched);

static void key(SDL_Keycode k) {
  SDL_Event e={0};e.type=SDL_KEYDOWN;e.key.keysym.sym=k;episodes_event(&e);
}
static void keyPlayer(SDL_Keycode k) {
  SDL_Event e={0};e.type=SDL_KEYDOWN;e.key.keysym.sym=k;player_event(&e);
}
static void keyMenu(SDL_Keycode k) {
  SDL_Event e={0};e.type=SDL_KEYDOWN;e.key.keysym.sym=k;menu_event(&e);
}
static void testar(void) {
  profile_start();profile_open_side();
  assert(profile_side() && profile_is_open());
  { ProfileData pd={0};
    snprintf(pd.name,sizeof pd.name,"Test profile");
    snprintf(pd.user,sizeof pd.user,"teste");
    pd.period[0]='J'; pd.period[1]=0;
    profile_set_data(&pd);
    profile_set_loading(1);
    profile_set_error("network unavailable");
    profile_set_data(NULL);
  }
  SDL_Event pe={0};pe.type=SDL_KEYDOWN;pe.key.keysym.sym=SDLK_DOWN;
  profile_event(&pe);pe.key.keysym.sym=SDLK_RETURN;profile_event(&pe);
  assert(profile_requested_update());
  pe.key.keysym.sym=SDLK_DOWN;profile_event(&pe);
  pe.key.keysym.sym=SDLK_RETURN;profile_event(&pe);
  assert(profile_requested_complete());
  profile_open_side();pe.key.keysym.sym=SDLK_ESCAPE;profile_event(&pe);
  assert(!profile_is_open() && profile_wants_exit());
  char json[24000];size_t p=0;
  SubtitleCue *lc=NULL;
  int nc=subtitle_parse("WEBVTT\n\n00:00:01.000 --> 00:00:03.250\n<i>Hello &amp; welcome</i>\n\n2\n00:00:04,000 --> 00:00:06,000\nSecond line\n",&lc);
  assert(nc==2&&lc[0].start==1.0&&lc[0].end==3.25&&!strcmp(lc[0].text,"Hello & welcome"));free(lc);
  IntroChunk it[3];int ni=intro_parse("{\"intro\":{\"start_sec\":12,\"end_sec\":44},\"recap\":null,\"outro\":{\"start_sec\":3000,\"end_sec\":3060}}",it,3);
  assert(ni==2&&it[0].kind==INTRO_OPENING&&it[0].end==44&&it[1].kind==INTRO_CREDITS);
  char subUrl[256];
  video_normalize_url_subtitle("https://subs.example/file/123",subUrl,sizeof subUrl);
  assert(!strcmp(subUrl,"https://subs.example/file/123.srt"));
  video_normalize_url_subtitle("https://subs.example/file/123?x=1",subUrl,sizeof subUrl);
  assert(!strcmp(subUrl,"https://subs.example/file/123.srt?x=1"));
  video_normalize_url_subtitle("https://subs.example/file/123.VTT?x=1",subUrl,sizeof subUrl);
  assert(!strcmp(subUrl,"https://subs.example/file/123.VTT?x=1"));
  p+=snprintf(json+p,sizeof json-p,"{\"streams\":[");
  for(int i=0;i<40;i++) p+=snprintf(json+p,sizeof json-p,
    "%s{\"name\":\"Source %d\",\"url\":\"https://example.invalid/%d\",\"behaviorHints\":{\"filename\":\"Silo.S02E04.%s\"}}",
    i?",":"",i,i,i==37?"2160p.DV.Atmos.mp4":"1080p.DVDRip.mkv");
  snprintf(json+p,sizeof json-p,"]}");
  Stream *v=NULL;int n=stream_parse(json,"Fixture",&v);
  assert(n==40 && v[37].dolbyVision && v[37].mp4 && v[37].dolbyAtmos && v[37].height==2160);
  assert(!v[0].dolbyVision);
  stream_set_list(v,n);free(v);
  assert(stream_n()==40 && stream_automatic()==37 && stream_item(40)==NULL);
  stream_set_list(NULL,0);assert(stream_n()==0 && stream_automatic()==-1);
  n=stream_parse("{\"streams\":[{\"infoHash\":\"abc\"},{\"externalUrl\":\"https://example.invalid\"},{\"url\":\"https://example.invalid/a.mp4\",\"title\":\"4K [DV] Atmos\"}]}","Fixture",&v);
  assert(n==1 && v[0].mp4 && v[0].dolbyVision && v[0].height==2160);free(v);
  char longa[4500];memset(longa,'a',sizeof longa);memcpy(longa,"https://example.invalid/",24);longa[4090]=0;
  snprintf(json,sizeof json,"{\"streams\":[{\"url\":\"%s\",\"name\":\"DV/HDR 4K MP4\"}]}",longa);
  n=stream_parse(json,"Fixture",&v);assert(n==1 && strlen(v[0].url)==4090 && v[0].dolbyVision);free(v);
  longa[4090]='a';longa[4400]=0;
  snprintf(json,sizeof json,"{\"streams\":[{\"url\":\"%s\"}]}",longa);
  n=stream_parse(json,"Fixture",&v);assert(n==0);free(v);
  CatItem c={0};snprintf(c.kind,sizeof c.kind,"series");snprintf(c.title,sizeof c.title,"Silo");
  c.nSeasons=2;c.seasons[0]=1;c.seasons[1]=2;cat_set(&c,1);
  CatEp eps[40]={0};
  for(int i=0;i<40;i++){eps[i].season=i/20+1;eps[i].episode=i%20+1;snprintf(eps[i].name,sizeof eps[i].name,"Episode %d",i+1);}
  cat_set_episodes(0,eps,40);assert(cat_n_episodes(0)==40);
  episodes_open(0,2,4);episodes_update(.016f);key(SDLK_RETURN);
  int t=0,e=0;assert(!episodes_is_open() && !episodes_chose(&t,&e));
  episodes_open(0,2,4);episodes_update(.016f);key(SDLK_DOWN);key(SDLK_RETURN);
  assert(episodes_chose(&t,&e) && t==2 && e==5);
  assert(!episodes_chose(&t,&e));
  player_open(0,NULL);player_set_episode(2,4);
  assert(strstr(player_line_episode(),"T2E4") && strstr(player_line_episode(),"Episode 24"));
  assert(player_next_episode()&&player_next_episode()->season==2&&player_next_episode()->episode==5);
  keyPlayer(SDLK_RIGHT);keyPlayer(SDLK_RIGHT);keyPlayer(SDLK_RETURN);
  assert(player_requested_tracks()==2); /* Play, aspect, subtitles: no gaps. */
  assert(player_controls_visible());
  keyPlayer(SDLK_DOWN);assert(!player_controls_visible());
  keyPlayer(SDLK_DOWN);assert(player_controls_visible());
  keyPlayer(SDLK_RIGHT);keyPlayer(SDLK_RIGHT);keyPlayer(SDLK_RETURN);
  assert(player_requested_sources());
  assert(player_loading());player_shutdown();
  strcpy(c.kind,"movie");cat_set(&c,1);player_open(0,NULL);
  player_set_episode(2,4);assert(!player_line_episode()[0]);
  for(int i=0;i<10;i++)keyPlayer(SDLK_RIGHT);
  keyPlayer(SDLK_RETURN);assert(player_requested_sources() && !episodes_is_open());
  player_shutdown();strcpy(c.kind,"series");
  menu_start();menu_open();keyMenu(SDLK_DOWN);keyMenu(SDLK_RETURN);
  assert(menu_destination()==MENU_FETCH && menu_changed_destination() && !menu_changed_destination());
  menu_open();keyMenu(SDLK_DOWN);keyMenu(SDLK_ESCAPE);
  assert(!menu_is_open() && menu_destination()==MENU_FETCH);
  char folder[]="/tmp/nuvio-progress-test-XXXXXX",file[256];assert(mkdtemp(folder));
  snprintf(file,sizeof file,"%s/catalog.txt",folder);
  FILE *fp=fopen(file,"w");assert(fp);fclose(fp);
  cat_load(folder);strcpy(c.imdb,"tt0000001");cat_set(&c,1);
  c.progress=95;assert(cat_history_state_item(0)<0);
  cat_history_set_id("tt0000001","series",1);
  assert(cat_history_state_item(0)==1);
  cat_history_set_id("tt0000001:2:4","series",0);
  assert(cat_history_state_item(0)==0);
  cat_save_progress_ep(0,500,1000,2,4);
  c.season=1;c.episode=1;strcpy(c.nameEpisode,"Old name");
  cat_set(&c,1);
  assert(cat_item(0)->season==2 && cat_item(0)->episode==4 && cat_item(0)->progress==50);
  assert(!cat_item(0)->nameEpisode[0]);
  unlink(file);snprintf(file,sizeof file,"%s/progress.txt",folder);unlink(file);rmdir(folder);
  puts("PASS: 40 sources, DV in filename, DVD negative, no fictional sources, 40 episodes, focus/selection and episode title.");
}

static void capture(const char *name,SDL_Window *win,int panel) {
  for(int i=0;i<90;i++) {
    SDL_PumpEvents();txt_new_frame();tex_new_frame();tex_pump(6);
    player_update(1.f/60,SDL_GetTicks());episodes_update(1.f/60);
    stream_sheet_update(1.f/60,SDL_GetTicks());tracks_update(1.f/60,SDL_GetTicks());
    glClearColor(.025,.025,.03,1);glClear(GL_COLOR_BUFFER_BIT);
    if(panel<4) player_draw(SDL_GetTicks());
    if(panel==5) {
      profile_update(1.f/60,SDL_GetTicks());profile_draw(SDL_GetTicks());
    }
    if(panel==4) {
      CatItem c=*cat_item(0);c.season=2;c.episode=4;c.remainingMin=85;c.progress=34;
      strcpy(c.nameEpisode,"The Harmonium");
      for(int k=0;k<3;k++) {
        GfxRect r={180+k*470.f,360,440,248};
        gfx_color(r,.045f,.18f+.05f*k,.24f,.30f,1);
        resume_draw(&c,r);
      }
      menu_update(1.f/60,SDL_GetTicks());menu_draw(SDL_GetTicks());
    }
    if(panel==1) episodes_draw();
    if(panel==2) stream_sheet_draw(SDL_GetTicks());
    if(panel==3) tracks_draw(SDL_GetTicks());
    if(i==89) {
      unsigned char *pix=malloc(1920*1080*4);assert(pix);
      glReadPixels(0,0,1920,1080,GL_RGBA,GL_UNSIGNED_BYTE,pix);
      SDL_Surface *s=SDL_CreateRGBSurfaceWithFormat(0,1920,1080,32,SDL_PIXELFORMAT_RGBA32);assert(s);
      for(int y=0;y<1080;y++)memcpy((char*)s->pixels+y*s->pitch,pix+(1079-y)*1920*4,1920*4);
      assert(SDL_SaveBMP(s,name)==0);SDL_FreeSurface(s);free(pix);
    }
    SDL_GL_SwapWindow(win);SDL_Delay(8);
  }
}
int main(int argc,char **argv) {
  assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)==0);
  testar();
  if(argc<2)return 0;
  if(!strcmp(argv[1],"--social")) {
    net_prepare();assert(trakt_load("deploy/app/art"));
    CatItem *social=calloc(8,sizeof *social);assert(social);
    int n=trakt_social(social,8);assert(n>0);
    for(int i=0;i<n;i++)assert(social[i].imdb[0]&&social[i].pais[0]&&social[i].title[0]);
    printf("PASS: %d real activities, with author and title.\n",n);free(social);return 0;
  }
  if(!strcmp(argv[1],"--live")) {
    net_prepare();
    addons_load("deploy/app/art");addons_fetch("tt14688458:2:4","series");
    for(int i=0;i<900 && addons_state()==ADD_SEARCHING;i++)SDL_Delay(100);
    assert(addons_state()!=ADD_SEARCHING);
    int dv=0,mp4dv=0;
    for(int i=0;i<stream_n();i++) {
      const Stream *s=stream_item(i);dv+=!!s->dolbyVision;
      if(s->mp4 && s->dolbyVision) {mp4dv++;printf("MP4/DV at position %d of %d\n",i+1,stream_n());}
    }
    printf("LIVE Silo S2E4: %d direct sources, %d DV, %d MP4/DV (no URL exposed).\n",stream_n(),dv,mp4dv);
    addons_fetch_subtitles("tt14688458:2:4","series");
    for(int i=0;i<300 && addons_n_subtitles()==0;i++)SDL_Delay(100);
    assert(addons_n_subtitles()>0);
    for(int i=0;i<addons_n_subtitles();i++) {
      const Subtitle *l=addons_subtitle(i);
      assert(l && strstr(l->label,"T2E4"));
    }
    { const Subtitle *l=addons_subtitle(0);char *s=l?net_download(l->url,20):NULL;
      SubtitleCue *v=NULL;int n=s?subtitle_parse(s,&v):0;
      assert(n>10);free(v);free(s);
      printf("LIVE OpenSubtitles: file downloaded and parsed by the overlay (%d blocks).\n",n); }
    printf("LIVE OpenSubtitles: %d subtitles, all confirmed for S2E4 (no URL exposed).\n",
           addons_n_subtitles());
    addons_shutdown();
    if(trakt_load("deploy/app/art")) {
      extras_request("tt14688458",1,0);
      for(int i=0;i<300 && !extras_progress_ready();i++)SDL_Delay(100);
      int t=0,e=0;
      printf("LIVE Trakt: history=%s, next=%s",extras_progress_ready()?"recebido":"unavailable",extras_next_episode(&t,&e)?"sim":"not given");
      if(t&&e)printf(" T%dE%d",t,e);
      puts(" (read-only query)");
      if(t&&e) {
        CatItem c={0};strcpy(c.imdb,"tt14688458");strcpy(c.kind,"series");strcpy(c.title,"Silo");
        cat_set(&c,1);
        HomeItem it={0};it.index_=0;it.rect=(GfxRect){0,0,320,180};it.title=c.title;
        detail_open(&it);int dt=0,de=0;
        assert(detail_ep_focus(&dt,&de) && dt==t && de==e);
        puts("PASS: the Play button target matches the real next episode from Trakt.");
      }
    }
    return 0;
  }
  IMG_Init(IMG_INIT_PNG|IMG_INIT_JPG);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
  SDL_Window *w=SDL_CreateWindow("Nuvio: player review",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,1920,1080,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);assert(w);
  SDL_GLContext gl=SDL_GL_CreateContext(w);assert(gl);SDL_GL_SetSwapInterval(0);
  glViewport(0,0,1920,1080);gfx_size_target(1920,1080);assert(gfx_start());
  assert(txt_start("deploy/app",1));tex_start(64);
  gfx_icons_dir("deploy/app/art");
  if(!strcmp(argv[1],"--profile")) {
    ProfileData d={0};net_prepare();
    tex_cache_dir("deploy/app/art/cache");
    assert(trakt_load("deploy/app/art"));assert(trakt_profile(&d));
    profile_start();profile_open();profile_set_data(&d);
    capture("/tmp/nuvio-profile.bmp",w,5);
    SDL_Event e={0};e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_DOWN;
    profile_event(&e);capture("/tmp/nuvio-profile-calendar.bmp",w,5);
    for(int i=0;i<6;i++)profile_event(&e);
    capture("/tmp/nuvio-profile-ranks.bmp",w,5);
    profile_open_side();capture("/tmp/nuvio-profile-sidebar.bmp",w,5);
    return 0;
  }
  cat_load("deploy/app/art");
  CatItem c={0};if(cat_n())c=*cat_item(0);
  for(int i=0;i<cat_n();i++) if(!strcmp(cat_item(i)->title,"Silo")){c=*cat_item(i);break;}
  /* The package may not contain Silo: do not match another title's logo. */
  if(strcmp(c.title,"Silo"))c.logo[0]=c.backdrop[0]=c.poster[0]=0;
  c.imdb[0]=0;snprintf(c.kind,sizeof c.kind,"series");snprintf(c.title,sizeof c.title,"Silo");
  c.nSeasons=3;c.seasons[0]=1;c.seasons[1]=2;c.seasons[2]=3;cat_set(&c,1);
  const char *names[]={"The Engineer","Order","Solo","The Harmonium","Descent"};
  CatEp ep[5]={0};
  for(int i=0;i<5;i++) {
    ep[i].season=2;ep[i].episode=i+1;snprintf(ep[i].name,sizeof ep[i].name,"%s",names[i]);
    snprintf(ep[i].date,sizeof ep[i].date,"5 de dezembro de 2024");snprintf(ep[i].duration,sizeof ep[i].duration,"53 min");
    snprintf(ep[i].synopsis,sizeof ep[i].synopsis,"Juliette sets out on a dangerous quest to retrieve a suit so she can return home.");
  }
  cat_set_episodes(0,ep,5);player_open(0,NULL);player_set_episode(2,4);
  capture("/tmp/nuvio-player-loading.bmp",w,0);
  episodes_open(0,2,4);capture("/tmp/nuvio-player-episodes.bmp",w,1);episodes_close();
  Stream s[5]={0};
  for(int i=0;i<5;i++) {
    snprintf(s[i].label,sizeof s[i].label,"Silo S02 E04");snprintf(s[i].provider,sizeof s[i].provider,"Addon de teste");
    snprintf(s[i].description,sizeof s[i].description,"English | Portuguese\nSilo.S02E04.2160p.WEB.DV.Atmos.mp4");
    s[i].mp4=s[i].dolbyVision=s[i].dolbyAtmos=1;s[i].height=2160;s[i].sizeMB=11264;
  }
  stream_set_list(s,5);stream_sheet_context(player_line_episode());stream_sheet_open();
  capture("/tmp/nuvio-player-sources.bmp",w,2);
  tracks_open_em(1);capture("/tmp/nuvio-player-subtitles.bmp",w,3);
  menu_start();capture("/tmp/nuvio-player-cards.bmp",w,4);
  menu_open();capture("/tmp/nuvio-player-menu.bmp",w,4);
  tex_shutdown();txt_shutdown();gfx_shutdown();SDL_GL_DeleteContext(gl);SDL_DestroyWindow(w);SDL_Quit();
  puts("PASS: six review captures in /tmp/nuvio-player-*.bmp (test data, no real playback).");return 0;
}
