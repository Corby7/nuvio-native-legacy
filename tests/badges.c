#include <assert.h>
#include "../src/badges.c"
int main(void){
  uint64_t m=badges_detectar("Movie.2160p.WEB-DL.DV.Atmos.5.1.HEVC.NFLX");
  assert(m&bit("r-4k"));assert(m&bit("a-atmos-dv"));assert(!(m&bit("v-dv")));
  assert(m&bit("co-x265"));assert(m&bit("p-netflix"));assert(m&bit("c-51"));
  assert(!badges_detectar("Adventure.S01E01.mkv"));
  m=badges_detectar("1080p HDR10+ DTS-HD MA 7.1 BluRay");
  assert(m&bit("v-hdr10plus"));assert(!(m&bit("v-hdr10")));assert(m&bit("a-dtshdma"));assert(!(m&bit("a-dts")));
  assert(!badges_provedor("unknown"));assert(badges_provedor("Apple TV+")==bit("p-appletv"));
  assert(!(badges_detectar("Movie 1080p 4k")&bit("r-4k")));
  puts("badges: PASS (metadata, combined Dolby, hierarchy, unknown omitted)");
}
