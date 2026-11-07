// "Continue watching" cards keep their episode when progress lands on them.
//
// The defect this pins: the sync replays the account's progress rows every five
// minutes through cat_save_progress_at, with the index of the FIRST item of the
// work — the Continue watching card. A next-up card "tt…:29:4" took the finished
// S29E3 line, came out as S29 E3 at 100% with no episode name, and stayed so.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/catalog.h"

static void card(CatItem *c, const char *imdb, const char *kind, int s, int e) {
  memset(c, 0, sizeof *c);
  snprintf(c->imdb, sizeof c->imdb, "%s", imdb);
  snprintf(c->kind, sizeof c->kind, "%s", kind);
  c->season = s; c->episode = e;
}

int main(void) {
  char dir[] = "/tmp/nuvio-cw-progress-XXXXXX";
  CatItem v[4];
  CatRow rows[2];
  const CatItem *c;
  assert(mkdtemp(dir));
  cat_dir_writing(dir);

  card(&v[0], "tt0121955:29:4", "series", 29, 4);   // next up, not started
  snprintf(v[0].nameEpisode, sizeof v[0].nameEpisode, "Next one");
  card(&v[1], "tt1442437:7:8", "series", 7, 8);     // in progress
  v[1].progress = 54;
  card(&v[2], "tt0121955", "series", 0, 0);         // the same show in a catalogue
  card(&v[3], "tt27714581", "movie", 0, 0);
  memset(rows, 0, sizeof rows);
  snprintf(rows[0].key, sizeof rows[0].key, "continue_watching");
  rows[0].start = 0; rows[0].n = 2;
  snprintf(rows[1].key, sizeof rows[1].key, "popular");
  rows[1].start = 2; rows[1].n = 2;
  cat_set_all(v, 4, rows, 2);

  // The sync's write: the account's row for the finished episode, onto the first
  // item of the work.
  cat_save_progress_at(cat_index_by_imdb("tt0121955"), 1335, 1335, 29, 3,
                       1794043211000LL, 2);
  c = cat_item(0);
  assert(!strcmp(c->imdb, "tt0121955:29:4"));
  assert(c->season == 29 && c->episode == 4);
  assert(c->progress == 0);
  assert(!strcmp(c->nameEpisode, "Next one"));
  // The catalogue's copy, which names no episode, does take it.
  c = cat_item(2);
  assert(c->progress == 100 && c->season == 29 && c->episode == 3);

  // The account moving the in-progress episode on is still taken.
  cat_save_progress_at(1, 900, 1298, 7, 8, 1794043300000LL, 2);
  assert(cat_item(1)->progress == 69);

  // Playback HERE still updates the card being played, whatever it names.
  cat_save_progress_at(0, 600, 1300, 29, 4, 0, 1);
  assert(cat_item(0)->progress == 46 && cat_item(0)->episode == 4);

  // And a swap (cat_set_all reapplies the file) keeps the rule: the line on disk
  // is now S29E4 at 46%, which the card for S29E4 takes.
  card(&v[0], "tt0121955:29:5", "series", 29, 5);
  cat_set_all(v, 4, rows, 2);
  assert(cat_item(0)->progress == 0 && cat_item(0)->episode == 5);

  // Mark as watched keeps the work out of the row; unmarking lets it back in —
  // and only that work.
  cat_cw_dismiss("tt0121955:29:5");
  cat_cw_dismiss("tt27714581");
  assert(cat_cw_dismissed("tt0121955:29:6", 1794043211000LL));
  cat_cw_undismiss("tt0121955");
  assert(!cat_cw_dismissed("tt0121955:29:6", 1794043211000LL));
  assert(cat_cw_dismissed("tt27714581", 1794043211000LL));

  {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    system(cmd);
  }
  puts("cw_progress: ok");
  return 0;
}
