#include "collections.h"
#include "js.h"
#include "addons.h"
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
// The collections' revision. The home decides whether to rebuild its rows from a
// hash of the CATALOGUE rows; without this number in that sum, a collection that
// arrives from the account sits in memory and never reaches the screen.
static unsigned revisionCol = 1;
unsigned col_revision(void) { return revisionCol; }

// Groups the owner pinned to the top (`pinToTop`).
#define COL_PINNED_MAX 24
static char pinned[COL_PINNED_MAX][64];
static int nPinned;

int col_group_pinned(const char *group) {
  int i;
  if (!group || !group[0]) return 0;
  for (i = 0; i < nPinned; i++) if (!strcasecmp(pinned[i], group)) return 1;
  return 0;
}

const char *col_source_base(const ColSource *s) {
  const char *b;
  if (!s) return "";
  b = addons_base_for_id(s->addonId);
  if (b && *b) return b;
  return s->base;
}

const char *col_group_by_id(const char *id) {
  int i;
  if (!id || !*id) return NULL;
  for (i = 0; i < count; i++)
    if (folders[i].groupId[0] && !strcmp(folders[i].groupId, id))
      return folders[i].group;
  return NULL;
}

int col_load_account(const char *body) {
  const char *root, *end, *c;
  const char *list = NULL;
  int added = 0, skippedNoSource = 0, nTmdb = 0, nTrakt = 0;
  int packaged = count;
  if (!body || !*body) return 0;
  end = body + strlen(body);
  // The body is [{"collections_json":[...]}]. If the server returns the list
  // directly, or under another name, try both before giving up — and say which.
  root = js_root_array(body);
  if (root) list = js_array(root, js_end(root), "collections_json");
  if (!list) list = js_array(body, end, "collections");
  if (!list) {
    printf("[col] account collections: no list found in the response\n");
    fflush(stdout);
    return 0;
  }
  // THE ACCOUNT REPLACES THE PACKAGE. The collections shipped in the .ipk are
  // whoever built it curating for themselves; when the owner has their own, those
  // are the ones that count.
  //
  // This is also where the accumulation bug is fixed: this function runs once per
  // sync cycle and did NOT reset `count`, so the same folders were appended again
  // on every pass until COL_MAX overflowed.
  //
  // The swap only happens if something was really read (see the commit below): an
  // empty or broken response must not wipe what is already on screen.
  count = 0;
  nPinned = 0;

  for (; list; list = js_next(js_end(list))) {
    const char *ce = js_end(list);
    char group[64] = "", groupId[96] = "";
    int pin;
    js_text(list, ce, "title", group, sizeof group);
    js_text(list, ce, "id",    groupId, sizeof groupId);
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
      snprintf(v->group,   sizeof v->group,   "%s", group);
      snprintf(v->groupId, sizeof v->groupId, "%s", groupId);
      js_text(c, fe, "id",    v->id,    sizeof v->id);
      js_text(c, fe, "title", v->title, sizeof v->title);
      // Remote art: CDN URLs. `locates` lets anything containing "://" through,
      // so no local path is invented on top of them, and tex_get knows how to
      // fetch http(s).
      js_text(c, fe, "coverImageUrl",   v->cover, sizeof v->cover);
      js_text(c, fe, "heroBackdropUrl", v->hero,  sizeof v->hero);
      js_text(c, fe, "titleLogoUrl",    v->logo,  sizeof v->logo);
      v->hideTitle = js_flag(c, fe, "hideTitle", 0);
      v->frames = 0;          // no frame folder for an account collection
      v->frameDir[0] = 0;
      // `catalogSources` is the list already filtered down to ADDON sources, the
      // only ones this app can fetch. `sources` covers the older shape.
      s = js_array(c, fe, "catalogSources");
      if (!s) s = js_array(c, fe, "sources");
      for (; s && v->nSources < COL_SOURCE_MAX; s = js_next(js_end(s))) {
        const char *se = js_end(s);
        ColSource *a = &v->sources[v->nSources];
        char provider[24] = "";
        // A collection source can be one of three kinds, and only one of them is
        // an addon catalogue. Counting them by kind turns "29 skipped" into a
        // sentence that says what would have to be built for each.
        js_text(s, se, "provider", provider, sizeof provider);
        if (!strcasecmp(provider, "tmdb"))  { nTmdb++;  continue; }
        if (!strcasecmp(provider, "trakt")) { nTrakt++; continue; }
        memset(a, 0, sizeof *a);
        // THE ID, NOT THE URL. This source's `addonBaseUrl` runs past 4 KB —
        // these addons carry their whole configuration inside the address — and it
        // fits in no field here without costing tens of megabytes of static data.
        //
        // The real address is the INSTALLED addon with this id, resolved in
        // col_source_base at fetch time. That is the same rule the web app uses:
        // match by id before looking at the URL stored in the collection.
        js_text(s, se, "addonId", a->addonId, sizeof a->addonId);
        js_text(s, se, "type",      a->type,  sizeof a->type);
        js_text(s, se, "catalogId", a->catId, sizeof a->catId);
        if (!a->catId[0]) js_text(s, se, "catId", a->catId, sizeof a->catId);
        js_text(s, se, "title", a->title, sizeof a->title);
        if (!a->title[0]) js_text(s, se, "catalogName", a->title, sizeof a->title);
        js_text(s, se, "genre", a->genre, sizeof a->genre);
        if (!a->addonId[0] || !a->type[0] || !a->catId[0]) continue;
        // DO NOT CHECK HERE WHETHER THE ADDON IS INSTALLED. Addon ids are learned
        // while reading the manifests, during discovery, and collections arrive
        // with the sync BEFORE that — checking now would reject every source of an
        // addon that is perfectly well installed, only because its manifest had
        // not been read yet this session. That is exactly what happened: 129
        // sources refused with "addon 'aio-metadata' is not installed", two lines
        // after "'AIOMetadata' is id 'aio-metadata'".
        //
        // col_source_base resolves it at fetch time, when the ids exist. See the
        // warning in seeall.c for the case where they never do.
        v->nSources++;
      }
      // A folder with no addon source has nowhere to get a single title from —
      // the case of a folder built on TMDB filters, which this app does not fetch.
      // Counting them and saying how many is better than an empty row.
      if (v->nSources && v->title[0]) { count++; added++; }
      else if (v->title[0]) skippedNoSource++;
    }
  }
  // COMMIT. If nothing was read, the swap is undone: `count` goes back to what
  // the package had and the home keeps the collections it was already showing.
  // Blanking the screen over an empty response would trade one fault for a worse
  // one.
  if (!added) {
    count = packaged;
    printf("[col] account collections: nothing usable; keeping the %d packaged\n",
           packaged);
  } else {
    revisionCol++;
    printf("[col] account collections: %d folder(s) added, %d skipped "
           "(replacing %d packaged)\n", added, skippedNoSource, packaged);
  }
  // WHAT WOULD BE MISSING for the skipped ones. These are whole fetch paths this
  // app does not have: TMDB here only serves cast and art, not filtered discovery,
  // and Trakt lists are fetched by id, which does not exist either.
  if (nTmdb || nTrakt)
    printf("[col] sources this app cannot fetch: %d TMDB, %d Trakt\n",
           nTmdb, nTrakt);
  fflush(stdout);
  return added;
}

int col_n(void) { return count; }
const ColFolder *col_folder(int i) { return i>=0&&i<count?&folders[i]:NULL; }
int col_group(const char *name,int *indices,int max) {
  int n=0;for(int i=0;i<count&&n<max;i++) if(!strcasecmp(name,folders[i].group)) indices[n++]=i;return n;
}
// Compares the RESOLVED address: a source from the account stores an addonId and
// not a URL, so comparing the raw field would never match the base the catalogue
// carries.
const ColFolder *col_by_catalog(const char *base,const char *type,const char *id) {
  for(int i=0;i<count;i++) for(int s=0;s<folders[i].nSources;s++) {
    const ColSource *v=&folders[i].sources[s];
    const char *b=col_source_base(v);
    if(b&&*b&&!strcmp(b,base)&&!strcmp(v->type,type)&&!strcmp(v->catId,id)) return &folders[i];
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
