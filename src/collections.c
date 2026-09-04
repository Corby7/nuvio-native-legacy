#include "collections.h"
#include "js.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
static ColFolder folders[COL_MAX];
static int count;
static void locates(char *value,size_t cap,const char *dir) {
  if(!value[0]||strstr(value,"://")||value[0]=='/')return;
  char rel[600];snprintf(rel,sizeof rel,"%s",value);snprintf(value,cap,"%s/%s",dir,rel);
}
int col_n(void) { return count; }
const ColFolder *col_folder(int i) { return i>=0&&i<count?&folders[i]:NULL; }
int col_group(const char *name,int *indices,int max) {
  int n=0;for(int i=0;i<count&&n<max;i++) if(!strcasecmp(name,folders[i].group)) indices[n++]=i;return n;
}
const ColFolder *col_by_catalog(const char *base,const char *type,const char *id) {
  for(int i=0;i<count;i++) for(int s=0;s<folders[i].nSources;s++) {
    const ColSource *v=&folders[i].sources[s];
    if(!strcmp(v->base,base)&&!strcmp(v->type,type)&&!strcmp(v->catId,id)) return &folders[i];
  }return NULL;
}
int col_load(const char *dir) {
  char path[700];snprintf(path,sizeof path,"%s/collections.json",dir);
  FILE *f=fopen(path,"rb");if(!f)return 0;
  fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
  if(size<2||size>4000000){fclose(f);return 0;}
  char *body=malloc((size_t)size+1);if(!body){fclose(f);return 0;}
  size_t got=fread(body,1,(size_t)size,f);body[got]=0;fclose(f);count=0;
  for(const char *g=js_array(body,NULL,"groups");g;g=js_next(js_end(g))) {
    const char *end=js_end(g);char group[64];js_text(g,end,"title",group,sizeof group);
    for(const char *p=js_array(g,end,"folders");p&&count<COL_MAX;p=js_next(js_end(p))) {
      const char *pe=js_end(p);ColFolder *v=&folders[count];memset(v,0,sizeof *v);
      snprintf(v->group,sizeof v->group,"%s",group);
      js_text(p,pe,"id",v->id,sizeof v->id);js_text(p,pe,"title",v->title,sizeof v->title);
      js_text(p,pe,"cover",v->cover,sizeof v->cover);js_text(p,pe,"hero",v->hero,sizeof v->hero);js_text(p,pe,"logo",v->logo,sizeof v->logo);
      locates(v->cover,sizeof v->cover,dir);locates(v->hero,sizeof v->hero,dir);locates(v->logo,sizeof v->logo,dir);
      v->hideTitle=js_num(p,pe,"hideTitle",0);v->frames=js_num(p,pe,"frames",0);
      if(v->frames<0||v->frames>90)v->frames=0;
      snprintf(v->frameDir,sizeof v->frameDir,"%s/collections/%s",dir,v->id);
      /* Local paired artwork survives catalog imports. Activate only a complete pair. */
      char editorial[512];
      snprintf(editorial,sizeof editorial,"%s/editorial/%s-home.png",dir,v->id);
      snprintf(v->detailHero,sizeof v->detailHero,"%s/editorial/%s-detail.png",dir,v->id);
      if(!access(editorial,R_OK)&&!access(v->detailHero,R_OK)) {
        snprintf(v->hero,sizeof v->hero,"%s",editorial);v->editorial=1;
      } else v->detailHero[0]=0;
      for(const char *s=js_array(p,pe,"sources");s&&v->nSources<COL_SOURCE_MAX;s=js_next(js_end(s))) {
        const char *se=js_end(s);ColSource *a=&v->sources[v->nSources];
        js_text(s,se,"title",a->title,sizeof a->title);js_text(s,se,"base",a->base,sizeof a->base);
        js_text(s,se,"type",a->type,sizeof a->type);js_text(s,se,"catId",a->catId,sizeof a->catId);js_text(s,se,"genre",a->genre,sizeof a->genre);
        if(a->base[0]&&a->type[0]&&a->catId[0])v->nSources++;
      }
      if(v->nSources&&v->title[0])count++;
    }
  }free(body);return count;
}
void col_color(const ColFolder *f,float *r,float *g,float *b) {
  *r=.16f;*g=.23f;*b=.30f;if(!f)return;
  if(strstr(f->title,"Netflix")){*r=.52f;*g=.035f;*b=.065f;}
  else if(strstr(f->title,"Prime")){*r=.025f;*g=.32f;*b=.58f;}
  else if(strstr(f->title,"Disney")){*r=.10f;*g=.13f;*b=.46f;}
  else if(strstr(f->title,"Max")||strstr(f->title,"HBO")){*r=.27f;*g=.12f;*b=.44f;}
  else if(strstr(f->title,"Letterboxd")){*r=.07f;*g=.32f;*b=.21f;}
  else if(!strcmp(f->group,"Awards")){*r=.40f;*g=.31f;*b=.095f;}
  else if(!strcmp(f->group,"Directors")){*r=.29f;*g=.24f;*b=.19f;}
}
