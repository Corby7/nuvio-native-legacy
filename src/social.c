#include "social.h"
#include "trakt.h"
#include "rede.h"
#include "js.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "layout.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Snapshot publicado por geracao. Nunca espera HTTP no fio de desenho.
typedef struct { char titulo[160], detalhe[160], data[24]; } Atividade;
typedef struct {
  unsigned geracao; CatItem pessoa; char bio[640], local[160];
  int erro, erroHistorico, n; Atividade atividades[20];
} SocialDados;
static pthread_mutex_t trava=PTHREAD_MUTEX_INITIALIZER;
static SocialDados dados, pronto;
static unsigned geracao;
static int temPronto, carregando, sair, selecionado;

static int slugValido(const char *s) {
  return s[0] && strspn(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==strlen(s);
}
static void *carregar(void *arg) {
  SocialDados *d=arg; const char *cab[4];char aut[200],key[140],url[512];
  if(!slugValido(d->pessoa.socialSlug) || !trakt_cabecalhos(cab,aut,sizeof aut,key,sizeof key))d->erro=1;
  else {
    snprintf(url,sizeof url,"https://api.trakt.tv/users/%s?extended=full",d->pessoa.socialSlug);
    char *body=rede_baixar_com(url,10,cab);
    if(!body)d->erro=1;
    else {
      js_texto(body,NULL,"name",d->pessoa.socialNome,sizeof d->pessoa.socialNome);
      js_texto(body,NULL,"about",d->bio,sizeof d->bio);
      js_texto(body,NULL,"location",d->local,sizeof d->local);
      const char *av=strstr(body,"\"avatar\"");
      if(av)js_texto(av,NULL,"full",d->pessoa.socialAvatar,sizeof d->pessoa.socialAvatar);
      free(body);
    }
    snprintf(url,sizeof url,"https://api.trakt.tv/users/%s/history?limit=20&extended=full",d->pessoa.socialSlug);
    body=rede_baixar_com(url,12,cab);
    if(!body)d->erroHistorico=1;
    else {
      const char *p=strchr(body,'[');p=p?p+1:NULL;
      while(p && *p && d->n<20) {
        while(*p && (unsigned char)*p<=' ')p++;
        if(*p!='{')break;
        const char *f=js_fim(p),*m=strstr(p,"\"movie\""),*s=strstr(p,"\"show\""),*ep=strstr(p,"\"episode\"");
        const char *t=s&&s<f?s:m&&m<f?m:NULL;
        if(t) {
          Atividade *a=&d->atividades[d->n]; const char *o=strchr(t,'{');const char *tf=o?js_fim(o):NULL;
          if(o&&tf&&js_texto(o,tf,"title",a->titulo,sizeof a->titulo)) {
            if(ep&&ep<f){const char *eo=strchr(ep,'{'),*ef=eo?js_fim(eo):NULL;char nome[100]="";
              js_texto(eo,ef,"title",nome,sizeof nome);
              snprintf(a->detalhe,sizeof a->detalhe,"T%dE%d · %s",(int)js_num(eo,ef,"season",0),(int)js_num(eo,ef,"number",0),nome);
            } else snprintf(a->detalhe,sizeof a->detalhe,"Filme");
            js_texto(p,f,"watched_at",a->data,sizeof a->data);a->data[10]=0;d->n++;
          }
        }
        p=js_prox(f);
      }
      free(body);
    }
  }
  pthread_mutex_lock(&trava);
  if(d->geracao==geracao){pronto=*d;temPronto=1;}
  pthread_mutex_unlock(&trava);free(d);return NULL;
}
void social_abrir(const CatItem *pessoa) {
  CatItem copia={0};if(pessoa)copia=*pessoa;
  pthread_mutex_lock(&trava);geracao++;temPronto=0;pthread_mutex_unlock(&trava);
  memset(&dados,0,sizeof dados);dados.pessoa=copia;dados.geracao=geracao;
  sair=0;selecionado=0;carregando=1;
  SocialDados *t=malloc(sizeof *t);pthread_t fio;
  if(!t){dados.erro=1;carregando=0;return;}*t=dados;
  if(pthread_create(&fio,NULL,carregar,t)){free(t);dados.erro=1;carregando=0;}else pthread_detach(fio);
}
int social_quer_sair(void){int v=sair;sair=0;return v;}
void social_evento(const SDL_Event *e) {
  if(e->type!=SDL_KEYDOWN)return;
  SDL_Keycode k=e->key.keysym.sym;
  if(k==SDLK_ESCAPE||k==SDLK_AC_BACK||k==SDLK_BACKSPACE||k==SDLK_LEFT){sair=1;return;}
  if(k==SDLK_UP&&selecionado>0)selecionado--;
  if(k==SDLK_DOWN&&selecionado<dados.n-1)selecionado++;
  if((k==SDLK_RETURN||k==SDLK_KP_ENTER)&&!carregando&&(dados.erro||dados.erroHistorico))social_abrir(&dados.pessoa);
}
void social_atualizar(float dt,Uint32 agora){(void)dt;(void)agora;
  pthread_mutex_lock(&trava);if(temPronto){dados=pronto;temPronto=0;carregando=0;}pthread_mutex_unlock(&trava);
}
static void texto(TxtEstilo estilo,const char *s,float x,float y,float w,int cor){txt_desenhar(txt_linha_corta(estilo,s,cor,cor,cor,255,w),x,y);}
void social_desenhar(Uint32 agora) {
  (void)agora;
  gfx_cor((GfxRect){0,0,NV_TELA_W,NV_TELA_H},0,NV_COR_FUNDO_R,NV_COR_FUNDO_G,NV_COR_FUNDO_B,1);
  texto(TXT_CAPTION,"ENTRE AMIGOS",96,56,550,179);
  GfxRect av={96,132,176,176};GLuint tex=dados.pessoa.socialAvatar[0]?tex_obter_larg(dados.pessoa.socialAvatar,220):0;
  gfx_cor(av,.5f,.15f,.16f,.18f,1);
  if(tex){gfx_tex_aspect_atual=tex_aspecto(dados.pessoa.socialAvatar);gfx_rect(av,tex,GFX_AVATAR,0,0,0,0,1,1,1,1);gfx_tex_aspect_atual=0;}
  else gfx_icone((GfxRect){148,184,72,72},"menu_profile",.8f,.81f,.83f,1);
  texto(TXT_TITULO2,dados.pessoa.socialNome[0]?dados.pessoa.socialNome:dados.pessoa.pais,96,346,480,245);
  char usuario[160];snprintf(usuario,sizeof usuario,"@%s",dados.pessoa.socialSlug);
  texto(TXT_CALLOUT,usuario,96,416,480,179);
  if(dados.local[0])texto(TXT_CAPTION,dados.local,96,470,480,179);
  if(dados.bio[0])txt_bloco(TXT_CAPTION,dados.bio,210,210,210,96,524,480,32,1,8);
  texto(TXT_CAPTION,"Trakt · Perfil público",96,900,480,179);
  texto(TXT_CAPTION,"← Voltar",96,970,480,235);
  texto(TXT_TITULO3,"Atividade recente",656,64,1150,245);
  texto(TXT_CAPTION,"Últimos registros compartilhados pela pessoa",656,122,1150,179);
  if(carregando){for(int i=0;i<4;i++){gfx_cor((GfxRect){656,210+i*170,1120,134},.06f,.12f,.125f,.14f,1);}
    texto(TXT_CAPTION,"Carregando atividade…",680,248,1050,210);return;}
  if(dados.erro||dados.erroHistorico){texto(TXT_TITULO3,"Não foi possível carregar tudo",656,236,1120,235);
    texto(TXT_CALLOUT,"Perfil privado ou serviço indisponível.",656,298,1120,179);
    texto(TXT_CALLOUT,"OK: tentar novamente",656,366,1120,235);return;}
  if(!dados.n){texto(TXT_TITULO3,"Sem atividade pública por enquanto",656,236,1120,235);return;}
  int inicio=selecionado>3?selecionado-3:0;
  for(int i=inicio;i<dados.n&&i<inicio+5;i++) {
    float y=200+(i-inicio)*154;Atividade *a=&dados.atividades[i];
    if(i==selecionado)gfx_cor((GfxRect){640,y-12,1160,144},.07f,.18f,.185f,.20f,1);
    texto(TXT_MINI,a->data,664,y,180,179);
    texto(TXT_CALLOUT,a->titulo,850,y,900,245);
    texto(TXT_CAPTION,a->detalhe,850,y+49,900,179);
  }
}
