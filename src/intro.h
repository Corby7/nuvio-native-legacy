#ifndef NV_INTRO_H
#define NV_INTRO_H
typedef struct { double start,end; int kind; } IntroChunk;
enum { INTRO_OPENING=1, INTRO_SUMMARY=2, INTRO_CREDITS=3 };
void intro_request(const char *imdb,int season,int episode);
void intro_off(void);
int  intro_active(double posSeg,double *end,int *kind);
// Where the credits begin, in seconds; -1 when this episode has no credits chunk.
double intro_credits_start(void);
// A copy of this episode's chunks, for drawing them; returns how many (0 until loaded).
int  intro_chunks(IntroChunk *out,int max);
int  intro_parse(const char *json,IntroChunk *output,int max);
#endif
