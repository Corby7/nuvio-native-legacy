// Parser isolado: permite ASan sem carregar o inicializador de SDL do macOS.
#include "streams.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
  char json[32000];size_t n=0;
  n+=snprintf(json+n,sizeof json-n,"{\"streams\":[");
  for(int i=0;i<100;i++) n+=snprintf(json+n,sizeof json-n,
    "%s{\"url\":\"https://example.invalid/%d\",\"behaviorHints\":{\"filename\":\"title.%s\"}}",
    i?",":"",i,i==99?"2160p.DV.Atmos.mp4":"1080p.DVDRip.mkv");
  snprintf(json+n,sizeof json-n,"]}");
  Stream *v=NULL;int count=stream_parse(json,"fixture",&v);
  assert(count==100 && v[99].mp4 && v[99].dolbyVision && v[99].height==2160);
  assert(!v[0].dolbyVision);free(v);
  count=stream_parse("{\"streams\":[]}","fixture",&v);assert(count==0);free(v);
  puts("PASS ASan/UBSan: parser in isolation, 100 sources, MP4/DV in the last position.");
}
