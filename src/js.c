#include "js.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *pula(const char *p) {
  while (*p && (unsigned char)*p <= ' ') p++;
  return p;
}

const char *js_fim(const char *p) {
  int prof = 0, texto = 0;
  char abre = *p, fecha = (abre == '[') ? ']' : '}';
  if (abre != '[' && abre != '{') return p;
  for (; *p; p++) {
    if (texto) { if (*p == '\\') p++; else if (*p == '"') texto = 0; continue; }
    if (*p == '"') texto = 1;
    else if (*p == abre) prof++;
    else if (*p == fecha && --prof == 0) return p + 1;
  }
  return p;
}

// Acha `"chave"` dentro da faixa, ignorando ocorrencias dentro de textos.
static const char *achaChave(const char *ini, const char *fim, const char *chave) {
  char busca[64];
  const char *p = ini;
  size_t n;
  snprintf(busca, sizeof busca, "\"%s\"", chave);
  n = strlen(busca);
  while ((p = strstr(p, busca)) != NULL) {
    if (fim && p >= fim) return NULL;
    { const char *q = pula(p + n);
      if (*q == ':') return q + 1; }
    p += n;
  }
  return NULL;
}

int js_texto(const char *ini, const char *fim, const char *chave,
             char *dst, size_t tam) {
  const char *p = achaChave(ini, fim, chave);
  size_t k = 0;
  if (!p) return 0;
  p = pula(p);
  if (*p != '"') return 0;
  p++;
  while (*p && *p != '"' && k + 1 < tam) {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'u') { p += 5; dst[k++] = ' '; continue; }
      if (*p == 'n' || *p == 't' || *p == 'r') { p++; dst[k++] = ' '; continue; }
      if (*p == '/' ) { p++; dst[k++] = '/'; continue; }
    }
    dst[k++] = *p++;
  }
  dst[k] = 0;
  return k > 0;
}

double js_num(const char *ini, const char *fim, const char *chave, double padrao) {
  char busca[64];
  const char *p = ini;
  size_t n;
  snprintf(busca, sizeof busca, "\"%s\"", chave);
  n = strlen(busca);
  while ((p = strstr(p, busca)) != NULL) {
    const char *q;
    if (fim && p >= fim) break;
    q = pula(p + n);
    if (*q == ':') {
      q = pula(q + 1);
      if ((*q >= '0' && *q <= '9') || *q == '-' || *q == '.') return atof(q);
    }
    p += n;
  }
  return padrao;
}

const char *js_array(const char *ini, const char *fim, const char *chave) {
  const char *p = achaChave(ini, fim, chave);
  if (!p) return NULL;
  p = pula(p);
  if (*p != '[') return NULL;
  p = pula(p + 1);
  return (*p == '{' || *p == '"') ? p : NULL;
}

const char *js_prox(const char *fimAnterior) {
  const char *p = pula(fimAnterior);
  if (*p == ',') {
    p = pula(p + 1);
    return (*p == '{' || *p == '"') ? p : NULL;
  }
  return NULL;
}
