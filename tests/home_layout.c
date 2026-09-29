// No window, network or TV: validates the editorial composition and the focus
// against real catalogue data (fixtures), using the Home and catalogue
// implementations.
#include <assert.h>
#include "../src/home.c"

// THE ACCOUNT'S ROW PREFERENCES, STUBBED. They live in discover.c, which drags in
// the network and SDL; this test links the logic under test and nothing else (see
// tests/home.sh). An empty preference list is the state syncRows already has to
// handle — it is what a first run looks like — so the composition it produces here
// is the packaged order, which is what the assertions below are written against.
int         disc_prefs_n(void) { return 0; }
const char *disc_prefs_key(int i) { (void)i; return ""; }
int         disc_prefs_hidden(const char *key) { (void)key; return 0; }
const char *disc_prefs_title(const char *key) { (void)key; return NULL; }
// The opt-in Trakt rows (homerows.c) are off, as on a first run.
int         homerows_trakt_watchlist(void) { return 0; }
int         homerows_trakt_recs(void) { return 0; }
// The row styles' file (data.c) is one string in memory, for one profile.
static char styleFile[4096];
int   profiles_active(void) { return 1; }
char *data_read(const char *name) {
  (void)name;
  return styleFile[0] ? strdup(styleFile) : NULL;
}
int   data_write(const char *name, const char *content) {
  (void)name; snprintf(styleFile, sizeof styleFile, "%s", content); return 1;
}

int main(void) {
  assert(MAX_FILTER <= FOCUS_MAX_ROWS);
  assert(profileCatalog("Oscars 2026 - Movie") == ROW_COLLECTION);
  assert(profileCatalog("NETFLIX - Series") == ROW_SERVICE);
  assert(profileCatalog("For You - Movie") == ROW_NORMAL);
  assert(widthOf(ROW_HIGHLIGHT) > widthOf(ROW_COLLECTION));
  assert(widthOf(ROW_COLLECTION) > widthOf(ROW_SERVICE));
  assert(!hasLabel(ROW_HIGHLIGHT));
  assert(!hasLabel(ROW_CATALOGS));

  CatItem *itemsTeste = calloc(48, sizeof *itemsTeste);
  CatRow filters[16] = {0};
  assert(itemsTeste);
  for (int i = 0; i < 16; i++) {
    snprintf(filters[i].key, sizeof filters[i].key, "catalog_%d", i);
    snprintf(filters[i].title, sizeof filters[i].title, "List %d", i);
    snprintf(filters[i].base, sizeof filters[i].base, "https://example.invalid/addon");
    snprintf(filters[i].kind, sizeof filters[i].kind, "movie");
    snprintf(filters[i].catId, sizeof filters[i].catId, "id%d", i);
    filters[i].start = i*3; filters[i].n = 3;
  }
  snprintf(filters[0].key, sizeof filters[0].key, "continue_watching");
  filters[0].base[0] = filters[0].catId[0] = 0;
  snprintf(filters[14].title, sizeof filters[14].title, "Netflix - Movie");
  snprintf(filters[15].title, sizeof filters[15].title, "Oscar - Movie");
  cat_set_all(itemsTeste, 48, filters, 16);
  syncRows();
  assert(nRows == 16);
  assert(focus.nRows == 16);
  assert(rows[1].kind == ROW_HIGHLIGHT);
  // THE OWNER'S ROW STYLE beats the guess, survives a reload and goes back to it
  // on "Automatic". Continue watching cannot take one.
  { char k1[192], kN[192];
    int netflix = -1;
    for (int r = 0; r < nRows; r++)
      if (!strcmp(rows[r].key, filters[14].key)) netflix = r;
    assert(netflix >= 0 && rows[netflix].kind == ROW_SERVICE);
    snprintf(k1, sizeof k1, "%s", rows[1].key);
    snprintf(kN, sizeof kN, "%s", rows[netflix].key);
    styleSet(k1, STYLE_POSTER);
    styleSet(kN, STYLE_LARGE);
    syncRows();
    assert(rows[1].kind == ROW_NORMAL);
    assert(rows[netflix].kind == ROW_HIGHLIGHT);
    styledProfile = -1; syncRows();        // read back from the file
    assert(nStyled == 2 && rows[1].kind == ROW_NORMAL);
    styleSet(k1, STYLE_AUTO);
    styleSet(kN, STYLE_AUTO);
    syncRows();
    assert(rows[1].kind == ROW_HIGHLIGHT && rows[netflix].kind == ROW_SERVICE);
    assert(!styleable(rows[0].kind, rows[0].key)); }
  for (int i = 0; i < 16; i++) {
    int found = 0;
    for (int r = 0; r < nRows; r++)
      if (!strcmp(rows[r].key, filters[i].key)) found++;
    assert(found == 1); // no catalogue removed or duplicated
  }
  focus.row = 0; focus.column = 0;
  for (int i = 0; i < 15; i++) assert(focus_move(&focus, 0, 1));
  assert(focus.row == 15);
  assert(!focus_move(&focus, 0, 1));
  // Same count, different order: keep key, column and scroll.
  focus.row = 5; focus.column = 2; scrollX[5] = 123;
  char key[192]; snprintf(key, sizeof key, "%s", rows[5].key);
  CatRow swap = filters[4]; filters[4] = filters[8]; filters[8] = swap;
  cat_set_all(itemsTeste, 48, filters, 16);
  syncRows();
  assert(!strcmp(rows[focus.row].key, key));
  assert(focus.column == 2);
  assert(scrollX[focus.row] == 123);
  assert(col_load("tests/fixtures/collections") == 2);
  assert(col_folder(0)->nSources==2);
  assert(col_folder(0)->frames==0);
  assert(col_folder(-1)==NULL);
  const char *ids[]={"", "now_playing_movies","trending_movies","trending_series",
    "ai_movies_for_you","ai_series_for_you","snoak_top100_movies","snoak_top100_series"};
  for(int i=1;i<8;i++)snprintf(filters[i].catId,sizeof filters[i].catId,"%s",ids[i]);
  cat_set_all(itemsTeste,48,filters,16);filtersApplied=-1;syncRows();
  // The curation orders the known shortcuts, but does not erase new rows
  // declared by the addon. The fixture has eight keys outside the editorial
  // table.
  assert(nRows>=11);
  assert(!strcmp(rows[1].title,"Recent Release"));
  assert(!strcmp(rows[2].title,"Streaming"));
  assert(rows[2].kind==ROW_CATALOGS);
  assert(!strcmp(col_folder(rows[2].folders[0])->title,"Netflix"));
  assert(!strcmp(rows[3].title,"Trending Movies"));
  assert(!strcmp(rows[5].title,"Themes"));
  assert(!strcmp(rows[6].catId,"ai_movies_for_you"));
  assert(rows[8].kind==ROW_TOP10);
  assert(rows[9].kind==ROW_TOP10);
  assert(rows[8].stackN==3 && rows[8].n==1);
  for (int i=8; i<16; i++) {
    int found=0;
    for (int r=0; r<nRows; r++)
      if (!strcmp(rows[r].key, filters[i].key)) found=1;
    assert(found);
  }
  rows[8].n=3;rows[8].stackN=0;rows[8].stackOpen=1;
  for(int i=0;i<nRows;i++)assert(strcmp(rows[i].title,"Your catalogues"));
  snprintf(filters[15].key,sizeof filters[15].key,"social_activity");
  filters[15].base[0]=filters[15].catId[0]=0;
  cat_set_all(itemsTeste,48,filters,16);syncRows();
  // The friends' feed is gone: a cached catalogue that still carries its row
  // loses it, and nothing takes its place.
  for(int i=0;i<nRows;i++)assert(strcmp(rows[i].key,"social_activity"));
  assert(rows[8].stackN==0 && rows[8].n==3 && rows[8].stackOpen);

  // Another title's art is never a silent fallback, even when the index is
  // beyond the local library. With no catalogue, the local arrays stay available
  // only at the same position.
  snprintf(itemsTeste[0].poster,sizeof itemsTeste[0].poster,"own-poster.jpg");
  snprintf(itemsTeste[0].backdrop,sizeof itemsTeste[0].backdrop,"own-backdrop.jpg");
  snprintf(itemsTeste[1].poster,sizeof itemsTeste[1].poster,"other-poster.jpg");
  nBd=2; nPst=1;
  snprintf(bd[0],sizeof bd[0],"fallback-0.jpg");
  snprintf(bd[1],sizeof bd[1],"fallback-1.jpg");
  snprintf(pst[0],sizeof pst[0],"fallback-poster-0.jpg");
  cat_set_all(itemsTeste,48,NULL,0);
  assert(!strcmp(art_by_identity(0,0),"own-poster.jpg"));
  assert(!strcmp(art_by_identity(0,1),"own-backdrop.jpg"));
  assert(!strcmp(art_by_identity(1,0),"other-poster.jpg"));
  assert(art_by_identity(999,0)==NULL);

  // The second-order integration retargets without overshoot, and reduced
  // motion can jump to the destination leaving no residual velocity.
  { float x=0, v=0;
    for (int i=0; i<60; i++) {
      x=anim_spring2(&v,x,1.0f,0.016f,NV_SPRING2_SCROLL);
      assert(x>=0.0f && x<=1.0f);
    }
    x=anim_spring2(&v,x,0.0f,0.016f,NV_SPRING2_SCROLL);
    assert(x>=0.0f && x<=1.0f);
    x=anim_spring2_reduced(&v,x,0.35f,0.016f,NV_SPRING2_SCROLL,1);
    assert(x==0.35f && v==0.0f);
  }
  assert(NV_HERO_FADE_MS>=180.0f && NV_HERO_FADE_MS<=250.0f);

  // A ROW GROWS IN PLACE: the page lands at the end of its own window, the rows
  // after it follow their items, and the home picks the longer row up. This is
  // what replaced the "See all" card, so the row has to be able to pass the twelve
  // it was born with.
  { CatRow two[2] = {0};
    CatItem page[3] = {0};
    int before;
    for (int i = 0; i < 2; i++) {
      snprintf(two[i].key,  sizeof two[i].key,  "grow_%d", i);
      snprintf(two[i].title, sizeof two[i].title, "Grow %d", i);
      snprintf(two[i].kind,  sizeof two[i].kind,  "movie");
      snprintf(two[i].base,  sizeof two[i].base,  "https://example.invalid/addon");
      snprintf(two[i].catId, sizeof two[i].catId, "grow%d", i);
      two[i].start = i * 12; two[i].n = 12;
    }
    for (int i = 0; i < 24; i++)
      snprintf(itemsTeste[i].imdb, sizeof itemsTeste[i].imdb, "tt%d", i);
    for (int i = 0; i < 3; i++)
      snprintf(page[i].imdb, sizeof page[i].imdb, "ttnew%d", i);
    cat_set_all(itemsTeste, 24, two, 2);
    before = cat_n();
    assert(cat_row(0)->n == 12 && cat_row(1)->start == 12);
    assert(cat_row_grow(0, page, 3) == 3);
    assert(cat_n() == before + 3);
    assert(cat_row(0)->n == 15);
    // The second row moved with its items, and they are still its items.
    assert(cat_row(1)->start == 15 && cat_row(1)->n == 12);
    assert(!strcmp(cat_item(15)->imdb, "tt12"));
    assert(!strcmp(cat_item(12)->imdb, "ttnew0"));
    filtersApplied = -1; syncRows();
    { int found = -1;
      for (int r = 0; r < nRows; r++) if (!strcmp(rows[r].key, "grow_0")) found = r;
      assert(found >= 0);
      assert(rows[found].n == 15);            // past the twelve it was built with
      assert(focus.nColumns[found] == 15); }  // and no "See all" column after it
  }
  free(itemsTeste);
  puts("home layout: PASS (fallback, imported collections, requested order, ranks, focus)");
  return 0;
}
