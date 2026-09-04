// Sem rede real: confirma que a busca nao aceita homonimo de ator e publica
// a ficha somente depois de validar o nome e o departamento no TMDB.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/director.c"

const char *disc_key_tmdb(void) { return "test-key"; }
void disc_date_long(const char *iso, char *dst, size_t size) {
  snprintf(dst, size, "%s", iso);
}

char *net_download(const char *url, int seconds) {
  (void)seconds;
  if (strstr(url, "/search/person?")) {
    const char *json =
      "{\"results\":["
      "{\"id\":11,\"name\":\"Alex Silva\",\"known_for_department\":\"Acting\",\"profile_path\":\"/wrong.jpg\"},"
      "{\"id\":22,\"name\":\"Alex Silva\",\"known_for_department\":\"Directing\",\"profile_path\":\"/right.jpg\",\"known_for\":[{\"title\":\"Filme\",\"backdrop_path\":\"/hero.jpg\"}]}]}";
    return strdup(json);
  }
  if (strstr(url, "/person/22?")) {
    return strdup("{\"known_for_department\":\"Directing\",\"biography\":\"A real profile.\",\"birthday\":\"1970-01-01\",\"place_of_birth\":\"Brazil\"}");
  }
  return NULL;
}

static int wait_profile(const char *name) {
  for (int i = 0; i < 100; i++) {
    if (director_ready(name)) return 1;
    usleep(10000);
  }
  return 0;
}

int main(void) {
  director_request("alex silva");
  assert(wait_profile("Alex Silva"));
  assert(strstr(director_photo("Alex Silva"), "/right.jpg"));
  assert(strcmp(director_photo("Alex Silva"), "https://image.tmdb.org/t/p/original/right.jpg") == 0);
  assert(strcmp(director_hero("Alex Silva"), "https://image.tmdb.org/t/p/original/hero.jpg") == 0);
  assert(strstr(director_bio("Alex Silva"), "real profile"));
  puts("director cache: PASS (identity, department, published profile)");
  return 0;
}
