#ifndef NV_COLECOES_H
#define NV_COLECOES_H
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
int col_carregar(const char *dir);
int col_n(void);
const ColFolder *col_folder(int i);
int col_grupo(const char *nome, int *indices, int max);
const ColFolder *col_por_catalogo(const char *base, const char *type, const char *id);
void col_cor(const ColFolder *f, float *r, float *g, float *b);
#endif
