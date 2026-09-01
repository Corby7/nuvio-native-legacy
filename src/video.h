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
int  video_iniciar(void);

// Comeca a tocar. `url` e http(s):// ou file://. NAO mandar mediaTransportType:
// o transporte sai do prefixo da URL, e mandar o campo faz o load aceitar,
// devolver mediaId e nunca buscar o arquivo — falha silenciosa.
int  video_tocar(const char *url);

// Chamar UMA VEZ POR QUADRO. Hoje serve ao prazo do recuo de Dolby Vision
// (ver o comentario em video.c): sem esta batida, um arquivo que a TV recusa
// com DolbyHdrInfo ficaria sem imagem ate o usuario desistir e sair.
void video_bombear(void);
void video_parar(void);
void video_pausar(int pausado);
void video_buscar(double segundos);

// Retangulo do plano de video, em coordenadas de tela 1920x1080.
void video_janela(int x, int y, int w, int h);

double video_pos(void);      // segundos decorridos
double video_duracao(void);  // 0 enquanto desconhecida
double video_buffer_fim(void); // ate onde o buffer cobre (s); 0 se desconhecido
// Afirmacao de Dolby Vision da FONTE escolhida (o addon descreve o arquivo).
// Chamar ANTES de video_tocar/definir_fonte: e o que decide o hdrType que o
// ACB descreve ao tv.display.
void video_definir_dv(int dv);
int    video_tocando(void);
int    video_pronto(void);   // 1 depois do loadCompleted
int    video_ativo(void);    // 1 assim que ha mediaId — e o que abre o furo

// --- faixas -----------------------------------------------------------------
// Tudo isto sai do evento sourceInfo da assinatura do uMS: o addon nao informa
// nada disso, e so o pipeline sabe o que ha DENTRO do arquivo.

#define NV_FAIXA_MAX 12

typedef struct {
  char rotulo[48];   // "Ingles · Atmos 5.1" ou "Legenda 3"
  char idioma[8];    // "en"; vazio quando o arquivo nao etiqueta
  int  numero;       // indice que o selectTrack espera
} VideoFaixa;

int  video_n_audio(void);
int  video_n_legenda(void);
const VideoFaixa *video_audio(int i);
const VideoFaixa *video_legenda(int i);
int  video_audio_atual(void);
int  video_legenda_atual(void);   // -1 = desligada

void video_escolher_audio(int i);
void video_escolher_legenda(int i);   // -1 desliga

// Legenda de arquivo externo (OpenSubtitles). O uMS baixa e sincroniza
// sozinho; o app so passa a URL.
void video_legenda_externa(const char *url);

// Verdade sobre o fluxo, para os selos da tela nao mentirem.
int  video_tem_atmos(void);
int  video_tem_dolby_vision(void);
int  video_largura(void);

void video_encerrar(void);

#endif
