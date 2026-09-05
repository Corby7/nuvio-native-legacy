#include "seeall.h"
#include "badges.h"
#include "discover.h"
#include "catalog.h"
#include "gfx.h"
#include "text.h"
#include "tex_cache.h"
#include "layout.h"
#include "anim.h"
#include "settings.h"
#include "director.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

// MEDIDAS do web (catalogSeeAllScreen, .seeall-card): cartaz de 248 de largura
// e raio 12. A 1920 cabem 5 colunas com o gutter da tela dos dois lados.
#define SEEALL_COLS      (timeline ? 1 : 5)
#define SEEALL_CARD_W  248.0f
#define SEEALL_CARD_H  (timeline ? 236.0f : SEEALL_CARD_W * 1.5f)
#define SEEALL_GAP_X    16.0f                 // .seeall-grid: gap 20px 16px
#define SEEALL_GAP_Y    (20.0f + 40.0f)       // gap + a linha de titulo sob o cartaz
#define SEEALL_TOP    (collection ? 332.0f : 244.0f)

// PAINEL DE DETALHE, a direita. Medidas do web (.seeall-detail), que as ancora
// explicitamente na tela real de 1920x1080: top 170, right 104, largura 336.
//
// FIXO e nao rolando junto: o proprio CSS registra que `position: sticky` nao
// existe no Chromium 53 da TV e que sem `fixed` o painel sumia assim que o dono
// descia. Aqui nao ha fluxo nenhum — ele so nao recebe scrollY.
#define SEEALL_PAN_W   336.0f
#define SEEALL_PAN_X   (NV_SCREEN_W - 104.0f - SEEALL_PAN_W)
#define SEEALL_PAN_Y   SEEALL_TOP
#define SEEALL_PAN_ART_H 330.0f
#define SEEALL_PAN_ART_W 220.0f

static int   is_open, focus, reqOpen = -1;
static float anim, scrollY, velY;
static char  title[96];
static const ColFolder *collection;
static int source, tabFocus, tabCursor, timeline, ranked;
static float tabAnim[COL_SOURCE_MAX];
static int order[SEEALL_MAX], orderN=-1;
static char catalogId[96];
static int group(const char *name) {
  return collection && !strcmp(collection->group, name);
}
static void colorCollection(float *r,float *g,float *b) {
  col_color(collection,r,g,b);
  // Serviços preservam sua marca. Famílias editoriais sem marca própria usam
  // cor apenas em seleção/estado, mantendo as superfícies neutras.
  if (group("Genres")) {*r=.075f;*g=.34f;*b=.285f;}
  else if (group("Themes")) {*r=.34f;*g=.13f;*b=.38f;}
  else if (group("Film Collections") || group("TV Collections"))
    {*r=.12f;*g=.27f;*b=.40f;}
}
static const char *labelGroup(void) {
  if (group("Directors")) return "FILMOGRAPHY";
  if (group("Awards")) return "AWARDS AND CANON";
  if (group("Genres")) return "GENRE";
  if (group("Themes")) return "THEME";
  if (group("Streaming")) return "STREAMING";
  if (ranked) return "RANKING";
  return "COLLECTION";
}
static const char *subtitleGroup(void) {
  if (timeline) return "Filmography in chronological order";
  if (group("Awards")) return ranked ? "Ranking in its original order" : "Awards list configured by the user";
  if (group("Genres")) return "Titles of this genre in your catalogue";
  if (group("Themes")) return "Curated by theme and mood";
  if (group("Streaming")) return "Catalogue organised by service";
  return ranked ? "Original ranking order" : "A selection from your catalogue";
}

static int yearOf(const CatItem *it) {
  for(const char *s=it->meta;*s;s++)if((*s=='1'||*s=='2')&&strlen(s)>=4&&s[1]>='0'&&s[1]<='9'&&s[2]>='0'&&s[2]<='9'&&s[3]>='0'&&s[3]<='9')return atoi(s);
  return 9999;
}
static int viewItem(int i,CatItem *out) {return disc_seeall_item(timeline&&i<orderN?order[i]:i,out);}
static void openSource(void) {
  const ColSource *s=&collection->sources[source];
  snprintf(catalogId,sizeof catalogId,"%s",s->catId);
  ranked=strstr(s->catId,"top100")||strstr(s->catId,"top250")||strstr(s->catId,"top10");
  focus=0;scrollY=velY=0;orderN=-1;
  // col_source_base and not s->base: a collection from the account stores the
  // addon's ID, and the address comes from the INSTALLED addon with that id.
  { const char *base=col_source_base(s);
    if(!base||!*base) {
      // This source's addon is not installed (or its manifest was never read).
      // With no address there is nothing to fetch, and an empty grid with no
      // explanation sends you looking for a fault where there is none.
      printf("[seeall] '%s': addon '%s' has no address; not fetching\n",
             s->title[0]?s->title:s->catId, s->addonId);
      fflush(stdout);
      return;
    }
    disc_seeall_filter(base,s->type,s->catId,s->genre); }
}
void seeall_collection(const ColFolder *folder) {
  if(!folder||!folder->nSources)return;
  memset(tabAnim, 0, sizeof tabAnim);
  collection=folder;source=tabCursor=0;tabFocus=folder->nSources>1;is_open=1;reqOpen=-1;
  timeline=!strcmp(folder->group,"Directors");
  snprintf(title,sizeof title,"%s",folder->title);openSource();
}

// The parameter MUST NOT be called `title`: there is a `static char title[96]`
// at the top of this file, and the parameter shadowed it. The effect was double
// and silent —
//
//   snprintf(title, sizeof title, "%s", title ? title : "");
//
// wrote INTO the parameter, a `const char *` pointing at the caller's memory,
// with `sizeof title` being 8 (the size of a pointer) instead of 96; and the
// real buffer was never filled, so the "see all" screen showed the previous
// title.
void seeall_open(const char *base, const char *kind, const char *catId,
                   const char *heading) {
  is_open = 1; focus = 0; scrollY = 0.0f; velY = 0.0f; reqOpen = -1;
  memset(tabAnim, 0, sizeof tabAnim);
  snprintf(title, sizeof title, "%s", heading ? heading : "");
  collection=col_by_catalog(base,kind,catId);timeline=collection&&!strcmp(collection->group,"Directors");
  source=tabCursor=tabFocus=0;orderN=-1;
  if(collection) {
    for(int i=0;i<collection->nSources;i++)if(!strcmp(collection->sources[i].catId,catId)&&!strcmp(collection->sources[i].type,kind)){source=tabCursor=i;break;}
    snprintf(title,sizeof title,"%s",collection->title);
  }
  snprintf(catalogId,sizeof catalogId,"%s",catId);
  ranked=strstr(catId,"top100")||strstr(catId,"top250")||strstr(catId,"top10");
  disc_seeall_open(base, kind, catId);
}

int seeall_is_open(void) { return is_open; }
int seeall_requested_open(void) { int v = reqOpen; reqOpen = -1; return v; }

static int nItems(void) { return disc_seeall_n(); }

void seeall_event(const SDL_Event *e) {
  int n = nItems(), k;
  if (!is_open || e->type != SDL_KEYDOWN) return;
  k = e->key.keysym.sym;
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE ||
      e->key.keysym.scancode == NV_SCANCODE_BACK) { is_open = 0; return; }
  if(collection&&tabFocus) {
    if(k==SDLK_LEFT&&tabCursor>0)tabCursor--;
    if(k==SDLK_RIGHT&&tabCursor+1<collection->nSources)tabCursor++;
    if(k==SDLK_RETURN||k==SDLK_KP_ENTER){source=tabCursor;openSource();tabFocus=0;}
    if(k==SDLK_DOWN&&n>0)tabFocus=0;
    return;
  }
  if(k==SDLK_UP&&focus<SEEALL_COLS&&collection){tabFocus=1;tabCursor=source;return;}
  if((k==SDLK_RETURN||k==SDLK_KP_ENTER)&&disc_seeall_error()){disc_seeall_more();return;}
  if (n < 1) return;
  if (k == SDLK_RIGHT && focus + 1 < n) focus++;
  else if (k == SDLK_LEFT && focus > 0) focus--;
  else if (k == SDLK_DOWN) { if (focus + SEEALL_COLS < n) focus += SEEALL_COLS;
                             else focus = n - 1; }
  else if (k == SDLK_UP) { if (focus >= SEEALL_COLS) focus -= SEEALL_COLS; }
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    // O item da grade NAO esta no catalogo global — ele veio de uma pagina que
    // so esta tela leu. Entra por cat_acrescentar para que a tela de titulo
    // possa abri-lo por indice, que e como todo o app trabalha.
    CatItem it;
    if (viewItem(focus, &it)) {
      int idx = it.imdb[0] ? cat_index_by_imdb(it.imdb) : -1;
      if (idx < 0) idx = cat_append(&it);
      if (idx >= 0) { reqOpen = idx; } // conserva a lista e a posição ao voltar
    }
  }
  // Chegando perto do fim, pede a proxima pagina. Antes de o dono ver o vazio,
  // e nao quando ele ja esta olhando para ele.
  if (focus >= n - SEEALL_COLS * 2) disc_seeall_more();
}

void seeall_update(float dt, Uint32 now) {
  float target, maxY;
  int n = nItems(), lines;
  (void)now;
  anim = anim_spring(anim, is_open ? 1.0f : 0.0f, dt, NV_SPRING_SCREEN);
  if (!is_open) return;
  for (int i = 0; i < COL_SOURCE_MAX; i++) {
    float targetTab = collection && tabFocus && i == tabCursor ? 1.0f : 0.0f;
    tabAnim[i] = anim_spring(tabAnim[i], targetTab, dt,
                           targetTab > tabAnim[i] ? NV_SPRING_FOCUS : NV_SPRING_BLUR);
  }
  if(timeline&&n!=orderN) {
    int old=orderN>0&&focus<orderN?order[focus]:-1;int years[SEEALL_MAX];
    for(int i=0;i<n;i++){CatItem it;order[i]=i;years[i]=disc_seeall_item(i,&it)?yearOf(&it):9999;}
    for(int i=1;i<n;i++){int v=order[i],j=i;while(j>0&&years[order[j-1]]>years[v]){order[j]=order[j-1];j--;}order[j]=v;}
    orderN=n;if(old>=0)for(int i=0;i<n;i++)if(order[i]==old){focus=i;break;}
  }
  lines = (n + SEEALL_COLS - 1) / SEEALL_COLS;
  // Mira a linha focada a 30% da altura util, como o resto do app faz.
  target = SEEALL_TOP + (float)(focus / SEEALL_COLS) * (SEEALL_CARD_H + SEEALL_GAP_Y)
       - NV_SCREEN_H * 0.30f;
  if(tabFocus)target=0;
  maxY = SEEALL_TOP + (float)lines * (SEEALL_CARD_H + SEEALL_GAP_Y) - NV_SCREEN_H + 120.0f;
  if (maxY < 0.0f) maxY = 0.0f;
  if (target < 0.0f) target = 0.0f;
  if (target > maxY) target = maxY;
  scrollY = anim_spring2(&velY, scrollY, target, dt, NV_SPRING2_SCROLL);
}

// PAINEL DA DIREITA: o que a grade sozinha nao diz — sinopse, generos, nota.
//
// Sem ele o dono ve 50 cartazes e nenhuma informacao; era o que faltava para a
// tela deixar de ser so uma parede de imagens.
static void panel(float a) {
  CatItem it;
  float y = SEEALL_PAN_Y;
  if (!viewItem(focus, &it)) return;

  // O contexto lateral usa o cartaz, nunca repete a cena da timeline.
  // Mantém a geometria 2:3 mesmo sem arte para não deslocar os metadados.
  { GfxRect r = { SEEALL_PAN_X, y, SEEALL_PAN_ART_W, SEEALL_PAN_ART_H };
    const char *art = it.poster[0] ? it.poster : it.backdrop;
    float radius = 12.0f / SEEALL_PAN_ART_W;
    GLuint t = art[0] ? tex_get_width(art, SEEALL_PAN_ART_W) : 0;
    gfx_color(r, radius, 1, 1, 1, 0.05f * a);
    if (t) {
      gfx_tex_aspect_current = tex_aspect(art);
      gfx_rect(r, t, GFX_CARD, 0, 0, 0, radius, 0, 0, 0, a);
      gfx_tex_aspect_current = 0.0f;
    } else {
      TxtLine missing = txt_line_trim(TXT_HERO_META, "No poster", 185, 191, 204, 255, r.w-24);
      txt_draw_alpha(missing, r.x+(r.w-missing.w)*.5f, r.y+(r.h-missing.h)*.5f, a);
    } }
  y += SEEALL_PAN_ART_H + 18.0f;
  float badgeW=badges_draw(badges_provider(it.providerName),SEEALL_PAN_X,y,SEEALL_PAN_W,28,a);
  if(badgeW>0)y+=40;

  // LOGO no lugar do titulo quando existe (max 264x82 no web); o nome escrito
  // com a fonte da interface so quando nao ha logo.
  { GLuint tl = it.logo[0] ? tex_get_width(it.logo, 264.0f) : 0;
    float ap = it.logo[0] ? tex_aspect(it.logo) : 0.0f;
    if (tl && ap > 0.0f) {
      float wL = 264.0f, hL = wL / ap;
      if (hL > 82.0f) { hL = 82.0f; wL = hL * ap; }
      { GfxRect rl = { SEEALL_PAN_X, y, wL, hL };
        GfxMode m = tex_brand_dark(it.logo) ? GFX_BRAND : GFX_TEXT;
        gfx_tex_aspect_current = 0.0f;
        gfx_rect(rl, tl, m, 0, 0, 0, 0.0f, 1, 1, 1, a); }
      y += hL + 4.0f;
    } else {
      TxtLine t = txt_line_trim(TXT_HEADLINE, it.title, 245, 245, 245, 255,
                                   SEEALL_PAN_W);
      txt_draw_alpha(t, SEEALL_PAN_X, y, a);
      y += t.h + 6.0f;
    } }

  if (it.genre[0]) {
    TxtLine t = txt_line_trim(TXT_CAPTION, it.genre, 220, 231, 244, 255,
                                 SEEALL_PAN_W);
    txt_draw_alpha(t, SEEALL_PAN_X, y, a * 0.72f);
    y += t.h + 6.0f;
  }
  // Pastilha da nota, no amarelo do IMDb que o web usa (245,197,24).
  if (it.score > 0) {
    char n[16];
    snprintf(n, sizeof n, "%.1f", it.score / 10.0f);
    { TxtLine t = txt_line(TXT_CAPTION, n, 23, 19, 10, 255);
      GfxRect r = { SEEALL_PAN_X, y + 8.0f, t.w + 26.0f, t.h + 8.0f };
      gfx_color(r, 8.0f / (t.h + 8.0f), 0.961f, 0.773f, 0.094f, 0.92f * a);
      txt_draw_alpha(t, SEEALL_PAN_X + 13.0f, y + 12.0f, a);
      y += r.h + 14.0f; }
  }
  if (it.meta[0]) {
    TxtLine t = txt_line_trim(TXT_DET_META2, it.meta, 240, 240, 240, 255,
                                 SEEALL_PAN_W);
    txt_draw_alpha(t, SEEALL_PAN_X, y, a * 0.92f);
    y += t.h + 12.0f;
  }
  if (it.synopsis[0]) {
    // Ate onde couber sem passar da base util (o web corta em
    // max-height: 100% - 210).
    int lines = (int)((NV_SCREEN_H - 48.0f - y) / 34.0f);
    if (lines > 8) lines = 8;
    if (lines > 0)
      txt_block(TXT_DET_META2, it.synopsis, 236, 236, 236,
                SEEALL_PAN_X, y, SEEALL_PAN_W, 34.0f, a * 0.86f, lines);
  }
}

static const char *portraitLocal(const ColFolder *folder) {
  static char path[700];
  if (!folder || !folder->frameDir[0]) return "";
  snprintf(path, sizeof path, "%s/portrait.png", folder->frameDir);
  if (access(path, R_OK) == 0) return path;
  snprintf(path, sizeof path, "%s/portrait.jpg", folder->frameDir);
  return access(path, R_OK) == 0 ? path : "";
}

// Uma única arte full-width, dissolvendo na mesma cor do corpo. Directors usa
// o retrato vertical local quando o pacote ja o tem; o hero horizontal dessa
// colecao e um placeholder neutro e so acrescenta uma camada sem informacao.
// Usa os shaders e o cache existentes, sem blur ou novas texturas por frame.
static void themeBackground(float a) {
  if(collection) {
    if(collection->editorial) {
      GLuint art=tex_get_hero(collection->detailHero);
      /* The content starts at 332; the separate detail illustration ends at 320. */
      if(art)gfx_rect((GfxRect){0,0,1920,320},art,GFX_TEXT,0,0,0,0,1,1,1,a);
      return;
    }
    if(group("Directors")) {
      const char *photo=portraitLocal(collection);
      if(!photo[0]) {
        director_request(collection->title);
        photo=director_photo(collection->title);
      }
      GLuint tp=photo[0]?tex_get_width(photo,260.0f):0;
      if(tp) {
        // O painel do item selecionado começa em VT_PAN_Y. O retrato ocupa
        // apenas o cabeçalho e termina antes dele, sem atravessar pôster ou
        // sinopse como uma segunda camada.
        GfxRect rp={1660,18,260,300};
        gfx_tex_aspect_current=tex_aspect(photo);
        gfx_rect(rp,tp,GFX_PORTRAIT,
                 0,0,0,0,0,0,0,a*.88f);
        gfx_tex_aspect_current=0;
      }
    } else {
      const char *art=collection->hero[0]?collection->hero:collection->cover;
      GLuint tex=art[0]?tex_get_hero(art):0;
      if(tex) {
        gfx_tex_aspect_current=tex_aspect(art);
        gfx_rect((GfxRect){0,0,NV_SCREEN_W,620},tex,GFX_HERO_FULL,
                 0,0,0,0,0,0,0,a*.38f);
        gfx_tex_aspect_current=0;
      }
    }
  }
}

static void themeHeader(float a,float x0) {
  float r,g,b;colorCollection(&r,&g,&b);
  TxtLine eyebrow=txt_line(TXT_HERO_META,labelGroup(),197,202,211,255);
  txt_draw_alpha(eyebrow,x0,40,a);
  int isDirector=collection&&!strcasecmp(collection->group,"Directors");
  // O wordmark de uma coleção de diretores pode conter cabeça ou lettering
  // composto. No cabeçalho da filmografia, o nome textual e o retrato limpo
  // deixam a identidade legível sem duplicar a mesma informação visual.
  GLuint logo=!isDirector&&collection&&!collection->editorial&&collection->logo[0]
             ?tex_get_width(collection->logo,560):0;
  float aspect=logo?tex_aspect(collection->logo):0;
  if(logo&&aspect>0) {
    // Wordmark oficial, grande o bastante para leitura a distancia. O PNG
    // transparente e importado em ate 800px, portanto 560px nao interpola para
    // cima nem perde a silhueta original da marca.
    float w=560.0f,h=w/aspect;if(h>108){h=108;w=h*aspect;}
    gfx_rect((GfxRect){x0,83,w,h},logo,tex_brand_dark(collection->logo)?GFX_BRAND:GFX_TEXT,0,0,0,0,.96f,.97f,.98f,a);
  } else {TxtLine line=txt_line_trim(TXT_TITLE1,title,242,243,247,255,940);txt_draw_alpha(line,x0,80,a);}
  char caption[180];int n=nItems();
  if(disc_seeall_error())snprintf(caption,sizeof caption,"Could not load. OK to try again.");
  else if(!n)snprintf(caption,sizeof caption,"%s",disc_seeall_loading()?"Loading titles…":"No titles in this list.");
  else snprintf(caption,sizeof caption,"%d titles%s  ·  %s",n,disc_seeall_end()?"":" loaded",subtitleGroup());
  TxtLine sub=txt_line_trim(TXT_DET_META2,caption,196,202,213,255,960);txt_draw_alpha(sub,x0,192,a);
  if(collection&&collection->nSources>1) {
    int first=tabCursor>3?tabCursor-3:0;
    gfx_crop(x0-6,244,NV_SCREEN_W-x0-90,72);
    for(int i=first;i<collection->nSources&&i<first+6;i++) {
      float x=x0+(i-first)*322.0f;const ColSource *s=&collection->sources[i];
      int f=tabFocus&&tabCursor==i, selected=source==i;
      float fa=tabAnim[i], scale=1.0f+.025f*fa;
      GfxRect pill={x-(304*scale-304)*.5f,250-(58*scale-58)*.5f,
                    304*scale,58*scale};
      gfx_color(pill,.28f,f?.94f:selected?r*.82f:.09f,
              f?.95f:selected?g*.82f:.10f,
              f?.97f:selected?b*.82f:.12f,a);
      if(!f) gfx_rect(pill,0,GFX_RING,0,1.5f/pill.h,0,.28f,
                      selected?.86f:.36f,selected?.88f:.38f,
                      selected?.92f:.43f,a*(selected?.72f:.35f));
      if(selected&&!f)
        gfx_color((GfxRect){pill.x+18,pill.y+pill.h-4,pill.w-36,3},.5f,
                .84f+r*.16f,.84f+g*.16f,.84f+b*.16f,a);
      char label[180];snprintf(label,sizeof label,"%s · %s",s->title,!strcmp(s->type,"series")?"Series":"Films");
      TxtLine t=txt_line_trim(TXT_HERO_META,label,f?22:238,f?24:240,f?28:245,255,276);
      txt_draw_alpha(t,pill.x+(pill.w-t.w)*.5f,pill.y+(pill.h-t.h)*.5f,a);
    }gfx_no_crop();
  }
}

static void timelineCard(int i,float cy,float a,float x0) {
  CatItem it;if(!viewItem(i,&it))return;
  int sel=i==focus&&!tabFocus;float r,g,b;colorCollection(&r,&g,&b);
  // A linha organiza a cronologia; não é uma borda decorativa de card.
  gfx_color((GfxRect){x0+109,cy-30,2,SEEALL_CARD_H+SEEALL_GAP_Y},0,.48f,.47f,.46f,a*.6f);
  gfx_color((GfxRect){x0+102,cy+24,16,16},.5f,sel?.95f:r,sel?.95f:g,sel?.97f:b,a);
  char year[16];int y=yearOf(&it);if(y==9999)snprintf(year,sizeof year,"—");else snprintf(year,sizeof year,"%d",y);
  TxtLine yr=txt_line(TXT_CW_TITLE,year,219,210,195,255);txt_draw_alpha(yr,x0,cy+14,a);
  float x=x0+158;GfxRect card={x,cy,1000,SEEALL_CARD_H};
  if(sel)gfx_color((GfxRect){x-4,cy-4,1008,SEEALL_CARD_H+8},.045f,.94f,.95f,.97f,a);
  gfx_color(card,.04f,.09f,.095f,.105f,a);
  const char *art=it.backdrop[0]?it.backdrop:it.poster;GLuint tex=art[0]?tex_get_width(art,390):0;
  if(tex){gfx_tex_aspect_current=tex_aspect(art);gfx_rect((GfxRect){x+12,cy+12,376,212},tex,GFX_CARD,0,0,0,.04f,0,0,0,a);gfx_tex_aspect_current=0;}
  else { gfx_color((GfxRect){x+12,cy+12,376,212},.04f,.16f,.16f,.18f,a);
         txt_draw_alpha(txt_line(TXT_MINI,"No art",184,188,198,255),
                            x+158,y+104,a*.9f); }
  TxtLine name=txt_line_trim(TXT_CW_TITLE,it.title,242,243,247,255,550);txt_draw_alpha(name,x+418,cy+22,a);
  TxtLine genre=txt_line_trim(TXT_HERO_META,it.genre,187,194,207,255,550);txt_draw_alpha(genre,x+418,cy+64,a);
  txt_block(TXT_DET_META2,it.synopsis,209,214,225,x+418,cy+106,545,30,a,3);
}

void seeall_draw(Uint32 now) {
  float a = anim, x0 = settings_content_x();
  int n = nItems(), i;
  (void)now;
  if (a < 0.01f) return;
  { GfxRect screen = { 0, 0, NV_SCREEN_W, NV_SCREEN_H };
    gfx_color(screen, 0.0f, NV_COLOR_BACKGROUND_R, NV_COLOR_BACKGROUND_G, NV_COLOR_BACKGROUND_B, a); }
  themeBackground(a);

  // A GRADE E RECORTADA ABAIXO DO CABECALHO.
  //
  // O cabecalho ja era desenhado em posicao fixa, mas os cartazes passavam POR
  // TRAS dele ao rolar — o titulo ficava sobre imagem em movimento e virava
  // "fundo". O recorte resolve sem precisar de faixa opaca: o que sobe alem do
  // topo simplesmente nao e desenhado.
  gfx_crop(0.0f, SEEALL_TOP - 12.0f, NV_SCREEN_W, NV_SCREEN_H - SEEALL_TOP + 12.0f);
  for (i = 0; i < n; i++) {
    float cx = x0 + (float)(i % SEEALL_COLS) * (SEEALL_CARD_W + SEEALL_GAP_X);
    float cy = SEEALL_TOP + (float)(i / SEEALL_COLS) * (SEEALL_CARD_H + SEEALL_GAP_Y) - scrollY;
    CatItem it;
    GLuint t;
    // MESMO raio dos cartazes da home: `posterCardCornerRadiusDp` (12dp x 2 =
    // 24px), fracao do MENOR lado porque o SDF do shader e normalizado. O
    // NV_RAIO_CARD fixo que estava aqui dava um canto diferente do resto do
    // app, e a grade lia como outra tela.
    float radius = settings_radius_poster_px() / SEEALL_CARD_W;
    int sel = (i == focus);
    if (cy > NV_SCREEN_H || cy + SEEALL_CARD_H + 40.0f < SEEALL_TOP - 12.0f) continue;
    if(timeline){timelineCard(i,cy,a,x0);continue;}
    if (!viewItem(i, &it)) continue;
    { GfxRect r = { cx, cy, SEEALL_CARD_W, SEEALL_CARD_H };
      if (sel && !tabFocus) {
        GfxRect ring = { cx - 4, cy - 4, SEEALL_CARD_W + 8, SEEALL_CARD_H + 8 };
        gfx_color(ring, settings_radius_poster_px() / (SEEALL_CARD_W + 8.0f), 1, 1, 1, a);
      }
      t = it.poster[0] ? tex_get_width(it.poster, SEEALL_CARD_W)
        : (it.backdrop[0] ? tex_get_width(it.backdrop, SEEALL_CARD_W) : 0);
      if (t) {
        gfx_tex_aspect_current = tex_aspect(it.poster[0] ? it.poster : it.backdrop);
        gfx_rect(r, t, GFX_CARD, sel ? 1.0f : 0.0f, 0, 0, radius, 0, 0, 0, a);
        gfx_tex_aspect_current = 0.0f;
      } else {
        // Esqueleto enquanto a arte nao chega — a mesma cor do resto do app.
        gfx_color(r, radius, NV_COLOR_SKELETON_R, NV_COLOR_SKELETON_G,
                NV_COLOR_SKELETON_B, a);
      } }
    { int c = sel ? 255 : 214;
      TxtLine l = txt_line_trim(TXT_DET_META2, it.title, c, c, c, 255,
                                   SEEALL_CARD_W);
      txt_draw_alpha(l, cx, cy + SEEALL_CARD_H + 10.0f, a * (sel ? 1.0f : 0.86f)); }
    if(ranked) {
      char rank[8];snprintf(rank,sizeof rank,"%d",i+1);
      TxtLine edge=txt_line(TXT_RANK,rank,234,236,241,255),ink=txt_line(TXT_RANK,rank,17,18,22,255);
      float x=cx-10,y=cy+SEEALL_CARD_H-edge.h;
      for(int dx=-2;dx<=2;dx+=2)for(int dy=-2;dy<=2;dy+=2)txt_draw_alpha(edge,x+dx,y+dy,a);
      txt_draw_alpha(ink,x,y,a);
    }
  }
  if(!n&&disc_seeall_loading())for(int i=0;i<5;i++)
    gfx_color((GfxRect){x0+i*264,SEEALL_TOP,248,372},.06f,.12f,.13f,.15f,a);
  gfx_no_crop();

  // CABECALHO por cima do recorte, entao ele nunca compete com a arte.
  themeHeader(a,x0);

  if (n > 0) panel(a);
}
