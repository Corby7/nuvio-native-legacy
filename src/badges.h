#ifndef NV_BADGES_H
#define NV_BADGES_H
#include <stdint.h>
void badges_carregar(const char *dir);
uint64_t badges_detectar(const char *metadata);
uint64_t badges_provedor(const char *name);
float badges_desenhar(uint64_t mask,float x,float y,float maxW,float height,float alpha);
#endif
