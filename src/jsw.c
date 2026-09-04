#include "jsw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void por(Jsw *w, const char *s, size_t n) {
  if (w->error || !s) return;
  if (w->n + n + 1 > w->cap) {
    size_t new = w->cap ? w->cap * 2 : 256;
    char *q;
    while (new < w->n + n + 1) new *= 2;
    q = (char *)realloc(w->p, new);
    if (!q) { w->error = 1; return; }
    w->p = q;
    w->cap = new;
  }
  memcpy(w->p + w->n, s, n);
  w->n += n;
  w->p[w->n] = 0;
}

static void pct(Jsw *w, char c) { por(w, &c, 1); }

// Chamado ANTES de todo valor: escreve a virgula que separa do anterior. E a
// unica virgula do arquivo, e por isso ela nunca sobra no fim de uma lista.
static void split(Jsw *w) {
  if (w->error) return;
  if (w->depth > 0 && w->depth <= JSW_DEPTH) {
    if (w->first[w->depth - 1]) w->first[w->depth - 1] = 0;
    else pct(w, ',');
  }
}

static void enter(Jsw *w, char openCh) {
  split(w);
  pct(w, openCh);
  if (w->depth >= JSW_DEPTH) { w->error = 1; return; }
  w->first[w->depth] = 1;
  w->depth++;
}

static void exitLevel(Jsw *w, char closeCh) {
  if (w->depth > 0) w->depth--;
  pct(w, closeCh);
}

void jsw_start(Jsw *w) {
  memset(w, 0, sizeof *w);
}

void jsw_free(Jsw *w) {
  free(w->p);
  memset(w, 0, sizeof *w);
}

const char *jsw_text_final(const Jsw *w) {
  return (w->error || !w->p) ? NULL : w->p;
}

void jsw_obj_start(Jsw *w) { enter(w, '{'); }
void jsw_obj_end(Jsw *w) { exitLevel(w, '}'); }
void jsw_arr_start(Jsw *w) { enter(w, '['); }
void jsw_arr_end(Jsw *w) { exitLevel(w, ']'); }

// A string em si, SEM passar pelo separador. Existe por causa de um defeito
// real: jsw_chave separava e depois chamava jsw_str, que separava de novo — e
// como o primeiro separar ja tinha consumido a marca de "primeiro do nivel", o
// segundo escrevia uma virgula. O corpo saia como {,"a":1} e o servidor
// respondia "Empty or invalid json", sem dizer onde.
static void writeStr(Jsw *w, const char *s) {
  const unsigned char *p;
  pct(w, '"');
  for (p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"':  por(w, "\\\"", 2); break;
      case '\\': por(w, "\\\\", 2); break;
      case '\n': por(w, "\\n", 2);  break;
      case '\r': por(w, "\\r", 2);  break;
      case '\t': por(w, "\\t", 2);  break;
      default:
        // Controle abaixo de 0x20 TEM de virar \u00XX; solto, ele torna o
        // documento invalido e o servidor recusa o corpo inteiro. Acima de
        // 0x7F o byte passa cru: o conteudo daqui e UTF-8, que e o que o JSON
        // espera, e escapar byte a byte quebraria os acentos.
        if (*p < 0x20) {
          char u[7];
          snprintf(u, sizeof u, "\\u%04x", (unsigned)*p);
          por(w, u, 6);
        } else {
          pct(w, (char)*p);
        }
    }
  }
  pct(w, '"');
}

void jsw_str(Jsw *w, const char *s) {
  if (!s) { jsw_null(w); return; }
  split(w);
  writeStr(w, s);
}

// A chave leva a virgula do nivel; o valor que vem depois dela nao leva outra,
// e por isso a marca de "primeiro" e reposta logo em seguida.
void jsw_key(Jsw *w, const char *name) {
  split(w);
  writeStr(w, name ? name : "");
  pct(w, ':');
  if (w->depth > 0 && w->depth <= JSW_DEPTH) w->first[w->depth - 1] = 1;
}

void jsw_num(Jsw *w, double v) {
  char b[40];
  split(w);
  // %.17g reproduz o double exatamente; %g simples arredonda e um progresso de
  // reproducao volta diferente do que foi enviado.
  snprintf(b, sizeof b, "%.17g", v);
  por(w, b, strlen(b));
}

void jsw_int(Jsw *w, long long v) {
  char b[32];
  split(w);
  snprintf(b, sizeof b, "%lld", v);
  por(w, b, strlen(b));
}

void jsw_bool(Jsw *w, int v) {
  split(w);
  if (v) por(w, "true", 4); else por(w, "false", 5);
}

void jsw_null(Jsw *w) {
  split(w);
  por(w, "null", 4);
}

void jsw_raw(Jsw *w, const char *json) {
  if (!json || !*json) { jsw_null(w); return; }
  split(w);
  por(w, json, strlen(json));
}

void jsw_cs(Jsw *w, const char *key, const char *value) {
  jsw_key(w, key);
  jsw_str(w, value);
}

void jsw_ci(Jsw *w, const char *key, long long value) {
  jsw_key(w, key);
  jsw_int(w, value);
}

void jsw_cb(Jsw *w, const char *key, int value) {
  jsw_key(w, key);
  jsw_bool(w, value);
}
