#include "intro.h"
#include "net.h"
#include "js.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static IntroChunk chunks[3];static int nChunks;static unsigned generation;

int intro_parse(const char *j,IntroChunk *out,int max){
  const char *names[]={"intro","recap","outro"};int kinds[]={INTRO_OPENING,INTRO_SUMMARY,INTRO_CREDITS},n=0;
  if(!j||!out||max<1)return 0;
  for(int i=0;i<3&&n<max;i++){
    char key[24];snprintf(key,sizeof key,"\"%s\"",names[i]);const char *p=strstr(j,key);if(!p)continue;
    p=strchr(p,':');if(!p)continue;p++;while(*p==' '||*p=='\t')p++;if(*p!='{')continue;
    const char *f=js_end(p);double a=js_num(p,f,"start_sec",-1),b=js_num(p,f,"end_sec",-1);
    if(a>=0&&b>a)out[n++]=(IntroChunk){a,b,kinds[i]};
  }return n;
}
typedef struct{char url[256];unsigned g;}Request;
static void *download(void *u){Request*p=u;char*j=net_download(p->url,12);IntroChunk v[3];int n=j?intro_parse(j,v,3):0;free(j);
 pthread_mutex_lock(&lock);if(p->g==generation){memcpy(chunks,v,(size_t)n*sizeof *v);nChunks=n;}pthread_mutex_unlock(&lock);
 printf("[intro] %d marcadores\n",n);fflush(stdout);free(p);return NULL;}
void intro_request(const char *imdb,int t,int e){Request*p;pthread_t thread;if(!imdb||strncmp(imdb,"tt",2)||t<1||e<1){intro_off();return;}
 p=calloc(1,sizeof*p);if(!p)return;pthread_mutex_lock(&lock);nChunks=0;p->g=++generation;pthread_mutex_unlock(&lock);
 snprintf(p->url,sizeof p->url,"https://api.introdb.app/segments?imdb_id=%.*s&season=%d&episode=%d",(int)strcspn(imdb,":"),imdb,t,e);
 if(pthread_create(&thread,NULL,download,p)==0)pthread_detach(thread);else free(p);}
void intro_off(void){pthread_mutex_lock(&lock);generation++;nChunks=0;pthread_mutex_unlock(&lock);}
int intro_active(double pos,double*end,int*kind){int ok=0;pthread_mutex_lock(&lock);for(int i=0;i<nChunks;i++)if(pos>=chunks[i].start&&pos<chunks[i].end){if(end)*end=chunks[i].end;if(kind)*kind=chunks[i].kind;ok=1;break;}pthread_mutex_unlock(&lock);return ok;}
