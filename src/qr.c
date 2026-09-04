#include "qr.h"
#include <string.h>

// --------------------------------------------------------------- tabelas
//
// Versoes 1..6, correcao L. Cada linha: total de codewords, codewords de
// correcao POR BLOCO, e numero de blocos. Numeros da ISO 18004; a versao 6 e a
// unica destas que se divide em dois blocos, e e por isso que o entrelacamento
// abaixo e generico em vez de assumir bloco unico.
static const struct { int total, ecByBlock, blocks; } SEE[7] = {
  { 0,   0,  0 },
  { 26,  7,  1 },
  { 44, 10,  1 },
  { 70, 15,  1 },
  { 100,20,  1 },
  { 134,26,  1 },
  { 172,18,  2 }
};

// Centro do unico padrao de alinhamento de cada versao (o outro centro e
// sempre 6, e as tres combinacoes com ele caem em cima dos localizadores).
static const int ALIGN[7] = { 0, 0, 18, 22, 26, 30, 34 };

// --------------------------------------------------------------- GF(256)

static unsigned char gfExp[512], gfLog[256];

static void gfStart(void) {
  int i, x = 1;
  if (gfExp[0]) return;
  for (i = 0; i < 255; i++) {
    gfExp[i] = (unsigned char)x;
    gfLog[x] = (unsigned char)i;
    x <<= 1;
    if (x & 0x100) x ^= 0x11D;   // polinomio primitivo do QR
  }
  for (i = 255; i < 512; i++) gfExp[i] = gfExp[i - 255];
}

static unsigned char gfMul(unsigned char a, unsigned char b) {
  if (!a || !b) return 0;
  return gfExp[gfLog[a] + gfLog[b]];
}

// Divisao polinomial: o resto sao os codewords de correcao.
static void rs(const unsigned char *data, int nData, int nEc, unsigned char *output) {
  unsigned char ger[32], rest[32];
  int i, j, degree = 1;
  gfStart();
  memset(ger, 0, sizeof ger);
  ger[0] = 1;
  // Gerador = produto de (x - a^i). Construido no lugar, do grau 1 para cima.
  for (i = 0; i < nEc; i++) {
    for (j = degree; j > 0; j--) ger[j] = (unsigned char)(ger[j - 1] ^ gfMul(ger[j], gfExp[i]));
    ger[0] = gfMul(ger[0], gfExp[i]);
    degree++;
  }
  memset(rest, 0, sizeof rest);
  for (i = 0; i < nData; i++) {
    unsigned char factor = (unsigned char)(data[i] ^ rest[0]);
    memmove(rest, rest + 1, (size_t)nEc - 1);
    rest[nEc - 1] = 0;
    for (j = 0; j < nEc; j++) rest[j] ^= gfMul(ger[nEc - 1 - j], factor);
  }
  memcpy(output, rest, (size_t)nEc);
}

// --------------------------------------------------------------- matriz

// `func` marca os modulos de FUNCAO (localizadores, tempo, formato): eles nao
// recebem dados nem mascara. Sem essa marca separada, a mascara inverteria o
// padrao de tempo e nenhum leitor acharia o simbolo.
static unsigned char func[QR_MAX_SIDE * QR_MAX_SIDE];

static void por(Qr *q, int x, int y, int v) {
  if (x < 0 || y < 0 || x >= q->side || y >= q->side) return;
  q->m[y * q->side + x] = (unsigned char)(v ? 1 : 0);
}
static int take(const Qr *q, int x, int y) { return q->m[y * q->side + x]; }
static void mark(const Qr *q, int x, int y) {
  if (x < 0 || y < 0 || x >= q->side || y >= q->side) return;
  func[y * q->side + x] = 1;
}
static int ehFunc(const Qr *q, int x, int y) { return func[y * q->side + x]; }

static void locator(Qr *q, int cx, int cy) {
  int dx, dy;
  // Inclui a faixa de separacao (raio 4), que e clara e tambem e funcao.
  for (dy = -4; dy <= 4; dy++) {
    for (dx = -4; dx <= 4; dx++) {
      int x = cx + dx, y = cy + dy;
      int d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
      if (x < 0 || y < 0 || x >= q->side || y >= q->side) continue;
      por(q, x, y, d != 2 && d != 4);
      mark(q, x, y);
    }
  }
}

static void alignment(Qr *q, int cx, int cy) {
  int dx, dy;
  for (dy = -2; dy <= 2; dy++) {
    for (dx = -2; dx <= 2; dx++) {
      int d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
      por(q, cx + dx, cy + dy, d != 1);
      mark(q, cx + dx, cy + dy);
    }
  }
}

static void defaults(Qr *q, int version) {
  int i, n = q->side;
  memset(func, 0, sizeof func);
  memset(q->m, 0, sizeof q->m);

  locator(q, 3, 3);
  locator(q, n - 4, 3);
  locator(q, 3, n - 4);

  for (i = 8; i < n - 8; i++) {          // padroes de tempo
    por(q, i, 6, !(i & 1)); mark(q, i, 6);
    por(q, 6, i, !(i & 1)); mark(q, 6, i);
  }
  if (version >= 2) alignment(q, ALIGN[version], ALIGN[version]);

  por(q, 8, n - 8, 1); mark(q, 8, n - 8);   // modulo escuro fixo

  // Area do formato: reservada agora, escrita depois de escolher a mascara.
  for (i = 0; i <= 8; i++) { mark(q, i, 8); mark(q, 8, i); }
  for (i = 0; i < 8; i++)  { mark(q, n - 1 - i, 8); mark(q, 8, n - 1 - i); }
}

// --------------------------------------------------------------- mascara

static int mascara(int m, int x, int y) {
  switch (m) {
    case 0: return ((y + x) & 1) == 0;
    case 1: return (y & 1) == 0;
    case 2: return (x % 3) == 0;
    case 3: return ((y + x) % 3) == 0;
    case 4: return (((y / 2) + (x / 3)) & 1) == 0;
    case 5: return ((y * x) % 2 + (y * x) % 3) == 0;
    case 6: return (((y * x) % 2 + (y * x) % 3) & 1) == 0;
    default:return (((y + x) % 2 + (y * x) % 3) & 1) == 0;
  }
}

// As quatro penalidades da norma. Sem elas o simbolo ate e valido, mas escolhe
// mascaras ruins e a leitura falha em angulo ou com pouca luz.
static int penalty(const Qr *q) {
  int n = q->side, p = 0, x, y, dark = 0;

  for (y = 0; y < n; y++) {          // N1: corridas de 5 ou mais
    int race = 1;
    for (x = 1; x < n; x++) {
      if (take(q, x, y) == take(q, x - 1, y)) { race++; }
      else { if (race >= 5) p += 3 + (race - 5); race = 1; }
    }
    if (race >= 5) p += 3 + (race - 5);
  }
  for (x = 0; x < n; x++) {
    int race = 1;
    for (y = 1; y < n; y++) {
      if (take(q, x, y) == take(q, x, y - 1)) { race++; }
      else { if (race >= 5) p += 3 + (race - 5); race = 1; }
    }
    if (race >= 5) p += 3 + (race - 5);
  }
  for (y = 0; y + 1 < n; y++)        // N2: blocos 2x2 da mesma cor
    for (x = 0; x + 1 < n; x++) {
      int v = take(q, x, y);
      if (v == take(q, x + 1, y) && v == take(q, x, y + 1) && v == take(q, x + 1, y + 1))
        p += 3;
    }
  // N3: o padrao 1:1:3:1:1 com quatro claros de um lado — o mesmo desenho do
  // localizador, que confundiria o leitor se aparecesse no meio dos dados.
  for (y = 0; y < n; y++)
    for (x = 0; x < n; x++) {
      static const int target[7] = { 1, 0, 1, 1, 1, 0, 1 };
      int k, bate;
      if (x + 7 <= n) {
        bate = 1;
        for (k = 0; k < 7; k++) if (take(q, x + k, y) != target[k]) { bate = 0; break; }
        if (bate) {
          int before = 1, after = 1;
          for (k = 1; k <= 4; k++) if (x - k < 0 || take(q, x - k, y)) { before = 0; break; }
          for (k = 0; k < 4; k++) if (x + 7 + k >= n || take(q, x + 7 + k, y)) { after = 0; break; }
          if (before || after) p += 40;
        }
      }
      if (y + 7 <= n) {
        bate = 1;
        for (k = 0; k < 7; k++) if (take(q, x, y + k) != target[k]) { bate = 0; break; }
        if (bate) {
          int before = 1, after = 1;
          for (k = 1; k <= 4; k++) if (y - k < 0 || take(q, x, y - k)) { before = 0; break; }
          for (k = 0; k < 4; k++) if (y + 7 + k >= n || take(q, x, y + 7 + k)) { after = 0; break; }
          if (before || after) p += 40;
        }
      }
      if (take(q, x, y)) dark++;
    }
  { int total = n * n;               // N4: desequilibrio entre claro e escuro
    int pct = dark * 100 / total;
    int k = (pct >= 50 ? pct - 50 : 50 - pct) / 5;
    p += k * 10; }
  return p;
}

// --------------------------------------------------------------- formato

static void writeFormat(Qr *q, int m) {
  int n = q->side, i;
  // L = 01. BCH(15,5) com o gerador 0x537 e mascara final 0x5412 — os dois
  // numeros sao da norma; o XOR final existe para o formato nunca sair todo
  // zero, que seria indistinguivel de area em branco.
  unsigned data = (unsigned)((1 << 3) | m);
  unsigned bch = data << 10;
  for (i = 14; i >= 10; i--) if (bch & (1u << i)) bch ^= 0x537u << (i - 10);
  { unsigned f = ((data << 10) | bch) ^ 0x5412u;
    // ORDEM: a primeira posicao recebe o bit MAIS significativo, nao o menos.
    // Este foi o erro que custou mais tempo — escrito ao contrario o simbolo
    // desenha perfeito, os localizadores estao certos, o olho nao ve nada, e
    // NENHUM leitor decodifica. Conferido lendo o formato de um simbolo gerado
    // por outra implementacao e comparando com o valor calculado aqui.
    #define BIT_F(k) ((f >> (14 - (k))) & 1)
    for (i = 0; i <= 5; i++)  por(q, i, 8, BIT_F(i));
    por(q, 7, 8, BIT_F(6));
    por(q, 8, 8, BIT_F(7));
    por(q, 8, 7, BIT_F(8));
    for (i = 9; i <= 14; i++) por(q, 8, 14 - i, BIT_F(i));
    // Segunda copia: os 7 primeiros bits descem pela COLUNA 8 a partir da base,
    // e os 8 ultimos correm pela LINHA 8 ate a borda direita. A divisao NAO e
    // 8+7 como a da primeira copia.
    for (i = 0; i <= 6; i++)  por(q, 8, n - 1 - i, BIT_F(i));
    for (i = 7; i <= 14; i++) por(q, n - 15 + i, 8, BIT_F(i));
    #undef BIT_F
  }
}

// --------------------------------------------------------------- geracao

int qr_generate(Qr *q, const char *text) {
  unsigned char bits[200], blocks[172], output[172];
  int nText, version, nData, i, j, b;
  int nBits = 0;

  if (!q || !text) return 0;
  memset(q, 0, sizeof *q);
  nText = (int)strlen(text);
  if (nText <= 0) return 0;

  // Menor versao que cabe. O cabecalho do modo byte custa 12 bits (4 de modo +
  // 8 de contagem), o que da 1,5 byte — por isso a conta usa bits e nao bytes.
  for (version = 1; version <= 6; version++) {
    nData = SEE[version].total - SEE[version].ecByBlock * SEE[version].blocks;
    if (4 + 8 + nText * 8 <= nData * 8) break;
  }
  if (version > 6) return 0;
  nData = SEE[version].total - SEE[version].ecByBlock * SEE[version].blocks;

  // --- fluxo de bits
  memset(bits, 0, sizeof bits);
  { int k;
    #define POR_BITS(value, count) \
      for (k = (count) - 1; k >= 0; k--) { \
        if (((value) >> k) & 1) bits[nBits >> 3] |= (unsigned char)(0x80 >> (nBits & 7)); \
        nBits++; }
    POR_BITS(4, 4);            // modo byte
    POR_BITS(nText, 8);       // contagem (8 bits ate a versao 9)
    for (i = 0; i < nText; i++) { POR_BITS((unsigned char)text[i], 8); }
    // Terminador de ate 4 zeros e alinhamento ao byte; depois o enchimento
    // alternado da norma.
    { int missing = nData * 8 - nBits;
      int term = missing > 4 ? 4 : missing;
      POR_BITS(0, term); }
    while (nBits & 7) { POR_BITS(0, 1); }
    #undef POR_BITS
  }
  { int nBytes = nBits / 8, toggles = 0;
    while (nBytes < nData) bits[nBytes++] = (toggles++ & 1) ? 0x11 : 0xEC; }

  // --- blocos e entrelacamento
  { int nBlocks = SEE[version].blocks, nEc = SEE[version].ecByBlock;
    int short_ = nData / nBlocks, rest = nData % nBlocks;
    int off = 0, maxLen = 0;
    unsigned char ec[8][32];
    int size[8], start[8];
    for (b = 0; b < nBlocks; b++) {
      size[b] = short_ + (b >= nBlocks - rest ? 1 : 0);
      start[b] = off;
      off += size[b];
      if (size[b] > maxLen) maxLen = size[b];
      rs(bits + start[b], size[b], nEc, ec[b]);
    }
    off = 0;
    for (i = 0; i < maxLen; i++)
      for (b = 0; b < nBlocks; b++)
        if (i < size[b]) blocks[off++] = bits[start[b] + i];
    for (i = 0; i < nEc; i++)
      for (b = 0; b < nBlocks; b++)
        blocks[off++] = ec[b][i];
    memcpy(output, blocks, (size_t)off);
  }

  // --- matriz
  q->side = 17 + 4 * version;
  defaults(q, version);

  // Zigue-zague de baixo para cima, em pares de colunas, pulando a coluna 6
  // (padrao de tempo vertical).
  { int bit = 0, total = SEE[version].total * 8;
    int col, line, up = 1;
    for (col = q->side - 1; col > 0; col -= 2) {
      if (col == 6) col--;
      for (i = 0; i < q->side; i++) {
        line = up ? q->side - 1 - i : i;
        for (j = 0; j < 2; j++) {
          int x = col - j;
          if (ehFunc(q, x, line)) continue;
          if (bit < total) {
            por(q, x, line, (output[bit >> 3] >> (7 - (bit & 7))) & 1);
            bit++;
          }
        }
      }
      up = !up;
    }
  }

  // --- mascara: aplica as 8, pontua, fica com a melhor
  { int best = -1, bestP = 0;
    unsigned char base[QR_MAX_SIDE * QR_MAX_SIDE];
    memcpy(base, q->m, sizeof base);
    for (i = 0; i < 8; i++) {
      int p, x, y;
      memcpy(q->m, base, sizeof base);
      for (y = 0; y < q->side; y++)
        for (x = 0; x < q->side; x++)
          if (!ehFunc(q, x, y) && mascara(i, x, y))
            q->m[y * q->side + x] ^= 1;
      writeFormat(q, i);
      p = penalty(q);
      if (best < 0 || p < bestP) { best = i; bestP = p; }
    }
    memcpy(q->m, base, sizeof base);
    { int x, y;
      for (y = 0; y < q->side; y++)
        for (x = 0; x < q->side; x++)
          if (!ehFunc(q, x, y) && mascara(best, x, y))
            q->m[y * q->side + x] ^= 1; }
    writeFormat(q, best);
  }
  return 1;
}
