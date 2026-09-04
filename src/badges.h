#ifndef NV_BADGES_H
#define NV_BADGES_H
#include <stdint.h>
void badges_load(const char *dir);
uint64_t badges_detect(const char *metadata);
uint64_t badges_provider(const char *name);
float badges_draw(uint64_t mask,float x,float y,float maxW,float height,float alpha);
#endif
