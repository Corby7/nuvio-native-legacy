#include "dados.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[512];
static char clienteId[64];

// Tenta criar a pasta e escrever nela. Criar nao basta: em varios pontos do
// sistema de arquivos do aparelho o mkdir passa e o open falha depois, e um
// teste que so olha o mkdir escolheria uma pasta onde nada e gravado.
static int serve(const char *candidato) {
  char teste[600];
  FILE *f;
  if (!candidato || !*candidato) return 0;
  mkdir(candidato, 0755);   // ja existir nao e erro para o que interessa aqui
  snprintf(teste, sizeof teste, "%s/.escrita", candidato);
  f = fopen(teste, "w");
  if (!f) return 0;
  if (fputs("ok\n", f) < 0) { fclose(f); return 0; }
  if (fclose(f) != 0) return 0;
  remove(teste);
  return 1;
}

void dados_iniciar(const char *dirArte) {
  char lar[512];
  const char *env = getenv("NUVIO_DADOS");
  const char *home = getenv("HOME");
  const char *candidatos[5];
  int n = 0, i;

  if (env && *env) candidatos[n++] = env;
  if (home && *home) {
    snprintf(lar, sizeof lar, "%s/.nuvio", home);
    candidatos[n++] = lar;
  }
  // Pasta de trabalho do modo desenvolvedor do webOS. Existe e e gravavel nos
  // aparelhos onde este app roda hoje; num aparelho de loja pode nao existir, e
  // por isso ela e candidata e nao resposta.
  candidatos[n++] = "/media/developer/temp/nuvio";
  if (dirArte && *dirArte) candidatos[n++] = dirArte;

  for (i = 0; i < n; i++) {
    if (serve(candidatos[i])) {
      snprintf(dir, sizeof dir, "%s", candidatos[i]);
      printf("[dados] gravando em %s\n", dir);
      fflush(stdout);
      return;
    }
    printf("[dados] recusou %s\n", candidatos[i]);
  }
  dir[0] = 0;
  printf("[dados] NENHUMA pasta gravavel: sessao e ajustes nao vao sobreviver "
         "ao proximo arranque\n");
  fflush(stdout);
}

const char *dados_dir(void) { return dir; }

char *dados_caminho(char *dst, unsigned tam, const char *nome) {
  if (!dir[0] || !nome || !*nome) return NULL;
  snprintf(dst, tam, "%s/%s", dir, nome);
  return dst;
}

int dados_gravar(const char *nome, const char *conteudo) {
  char caminho[600], tmp[600];
  FILE *f;
  size_t n;
  if (!dados_caminho(caminho, sizeof caminho, nome)) return 0;
  snprintf(tmp, sizeof tmp, "%s.tmp", caminho);
  f = fopen(tmp, "w");
  if (!f) return 0;
  n = conteudo ? strlen(conteudo) : 0;
  if (n && fwrite(conteudo, 1, n, f) != n) { fclose(f); remove(tmp); return 0; }
  if (fclose(f) != 0) { remove(tmp); return 0; }
  if (rename(tmp, caminho) != 0) { remove(tmp); return 0; }
  return 1;
}

char *dados_ler(const char *nome) {
  char caminho[600];
  FILE *f;
  long n;
  char *buf;
  if (!dados_caminho(caminho, sizeof caminho, nome)) return NULL;
  f = fopen(caminho, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  n = (long)fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[n] = 0;
  return buf;
}

int dados_apagar(const char *nome) {
  char caminho[600];
  if (!dados_caminho(caminho, sizeof caminho, nome)) return 0;
  return remove(caminho) == 0;
}

void dados_uuid(char *dst, unsigned tam) {
  static const char *hex = "0123456789abcdef";
  static int semeado;
  int i;
  if (tam < 37) { if (tam) dst[0] = 0; return; }
  if (!semeado) {
    srand((unsigned)time(NULL) ^ (unsigned)getpid() ^ (unsigned)(size_t)dst);
    semeado = 1;
  }
  for (i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { dst[i] = '-'; continue; }
    if (i == 14) { dst[i] = '4'; continue; }              // versao
    if (i == 19) { dst[i] = hex[8 + (rand() & 3)]; continue; }  // variante
    dst[i] = hex[rand() & 15];
  }
  dst[36] = 0;
}

const char *dados_cliente_id(void) {
  char *lido;
  if (clienteId[0]) return clienteId;

  lido = dados_ler("cliente.txt");
  if (lido) {
    char *fim = lido + strlen(lido);
    while (fim > lido && (fim[-1] == '\n' || fim[-1] == '\r' || fim[-1] == ' ')) *--fim = 0;
    if (lido[0]) snprintf(clienteId, sizeof clienteId, "%s", lido);
    free(lido);
    if (clienteId[0]) return clienteId;
  }

  // Formato de UUID v4 porque e o que o servidor recebe do Android e do web; a
  // aleatoriedade nao precisa ser criptografica — este numero identifica um
  // aparelho para nao ecoar a propria escrita, nao protege nada.
  dados_uuid(clienteId, sizeof clienteId);

  { char linha[64];
    snprintf(linha, sizeof linha, "%s\n", clienteId);
    dados_gravar("cliente.txt", linha); }
  return clienteId;
}
