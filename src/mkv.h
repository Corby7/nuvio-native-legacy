// Leitura do CABECALHO de um Matroska, so para descobrir o idioma das faixas.
//
// POR QUE ISTO EXISTE. O pipeline da LG devolve, no sourceInfo, o idioma de
// cada faixa de AUDIO ("en", "es", "fr", "it") e NENHUM idioma de legenda:
// medido num arquivo do dono com 43 legendas, todas com
// "language":"(null)", e os unicos campos do subtitleTrackInfo sao trackNum,
// language, type e periodStart. Nao ha outro campo para ler — a informacao
// simplesmente nao sai do pipeline.
//
// O app web mostra os idiomas porque o NAVEGADOR demuxa o arquivo por conta
// propria e expoe textTracks. Este modulo faz a mesma coisa em pequeno: baixa
// os primeiros megabytes por Range e le o elemento Tracks do EBML.
//
// NAO E UM DEMUXER. Nao decodifica nada, nao segue Cues, nao le Clusters. Anda
// pela arvore de elementos ate Segment > Tracks e para. Qualquer coisa que nao
// case com o esperado faz a leitura desistir em silencio — o chamador continua
// com "Legenda N", que e o que havia antes.
#ifndef NV_MKV_H
#define NV_MKV_H

#define MKV_MAX_TRACKS 64

typedef struct {
  int  number;        // TrackNumber, o mesmo `trackNum` do sourceInfo da LG
  int  kind;          // 1 video, 2 audio, 17 legenda (TrackType do Matroska)
  char language[8];     // "por", "eng"... vazio quando o arquivo nao etiqueta
  char name[48];      // Name, quando existe ("Forced", "SDH", "Full")
  char codec[24];     // CodecID ("S_TEXT/UTF8", "S_HDMV/PGS")
} MkvTrack;

// Le o cabecalho de `url` e preenche `saida`. Devolve quantas faixas achou, 0
// quando nao deu (nao e MKV, servidor sem Range, cabecalho maior que o trecho).
// BLOQUEIA: chamar de um fio proprio.
int mkv_tracks(const char *url, MkvTrack *output, int max);

#endif
