#include "collections.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  assert(col_load("deploy/app/art")>0);
  int checked=0;
  for(int i=0;i<col_n();i++) {
    const ColFolder *f=col_folder(i);
    if(strcmp(f->group,"Directors")&&strcmp(f->group,"Awards"))continue;
    assert(f->editorial&&f->detailHero[0]);
    assert(strstr(f->hero,f->id)&&strstr(f->detailHero,f->id));
    assert(strcmp(f->hero,f->detailHero));checked++;
  }
  assert(checked==23);
  puts("editorial loader: PASS (23 complete identity-specific pairs)");
}
