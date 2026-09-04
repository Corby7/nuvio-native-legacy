// Reproducao de video de verdade nesta TV (webOS 4.10), por LS2 direto.
//
// O modulo NAO desenha nada. O video vive num PLANO DE HARDWARE separado, atras
// da superficie GL do app; o que o app faz e abrir um buraco transparente por
// onde esse plano aparece (ver gfx_furo). Consequencia que economiza horas:
// glReadPixels e o servico de captura da TV NUNCA vao fotografar o video. Isso
// e o modelo, nao defeito — verificar reproducao se faz pelo estado, ou pelo
// access log de quem serve o arquivo.
//
// Por que LS2 direto e nao StarfishMediaAPIs, tudo medido no aparelho:
// o construtor da StarfishMediaAPIs chama exit(0) quando o processo nao casa com
// o "exeName" do papel LS2 (nao e crash: atexit dispara e o journal fica mudo),
// e mesmo com o papel certo ele nunca chega a falar com com.webos.media aqui —
// responde erro 202 "Media Not Found", que e string interna da propria lib.
// A sequencia abaixo, ao contrario, saiu de captura ls-monitor do navegador da
// TV tocando o mesmo arquivo: e o que comprovadamente funciona.
#ifndef NV_VIDEO_H
#define NV_VIDEO_H

// Registra no barramento e sobe o laco de eventos. 1 se deu certo.
// Falhar aqui nao e fatal: o app segue sem video.
int  video_start(void);

// Comeca a tocar. `url` e http(s):// ou file://. NAO mandar mediaTransportType:
// o transporte sai do prefixo da URL, e mandar o campo faz o load aceitar,
// devolver mediaId e nunca buscar o arquivo — falha silenciosa.
int  video_play(const char *url);

// Chamar UMA VEZ POR QUADRO. Hoje serve ao prazo do recuo de Dolby Vision
// (ver o comentario em video.c): sem esta batida, um arquivo que a TV recusa
// com DolbyHdrInfo ficaria sem imagem ate o usuario desistir e sair.
void video_pump(void);
void video_stop(void);
void video_pause(int paused);
void video_fetch(double seconds);

// Retangulo do plano de video, em coordenadas de tela 1920x1080. Fica preso a
// tela: pedir origem negativa ou tamanho maior que o painel APAGA o plano — um
// plano de hardware nao recorta o excedente. Para ampliar, use a funcao abaixo.
void video_window(int x, int y, int w, int h);

// Zoom de verdade: recorta a FONTE (coordenadas do quadro decodificado, ver
// video_largura/video_altura) e desenha no destino (coordenadas de tela). Pedir
// um pedaco menor da fonte para o mesmo destino e o que amplia a imagem, e o
// que tira da vista a barra preta embutida no quadro.
void video_window_source(int sx, int sy, int sw, int sh,
                        int dx, int dy, int dw, int dh);

double video_pos(void);      // segundos decorridos
double video_duration(void);  // 0 enquanto desconhecida
double video_buffer_end(void); // ate onde o buffer cobre (s); 0 se desconhecido
// Afirmacao de Dolby Vision da FONTE escolhida (o addon descreve o arquivo).
// Chamar ANTES de video_tocar/definir_fonte: e o que decide o hdrType que o
// ACB descreve ao tv.display.
void video_set_dv(int dv);

// A fonte e MP4? Chamar ANTES de video_tocar, junto com video_definir_dv.
//
// Serve para NAO sondar o cabecalho Matroska num arquivo que nunca vai ter um.
// Essa sonda baixa o inicio do arquivo pela MESMA conexao que esta
// transmitindo, e num MP4 ela e trabalho garantidamente perdido — o proprio
// log dizia "nenhuma faixa lida" toda vez.
void video_set_mp4(int ehMp4);
int    video_playing(void);
int    video_ready(void);   // 1 depois do loadCompleted
int    video_active(void);    // 1 assim que ha mediaId — e o que abre o furo

// --- faixas -----------------------------------------------------------------
// Tudo isto sai do evento sourceInfo da assinatura do uMS: o addon nao informa
// nada disso, e so o pipeline sabe o que ha DENTRO do arquivo.

#define NV_TRACK_MAX 12

typedef struct {
  char label[48];   // "Ingles · Atmos 5.1" ou "Legenda 3"
  char language[8];    // "en"; vazio quando o arquivo nao etiqueta
  int  number;       // indice que o selectTrack espera
} VideoTrack;

int  video_n_audio(void);
int  video_n_subtitle(void);
const VideoTrack *video_audio(int i);
const VideoTrack *video_subtitle(int i);
int  video_audio_current(void);
int  video_subtitle_current(void);   // -1 = desligada

void video_choose_audio(int i);
void video_choose_subtitle(int i);   // -1 desliga

// Legenda de arquivo externo (OpenSubtitles). O uMS baixa e sincroniza
// sozinho; o app so passa a URL.
void video_subtitle_external(const char *url);
// O uMS identifica o formato pela extensao da URI. Addons costumam entregar
// /file/123 sem .srt; esta funcao torna a URI reconhecivel sem mudar o arquivo.
void video_normalize_url_subtitle(const char *url, char *dst, unsigned size);

// --- ESTILO DA LEGENDA -------------------------------------------------------
//
// PROVADO NO APARELHO (LG C9, webOS 4.10, 2026-09-02) com um filme tocando: os
// cinco metodos abaixo mudam a legenda na tela de verdade. O teste foi visual e
// nao pelo retorno, porque o uMS responde `returnValue:true` PARA QUALQUER
// COISA — ele aceitou ate valores que eu inventei para `charEdgeType`. Nesta
// API o codigo de retorno nao e evidencia de nada.
//
// A legenda e desenhada pelo PIPELINE, abaixo da superficie GL: ela nao aparece
// em glReadPixels, igual ao video. Verificar mudanca aqui exige olhar a tela.
//
// Os valores sao os indices das opcoes oferecidas na folha, nao os do uMS: a
// traducao para o vocabulario do aparelho mora em video.c, que e quem conhece o
// pipeline.
typedef struct {
  int size;    // 50..200%, passo 10 (120 = padrao)
  int color;        // indice em VIDEO_LEG_CORES
  int background;      // 0 nenhum; 1..4 = escuro 25/50/75/100%
  int position;    // 0..7  -> position -3..4 no uMS
  int border;      // 0 nenhuma, 1 contorno, 2 sombra
  int delayMs;   // negativo adianta
  int opacity;  // 0..3 = texto 100/75/50/25%
  int family;    // TxtFamilia; aplicada ao overlay externo (OpenSubtitles)
} VideoSubtitleStyle;

#define VIDEO_SUB_NCORES 6
// Nomes que o uMS aceita em charColor. Expostos porque a folha desenha os
// rotulos e precisa da mesma ordem.
extern const char *const VIDEO_SUB_COLORS[VIDEO_SUB_NCORES];
extern const char *const VIDEO_SUB_COLORS_PT[VIDEO_SUB_NCORES];

// Aplica agora, se houver sessao. O estilo fica GUARDADO e e reaplicado a cada
// load: o pipeline nasce de novo a cada video e nao carrega o ajuste anterior.
void video_subtitle_style(const VideoSubtitleStyle *e);

// Verdade sobre o fluxo, para os selos da tela nao mentirem.
int  video_tem_atmos(void);
// 1 so quando o hdrType do PIPELINE diz DolbyVision. A afirmacao da fonte
// (video_definir_dv) nao entra aqui de proposito: ver o comentario em video.c.
int  video_tem_dolby_vision(void);
const char *video_hdr(void);   // hdrType cru: "none", "HDR10", "DolbyVision"...
// Dimensoes do QUADRO decodificado, do videoInfo. Sao elas que dao a proporcao
// usada pelos modos de zoom do player.
int  video_width(void);
int  video_height(void);

void video_shutdown(void);

#endif
