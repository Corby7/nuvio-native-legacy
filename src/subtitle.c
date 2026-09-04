#include "subtitle.h"
#include "net.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SubtitleCue *cues;
static int nCues, on;
static unsigned generation;

static double parseTime(const char *s) {
  int h=0,m=0; double seg=0;
  if (sscanf(s,"%d:%d:%lf",&h,&m,&seg)==3) return h*3600.0+m*60.0+seg;
  if (sscanf(s,"%d:%lf",&m,&seg)==2) return m*60.0+seg;
  return -1;
}

static void entity(char *s) {
  char *r=s,*w=s;
  while (*r) {
    if (*r=='<' ) {
      if (!strncasecmp(r,"<br",3)) { *w++='\n'; }
      while (*r && *r!='>') r++;
      if (*r) r++;
    } else if (!strncmp(r,"&amp;",5))  { *w++='&'; r+=5; }
    else if (!strncmp(r,"&lt;",4))   { *w++='<'; r+=4; }
    else if (!strncmp(r,"&gt;",4))   { *w++='>'; r+=4; }
    else if (!strncmp(r,"&quot;",6)) { *w++='"'; r+=6; }
    else if (!strncmp(r,"&#39;",5))  { *w++='\''; r+=5; }
    else *w++=*r++;
  }
  *w=0;
}

int subtitle_parse(const char *body, SubtitleCue **output) {
  char *buf,*p,*line; int n=0,cap=128;
  SubtitleCue *v;
  if (output) *output=NULL;
  if (!body || !output) return 0;
  buf=strdup(body); if(!buf)return 0;
  v=calloc((size_t)cap,sizeof *v); if(!v){free(buf);return 0;}
  p=buf;
  if ((unsigned char)p[0]==0xef && (unsigned char)p[1]==0xbb && (unsigned char)p[2]==0xbf) p+=3;
  while (*p) {
    char *next=strchr(p,'\n');
    if(next)*next++=0;
    { char *q=strchr(p,'\r'); if(q)*q=0; }
    line=p; p=next?next:p+strlen(p);
    if (!strstr(line,"-->")) continue;
    char *seta=strstr(line,"-->"); *seta=0; seta+=3;
    while(isspace((unsigned char)*seta))seta++;
    for(char *q=line;*q;q++)if(*q==',')*q='.';
    for(char *q=seta;*q;q++)if(*q==',')*q='.';
    double start=parseTime(line),end=parseTime(seta);
    if(start<0||end<=start)continue;
    char text[768]={0}; size_t used=0;
    while(*p) {
      char *nl=strchr(p,'\n'); if(nl)*nl++=0;
      { char *q=strchr(p,'\r');if(q)*q=0; }
      if(!*p){p=nl?nl:p;break;}
      size_t l=strlen(p),remains=sizeof text-used-1;
      if(used&&remains){text[used++]='\n';remains--;}
      if(l>remains)l=remains;memcpy(text+used,p,l);used+=l;text[used]=0;
      p=nl?nl:p+strlen(p);
    }
    entity(text); if(!text[0])continue;
    if(n==cap){cap*=2;SubtitleCue *nv=realloc(v,(size_t)cap*sizeof *v);if(!nv)break;v=nv;}
    v[n].start=start;v[n].end=end;snprintf(v[n].text,sizeof v[n].text,"%s",text);n++;
  }
  free(buf);
  if(!n){free(v);return 0;}
  *output=v;return n;
}

typedef struct { char url[1400]; unsigned g; } Request;
static void *download(void *u) {
  Request *p=u; char *body=net_download(p->url,20); SubtitleCue *v=NULL;
  int n=body?subtitle_parse(body,&v):0; free(body);
  pthread_mutex_lock(&lock);
  if(p->g==generation&&on){free(cues);cues=v;nCues=n;v=NULL;}
  pthread_mutex_unlock(&lock);
  free(v);printf("[subtitle] OpenSubtitles: %d blocks%s\n",n,n?"":" (failed)");fflush(stdout);
  free(p);return NULL;
}

void subtitle_load(const char *url) {
  Request *p; pthread_t thread;
  if(!url||!*url)return;
  p=calloc(1,sizeof *p);if(!p)return;
  pthread_mutex_lock(&lock);on=1;p->g=++generation;free(cues);cues=NULL;nCues=0;pthread_mutex_unlock(&lock);
  snprintf(p->url,sizeof p->url,"%s",url);
  if(pthread_create(&thread,NULL,download,p)==0)pthread_detach(thread);else free(p);
}

void subtitle_off(void) {
  pthread_mutex_lock(&lock);on=0;generation++;free(cues);cues=NULL;nCues=0;pthread_mutex_unlock(&lock);
}

int subtitle_text(double posSeg,int delayMs,char *dst,size_t size) {
  int ok=0,lo=0,hi; double t=posSeg+(double)delayMs/1000.0;
  if(!dst||!size)return 0;dst[0]=0;
  pthread_mutex_lock(&lock);hi=nCues-1;
  while(lo<=hi){int m=(lo+hi)/2;if(t<cues[m].start)hi=m-1;else if(t>cues[m].end)lo=m+1;else{snprintf(dst,size,"%s",cues[m].text);ok=1;break;}}
  pthread_mutex_unlock(&lock);return ok;
}
