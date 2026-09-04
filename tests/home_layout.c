// No window, network or TV: validates the editorial composition and the focus
// against real catalogue data (fixtures), using the Home and catalogue
// implementations.
#include <assert.h>
#include "../src/home.c"

int main(void) {
  assert(MAX_FILTER <= FOCUS_MAX_ROWS);
  assert(profileCatalog("Oscars 2026 - Film") == ROW_COLLECTION);
  assert(profileCatalog("NETFLIX - Series") == ROW_SERVICE);
  assert(profileCatalog("For You - Film") == ROW_NORMAL);
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
  snprintf(filters[14].title, sizeof filters[14].title, "Netflix - Film");
  snprintf(filters[15].title, sizeof filters[15].title, "Oscar - Film");
  cat_set_all(itemsTeste, 48, filters, 16);
  syncRows();
  assert(nRows == 17);
  assert(focus.nRows == 17);
  assert(rows[1].kind == ROW_SOCIAL && rows[1].start == -1 && rows[1].n == 1);
  assert(rows[2].kind == ROW_HIGHLIGHT);
  for (int i = 0; i < 16; i++) {
    int found = 0;
    for (int r = 0; r < nRows; r++)
      if (!strcmp(rows[r].key, filters[i].key)) found++;
    assert(found == 1); // nenhum catálogo removido ou duplicado
  }
  focus.row = 0; focus.column = 0;
  for (int i = 0; i < 16; i++) assert(focus_mover(&focus, 0, 1));
  assert(focus.row == 16);
  assert(!focus_mover(&focus, 0, 1));
  // Mesma contagem, ordem diferente: manter chave, coluna e scroll.
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
  assert(rows[1].kind==ROW_SOCIAL);
  assert(!strcmp(rows[2].title,"Recent Release"));
  assert(!strcmp(rows[3].title,"Streaming"));
  assert(rows[3].kind==ROW_CATALOGS);
  assert(!strcmp(col_folder(rows[3].folders[0])->title,"Netflix"));
  assert(!strcmp(rows[4].title,"Trending Movies"));
  assert(!strcmp(rows[6].title,"Themes"));
  assert(!strcmp(rows[7].catId,"ai_movies_for_you"));
  assert(rows[9].kind==ROW_TOP10);
  assert(rows[10].kind==ROW_TOP10);
  assert(rows[9].stackN==3 && rows[9].n==1);
  for (int i=8; i<16; i++) {
    int found=0;
    for (int r=0; r<nRows; r++)
      if (!strcmp(rows[r].key, filters[i].key)) found=1;
    assert(found);
  }
  rows[9].n=3;rows[9].stackN=0;rows[9].seeAll=1;
  for(int i=0;i<nRows;i++)assert(strcmp(rows[i].title,"Your catalogues"));
  snprintf(filters[15].key,sizeof filters[15].key,"social_activity");
  filters[15].base[0]=filters[15].catId[0]=0;
  cat_set_all(itemsTeste,48,filters,16);syncRows();
  int social=0;
  for(int i=0;i<nRows;i++)if(rows[i].kind==ROW_SOCIAL){
    social++;assert(rows[i].start==45 && rows[i].n==3);
  }
  assert(social==1); // dados reais substituem vazio, nunca duplicam a fileira
  assert(rows[9].stackN==0 && rows[9].n==3 && rows[9].seeAll);

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
  free(itemsTeste);
  puts("home layout: PASS (fallback, imported collections, requested order, ranks, focus)");
  return 0;
}
