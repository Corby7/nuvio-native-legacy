#include "legenda.h"
#include "rede.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;
static LegendaCue *cues;
static int nCues, ligada;
static unsigned geracao;

static double tempo(const char *s) {
  int h=0,m=0; double seg=0;
  if (sscanf(s,"%d:%d:%lf",&h,&m,&seg)==3) return h*3600.0+m*60.0+seg;
  if (sscanf(s,"%d:%lf",&m,&seg)==2) return m*60.0+seg;
  return -1;
}

static void entidade(char *s) {
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

int legenda_extrair(const char *corpo, LegendaCue **saida) {
  char *buf,*p,*linha; int n=0,cap=128;
  LegendaCue *v;
  if (saida) *saida=NULL;
  if (!corpo || !saida) return 0;
  buf=strdup(corpo); if(!buf)return 0;
  v=calloc((size_t)cap,sizeof *v); if(!v){free(buf);return 0;}
  p=buf;
  if ((unsigned char)p[0]==0xef && (unsigned char)p[1]==0xbb && (unsigned char)p[2]==0xbf) p+=3;
  while (*p) {
    char *proxima=strchr(p,'\n');
    if(proxima)*proxima++=0;
    { char *q=strchr(p,'\r'); if(q)*q=0; }
    linha=p; p=proxima?proxima:p+strlen(p);
    if (!strstr(linha,"-->")) continue;
    char *seta=strstr(linha,"-->"); *seta=0; seta+=3;
    while(isspace((unsigned char)*seta))seta++;
    for(char *q=linha;*q;q++)if(*q==',')*q='.';
    for(char *q=seta;*q;q++)if(*q==',')*q='.';
    double ini=tempo(linha),fim=tempo(seta);
    if(ini<0||fim<=ini)continue;
    char texto[768]={0}; size_t usado=0;
    while(*p) {
      char *nl=strchr(p,'\n'); if(nl)*nl++=0;
      { char *q=strchr(p,'\r');if(q)*q=0; }
      if(!*p){p=nl?nl:p;break;}
      size_t l=strlen(p),resta=sizeof texto-usado-1;
      if(usado&&resta){texto[usado++]='\n';resta--;}
      if(l>resta)l=resta;memcpy(texto+usado,p,l);usado+=l;texto[usado]=0;
      p=nl?nl:p+strlen(p);
    }
    entidade(texto); if(!texto[0])continue;
    if(n==cap){cap*=2;LegendaCue *nv=realloc(v,(size_t)cap*sizeof *v);if(!nv)break;v=nv;}
    v[n].inicio=ini;v[n].fim=fim;snprintf(v[n].texto,sizeof v[n].texto,"%s",texto);n++;
  }
  free(buf);
  if(!n){free(v);return 0;}
  *saida=v;return n;
}

typedef struct { char url[1400]; unsigned g; } Pedido;
static void *baixar(void *u) {
  Pedido *p=u; char *corpo=rede_baixar(p->url,20); LegendaCue *v=NULL;
  int n=corpo?legenda_extrair(corpo,&v):0; free(corpo);
  pthread_mutex_lock(&trava);
  if(p->g==geracao&&ligada){free(cues);cues=v;nCues=n;v=NULL;}
  pthread_mutex_unlock(&trava);
  free(v);printf("[legenda] OpenSubtitles: %d blocos%s\n",n,n?"":" (falha)");fflush(stdout);
  free(p);return NULL;
}

void legenda_carregar(const char *url) {
  Pedido *p; pthread_t fio;
  if(!url||!*url)return;
  p=calloc(1,sizeof *p);if(!p)return;
  pthread_mutex_lock(&trava);ligada=1;p->g=++geracao;free(cues);cues=NULL;nCues=0;pthread_mutex_unlock(&trava);
  snprintf(p->url,sizeof p->url,"%s",url);
  if(pthread_create(&fio,NULL,baixar,p)==0)pthread_detach(fio);else free(p);
}

void legenda_desligar(void) {
  pthread_mutex_lock(&trava);ligada=0;geracao++;free(cues);cues=NULL;nCues=0;pthread_mutex_unlock(&trava);
}

int legenda_texto(double posSeg,int atrasoMs,char *dst,size_t tam) {
  int ok=0,lo=0,hi; double t=posSeg+(double)atrasoMs/1000.0;
  if(!dst||!tam)return 0;dst[0]=0;
  pthread_mutex_lock(&trava);hi=nCues-1;
  while(lo<=hi){int m=(lo+hi)/2;if(t<cues[m].inicio)hi=m-1;else if(t>cues[m].fim)lo=m+1;else{snprintf(dst,tam,"%s",cues[m].texto);ok=1;break;}}
  pthread_mutex_unlock(&trava);return ok;
}
