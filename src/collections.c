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
// Revisao das colecoes. A home decide se precisa remontar as fileiras por um
// hash das fileiras do CATALOGO; sem este numero entrando na conta, uma colecao
// que chega da conta fica na memoria e nunca aparece na tela.
static unsigned revisionCol = 1;
unsigned col_revision(void) { return revisionCol; }

// Grupos que o dono fixou no topo (`pinToTop`).
#define COL_PINNED_MAX 24
static char pinned[COL_PINNED_MAX][64];
static int nPinned;

int col_group_pinned(const char *group) {
  int i;
  if (!group || !group[0]) return 0;
  for (i = 0; i < nPinned; i++) if (!strcasecmp(pinned[i], group)) return 1;
  return 0;
}

// Buffer de leitura, fora da pilha: a URL de um addon configurado passa
// facil de 4 KB e uma variavel local desse tamanho por fonte nao cabe no
// fio do desenho.
static char scratch[8192];

int col_load_account(const char *body) {
  const char *root, *end, *c;
  const char *list = NULL;
  int added = 0, skippedNoSource = 0, nTmdb = 0, nTrakt = 0, nTooLong = 0;
  if (!body || !*body) return 0;
  end = body + strlen(body);
  // O corpo e [{"collections_json":[...]}]. Se o servidor devolver a lista
  // direto, ou sob outro nome, tenta os dois antes de desistir — e diz qual foi.
  root = js_root_array(body);
  if (root) list = js_array(root, js_end(root), "collections_json");
  if (!list) list = js_array(body, end, "collections");
  if (!list) {
    printf("[col] account collections: no list found in the response\n");
    fflush(stdout);
    return 0;
  }
  for (; list; list = js_next(js_end(list))) {
    const char *ce = js_end(list);
    char group[64] = "";
    int pin;
    js_text(list, ce, "title", group, sizeof group);
    if (!group[0]) continue;
    pin = js_flag(list, ce, "pinToTop", 0);
    if (pin && nPinned < COL_PINNED_MAX)
      snprintf(pinned[nPinned++], 64, "%s", group);
    for (c = js_array(list, ce, "folders"); c && count < COL_MAX;
         c = js_next(js_end(c))) {
      const char *fe = js_end(c);
      ColFolder *v = &folders[count];
      const char *s;
      memset(v, 0, sizeof *v);
      snprintf(v->group, sizeof v->group, "%s", group);
      js_text(c, fe, "id",    v->id,    sizeof v->id);
      js_text(c, fe, "title", v->title, sizeof v->title);
      // Arte remota: URLs de CDN. `locates` deixa passar o que tem "://", entao
      // nao ha caminho local sendo inventado por cima delas, e o tex_get sabe
      // buscar http(s).
      js_text(c, fe, "coverImageUrl",   v->cover, sizeof v->cover);
      js_text(c, fe, "heroBackdropUrl", v->hero,  sizeof v->hero);
      js_text(c, fe, "titleLogoUrl",    v->logo,  sizeof v->logo);
      v->hideTitle = js_flag(c, fe, "hideTitle", 0);
      v->frames = 0;          // nao ha pasta de quadros para colecao da conta
      v->frameDir[0] = 0;
      // `catalogSources` e a lista ja filtrada para fontes de ADDON, que sao as
      // unicas que este app sabe buscar. `sources` cobre o formato antigo.
      s = js_array(c, fe, "catalogSources");
      if (!s) s = js_array(c, fe, "sources");
      for (; s && v->nSources < COL_SOURCE_MAX; s = js_next(js_end(s))) {
        const char *se = js_end(s);
        ColSource *a = &v->sources[v->nSources];
        char provider[24] = "";
        // Uma fonte de colecao pode ser de tres tipos, e so um deles e um
        // catalogo de addon. Contar por tipo transforma "29 puladas" em uma
        // frase que diz o que faltaria implementar para cada uma.
        js_text(s, se, "provider", provider, sizeof provider);
        if (!strcasecmp(provider, "tmdb"))  { nTmdb++;  continue; }
        if (!strcasecmp(provider, "trakt")) { nTrakt++; continue; }
        memset(a, 0, sizeof *a);
        // O ENDERECO DO ADDON NAO CABE, e este e o motivo real de nenhuma
        // colecao aparecer. MEDIDO nesta conta: o `addonBaseUrl` de uma fonte
        // do AIOMetadata passa de 4096 bytes, porque estes addons carregam a
        // configuracao inteira dentro da propria URL. O campo aqui tem 600.
        //
        // js_text recusa em silencio quando nao cabe (devolve 0 e nao escreve),
        // entao a fonte era descartada mais abaixo por "falta base" — uma
        // explicacao errada, que mandava procurar no lugar errado.
        //
        // Aumentar o campo nao resolve: sao COL_MAX x COL_SOURCE_MAX fontes, e
        // 4 KB em cada uma passaria de 30 MB de estatico num aparelho que hoje
        // usa 78 MB no total.
        { int tooLong = 0;
          if (js_text(s, se, "addonBaseUrl", scratch, sizeof scratch)) {
            if (strlen(scratch) < sizeof a->base)
              snprintf(a->base, sizeof a->base, "%s", scratch);
            else tooLong = 1;
          } else tooLong = 1;      // nem em 8 KB
          // A contagem e cumulativa; a decisao NAO pode ser. Testar o total aqui
          // desligaria o campo alternativo para todas as fontes seguintes assim
          // que a primeira estourasse.
          if (tooLong) nTooLong++;
          else if (!a->base[0]) js_text(s, se, "base", a->base, sizeof a->base); }
        js_text(s, se, "type",      a->type,  sizeof a->type);
        js_text(s, se, "catalogId", a->catId, sizeof a->catId);
        if (!a->catId[0]) js_text(s, se, "catId", a->catId, sizeof a->catId);
        js_text(s, se, "title", a->title, sizeof a->title);
        if (!a->title[0]) js_text(s, se, "catalogName", a->title, sizeof a->title);
        js_text(s, se, "genre", a->genre, sizeof a->genre);
        if (a->base[0] && a->type[0] && a->catId[0]) v->nSources++;
      }
      // Uma pasta sem fonte de addon nao tem de onde tirar titulo nenhum — e o
      // caso de uma pasta montada sobre filtros do TMDB, que este app nao busca.
      // Contar e dizer quantas foram e melhor do que uma fileira vazia.
      if (v->nSources && v->title[0]) { count++; added++; }
      else if (v->title[0]) skippedNoSource++;
    }
  }
  if (added) revisionCol++;
  printf("[col] account collections: %d folder(s) added, %d skipped\n",
         added, skippedNoSource);
  // O QUE FALTARIA para as puladas. Sao caminhos de busca inteiros que este app
  // nao tem: o TMDB aqui so serve elenco e arte, nao descoberta por filtro, e as
  // listas do Trakt sao buscadas por id, que tambem nao existe.
  if (nTmdb || nTrakt)
    printf("[col] sources this app cannot fetch: %d TMDB, %d Trakt\n",
           nTmdb, nTrakt);
  if (nTooLong)
    printf("[col] %d source(s) dropped: the addon URL is longer than %d bytes "
           "(these addons put their whole configuration in the URL)\n",
           nTooLong, (int)sizeof(((ColSource *)0)->base));
  fflush(stdout);
  return added;
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
  }free(body);revisionCol++;return count;
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
