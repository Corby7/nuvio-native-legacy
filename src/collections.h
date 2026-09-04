#ifndef NV_COLLECTIONS_H
#define NV_COLLECTIONS_H
#define COL_MAX 256
#define COL_SOURCE_MAX 32
typedef struct { char title[128], base[600], type[8], catId[96], genre[96]; } ColSource;
typedef struct {
  char id[96], title[128], group[64], cover[512], hero[512], logo[512];
  char frameDir[600];
  char detailHero[512];
  int editorial;
  int frames, hideTitle, nSources;
  ColSource sources[COL_SOURCE_MAX];
} ColFolder;
int col_load(const char *dir);
int col_n(void);
const ColFolder *col_folder(int i);
int col_group(const char *name, int *indices, int max);
const ColFolder *col_by_catalog(const char *base, const char *type, const char *id);
void col_color(const ColFolder *f, float *r, float *g, float *b);
#endif
