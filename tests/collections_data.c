// Exercises the real paged reader with a fake network, including stale responses.
#include <assert.h>
#include <unistd.h>
#include "../src/discover.c"
static int calls;
static pthread_mutex_t fakeLock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fakeCond=PTHREAD_COND_INITIALIZER;
static int slowStarted,releaseSlow;
char *net_download(const char *url,int timeout) {
  (void)timeout;calls++;
  if(strstr(url,"/slow/")) {
    pthread_mutex_lock(&fakeLock);slowStarted=1;pthread_cond_broadcast(&fakeCond);
    while(!releaseSlow)pthread_cond_wait(&fakeCond,&fakeLock);
    pthread_mutex_unlock(&fakeLock);
    return strdup("{\"metas\":[{\"id\":\"ttold\",\"name\":\"OLD\",\"poster\":\"old.jpg\"}]}");
  }
  if(strstr(url,"/error/"))return NULL;
  if(strstr(url,"/empty/"))return strdup("{\"metas\":[]}");
  if(strstr(url,"skip=2"))return strdup("{\"metas\":[{\"id\":\"tt3\",\"name\":\"Third\",\"poster\":\"3.jpg\"}]}");
  if(strstr(url,"skip=3"))return strdup("{\"metas\":[]}");
  return strdup("{\"metas\":[{\"id\":\"tt1\",\"name\":\"First\",\"poster\":\"1.jpg\"},{\"id\":\"tt2\",\"name\":\"Second\",\"poster\":\"2.jpg\"}]}");
}
static void waitDone(void) {for(int i=0;i<2000&&disc_seeall_loading();i++)usleep(1000);assert(!disc_seeall_loading());}
int main(void) {
  disc_seeall_open("https://example.invalid","movie","rank");waitDone();
  assert(disc_seeall_n()==2&&!disc_seeall_end());
  disc_seeall_more();waitDone();assert(disc_seeall_n()==3);
  CatItem i;assert(disc_seeall_item(2,&i)&&!strcmp(i.imdb,"tt3"));
  disc_seeall_more();waitDone();assert(disc_seeall_end());
  disc_seeall_open("https://example.invalid/slow","movie","slow");
  pthread_mutex_lock(&fakeLock);while(!slowStarted)pthread_cond_wait(&fakeCond,&fakeLock);pthread_mutex_unlock(&fakeLock);
  disc_seeall_open("https://example.invalid/new","series","new");
  pthread_mutex_lock(&fakeLock);releaseSlow=1;pthread_cond_broadcast(&fakeCond);pthread_mutex_unlock(&fakeLock);
  waitDone();assert(disc_seeall_n()==2);assert(disc_seeall_item(0,&i)&&!strcmp(i.kind,"series")&&!strcmp(i.imdb,"tt1"));
  disc_seeall_open("https://example.invalid/error","movie","error");waitDone();assert(disc_seeall_error()&&!disc_seeall_end());
  int before=calls;disc_seeall_more();waitDone();assert(calls>before);
  disc_seeall_open("https://example.invalid/empty","movie","empty");waitDone();assert(!disc_seeall_error()&&disc_seeall_end()&&disc_seeall_n()==0);
  puts("collections data: PASS (short pages, rank order, stale tab discarded, retry, empty)");
}
