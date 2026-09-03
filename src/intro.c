#include "intro.h"
#include "rede.h"
#include "js.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t trava=PTHREAD_MUTEX_INITIALIZER;
static IntroTrecho trechos[3];static int nTrechos;static unsigned geracao;

int intro_extrair(const char *j,IntroTrecho *out,int max){
  const char *nomes[]={"intro","recap","outro"};int tipos[]={INTRO_ABERTURA,INTRO_RESUMO,INTRO_CREDITOS},n=0;
  if(!j||!out||max<1)return 0;
  for(int i=0;i<3&&n<max;i++){
    char chave[24];snprintf(chave,sizeof chave,"\"%s\"",nomes[i]);const char *p=strstr(j,chave);if(!p)continue;
    p=strchr(p,':');if(!p)continue;p++;while(*p==' '||*p=='\t')p++;if(*p!='{')continue;
    const char *f=js_fim(p);double a=js_num(p,f,"start_sec",-1),b=js_num(p,f,"end_sec",-1);
    if(a>=0&&b>a)out[n++]=(IntroTrecho){a,b,tipos[i]};
  }return n;
}
typedef struct{char url[256];unsigned g;}Pedido;
static void *baixar(void *u){Pedido*p=u;char*j=rede_baixar(p->url,12);IntroTrecho v[3];int n=j?intro_extrair(j,v,3):0;free(j);
 pthread_mutex_lock(&trava);if(p->g==geracao){memcpy(trechos,v,(size_t)n*sizeof *v);nTrechos=n;}pthread_mutex_unlock(&trava);
 printf("[intro] %d marcadores\n",n);fflush(stdout);free(p);return NULL;}
void intro_pedir(const char *imdb,int t,int e){Pedido*p;pthread_t fio;if(!imdb||strncmp(imdb,"tt",2)||t<1||e<1){intro_desligar();return;}
 p=calloc(1,sizeof*p);if(!p)return;pthread_mutex_lock(&trava);nTrechos=0;p->g=++geracao;pthread_mutex_unlock(&trava);
 snprintf(p->url,sizeof p->url,"https://api.introdb.app/segments?imdb_id=%.*s&season=%d&episode=%d",(int)strcspn(imdb,":"),imdb,t,e);
 if(pthread_create(&fio,NULL,baixar,p)==0)pthread_detach(fio);else free(p);}
void intro_desligar(void){pthread_mutex_lock(&trava);geracao++;nTrechos=0;pthread_mutex_unlock(&trava);}
int intro_ativo(double pos,double*fim,int*tipo){int ok=0;pthread_mutex_lock(&trava);for(int i=0;i<nTrechos;i++)if(pos>=trechos[i].inicio&&pos<trechos[i].fim){if(fim)*fim=trechos[i].fim;if(tipo)*tipo=trechos[i].tipo;ok=1;break;}pthread_mutex_unlock(&trava);return ok;}
