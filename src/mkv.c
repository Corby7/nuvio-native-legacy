#include "mkv.h"
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Quanto do arquivo baixar. O elemento Tracks fica logo apos o SeekHead e o
// Info, antes do primeiro Cluster — na pratica dentro dos primeiros 200 KB.
//
// 320 KB e nao 2 MB. Os 2 MB eram "folga larga" para remux com capa embutida
// antes do Tracks, e custavam caro no unico momento em que esta leitura
// acontece: COM O VIDEO JA TOCANDO, pela mesma conexao e do mesmo servidor.
// MEDIDO na TV do dono — a leitura terminou aos 45,9 s e o buffer entrou em
// falta 1,6 s depois, caindo a 2,8 s e levando 9 s para se recuperar.
//
// O caso que os 2 MB cobriam (capa antes do Tracks) e raro; o custo era pago
// em TODA reproducao. Perder o idioma num arquivo desses e melhor que engasgar
// o video em todos.
#define MKV_CHUNK  (320L * 1024)

// --- EBML: inteiros de tamanho variavel --------------------------------------
//
// O primeiro byte diz, pela posicao do bit 1 mais alto, quantos bytes o numero
// ocupa. No ID esse bit FAZ PARTE do valor (por isso os IDs sao escritos como
// 0x1A45DFA3); no TAMANHO ele e mascara e sai fora. Trocar os dois e o erro
// classico de quem escreve isto pela primeira vez, e o sintoma e a arvore
// inteira sair deslocada.
static int widthOf(unsigned char b) {
  int i;
  for (i = 0; i < 8; i++) if (b & (0x80 >> i)) return i + 1;
  return 0;                       // byte 0x00: invalido em EBML
}

// Le um ID (mantendo o bit marcador). 0 e o fim ou dado invalido.
static unsigned long readId(const unsigned char *p, long remains, int *used) {
  int w, i;
  unsigned long v;
  if (remains < 1) return 0;
  w = widthOf(p[0]);
  if (w < 1 || w > 4 || remains < w) return 0;
  v = 0;
  for (i = 0; i < w; i++) v = (v << 8) | p[i];
  *used = w;
  return v;
}

// Le um TAMANHO (removendo o bit marcador). Devolve -1 no invalido e -2 no
// tamanho "desconhecido" (todos os bits de dado em 1), que Segment usa em
// arquivo transmitido ao vivo — ali a leitura continua DENTRO do elemento em
// vez de pular por cima dele.
static long readSize(const unsigned char *p, long remains, int *used) {
  int w, i;
  unsigned long v;
  int allUm = 1;
  if (remains < 1) return -1;
  w = widthOf(p[0]);
  if (w < 1 || w > 8 || remains < w) return -1;
  v = p[0] & (0xFF >> w);
  if ((unsigned char)(p[0] & (0xFF >> w)) != (unsigned char)(0xFF >> w)) allUm = 0;
  for (i = 1; i < w; i++) {
    if (p[i] != 0xFF) allUm = 0;
    v = (v << 8) | p[i];
  }
  *used = w;
  if (allUm) return -2;
  return (long)v;
}

static unsigned long readUint(const unsigned char *p, long n) {
  unsigned long v = 0;
  long i;
  if (n < 1 || n > 8) return 0;
  for (i = 0; i < n; i++) v = (v << 8) | p[i];
  return v;
}

static void readText(const unsigned char *p, long n, char *dst, size_t size) {
  size_t k = (size_t)n;
  if (k > size - 1) k = size - 1;
  memcpy(dst, p, k);
  dst[k] = 0;
  // O Matroska preenche string com NUL a direita; cortar aqui evita que o
  // resto do campo vire lixo na tela.
  { size_t i; for (i = 0; i < k; i++) if (dst[i] == 0) { dst[i] = 0; break; } }
}

// --- ids que interessam ------------------------------------------------------
#define ID_SEGMENT     0x18538067UL
#define ID_TRACKS      0x1654AE6BUL
#define ID_TRACKENTRY  0xAEUL
#define ID_TRACKNUMBER 0xD7UL
#define ID_TRACKTYPE   0x83UL
#define ID_LANGUAGE    0x22B59CUL     // Language (ISO 639-2), o classico
#define ID_LANG_BCP47  0x22B59DUL     // LanguageBCP47 ("pt-BR"), mais novo
#define ID_NAME        0x536EUL
#define ID_CODECID     0x86UL

// Le os TrackEntry de dentro de um Tracks ja localizado.
static int readTracks(const unsigned char *p, long n, MkvTrack *output, int max) {
  long o = 0;
  int found = 0;
  while (o < n && found < max) {
    int ui = 0, ut = 0;
    unsigned long id = readId(p + o, n - o, &ui);
    long size;
    if (!id) break;
    size = readSize(p + o + ui, n - o - ui, &ut);
    if (size < 0) break;
    o += ui + ut;
    if (o + size > n) break;
    if (id == ID_TRACKENTRY) {
      MkvTrack f;
      long q = 0;
      memset(&f, 0, sizeof f);
      while (q < size) {
        int vi = 0, vt = 0;
        unsigned long fid = readId(p + o + q, size - q, &vi);
        long fontSize;
        if (!fid) break;
        fontSize = readSize(p + o + q + vi, size - q - vi, &vt);
        if (fontSize < 0) break;
        q += vi + vt;
        if (q + fontSize > size) break;
        { const unsigned char *v = p + o + q;
          if (fid == ID_TRACKNUMBER) f.number = (int)readUint(v, fontSize);
          else if (fid == ID_TRACKTYPE) f.kind = (int)readUint(v, fontSize);
          else if (fid == ID_LANGUAGE || fid == ID_LANG_BCP47) {
            // BCP47 ganha do ISO 639-2 quando os dois existem: "pt-BR" diz
            // mais que "por", e e o que o dono quer ver na lista.
            if (fid == ID_LANG_BCP47 || !f.language[0])
              readText(v, fontSize, f.language, sizeof f.language);
          }
          else if (fid == ID_NAME)    readText(v, fontSize, f.name,  sizeof f.name);
          else if (fid == ID_CODECID) readText(v, fontSize, f.codec, sizeof f.codec); }
        q += fontSize;
      }
      if (f.number > 0) output[found++] = f;
    }
    o += size;
  }
  return found;
}

// Anda pela arvore ate achar Tracks. Entra em Segment (que e um contentor
// gigante) e PULA o resto — sem o pulo a busca varreria byte a byte e casaria
// com qualquer coincidencia dentro dos dados de video.
static int findTracks(const unsigned char *p, long n, MkvTrack *output, int max) {
  long o = 0;
  while (o < n) {
    int ui = 0, ut = 0;
    unsigned long id = readId(p + o, n - o, &ui);
    long size;
    if (!id) return 0;
    size = readSize(p + o + ui, n - o - ui, &ut);
    if (size == -1) return 0;
    o += ui + ut;
    if (id == ID_SEGMENT || size == -2) {
      // Segment: descer para dentro. Tamanho desconhecido idem — nao ha por
      // onde pular.
      if (id == ID_SEGMENT) continue;
      return 0;
    }
    if (id == ID_TRACKS) {
      long disp = n - o;
      if (size > disp) size = disp;     // cabecalho maior que o trecho baixado
      return readTracks(p + o, size, output, max);
    }
    if (o + size > n) return 0;        // elemento passa do que baixamos
    o += size;
  }
  return 0;
}

int mkv_tracks(const char *url, MkvTrack *output, int max) {
  char *buf;
  long n = 0;
  int found;
  if (!url || !url[0] || !output || max < 1) return 0;
  buf = net_download_chunk(url, 20, 0, MKV_CHUNK - 1, &n);
  if (!buf) return 0;
  // Assinatura EBML. Sem ela nao e Matroska (pode ser MP4, ou um HTML de erro
  // que o servidor devolveu com 200), e seguir seria interpretar lixo.
  if (n < 64 || (unsigned char)buf[0] != 0x1A || (unsigned char)buf[1] != 0x45 ||
      (unsigned char)buf[2] != 0xDF || (unsigned char)buf[3] != 0xA3) {
    free(buf);
    return 0;
  }
  found = findTracks((const unsigned char *)buf, n, output, max);
  free(buf);
  printf("[mkv] %d tracks read from the header (%ld bytes)\n", found, n);
  fflush(stdout);
  return found;
}
