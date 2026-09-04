#ifndef NV_INTRO_H
#define NV_INTRO_H
typedef struct { double start,end; int kind; } IntroChunk;
enum { INTRO_OPENING=1, INTRO_SUMMARY=2, INTRO_CREDITS=3 };
void intro_request(const char *imdb,int season,int episode);
void intro_off(void);
int  intro_active(double posSeg,double *end,int *kind);
int  intro_parse(const char *json,IntroChunk *output,int max);
#endif
