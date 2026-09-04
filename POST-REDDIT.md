# Rascunho de post — NAO PUBLICADO

Escrito para você revisar e postar. Eu não publico nada.

**Antes de postar, decida duas coisas:**

1. **Atribuição.** O texto abaixo diz "port nativo do Nuvio". Se o Nuvio não é
   seu, deixe isso explícito e credite quem é. Se é, troque para primeira pessoa.
2. **Onde.** r/webos e r/LGOLED são os candidatos óbvios; cada um tem regra
   própria sobre link de download e autopromoção. Leia as regras da barra
   lateral antes — post removido por regra não volta.

Além disso: **nada aqui foi testado em TV sem root.** O texto já diz isso. Não
tire essa frase — é ela que evita que alguém instale, o vídeo dê tela preta, e
você vire o cara que prometeu.

## Onde os links entram, e por que não no corpo

Um post anterior no r/Nuvio caiu por **"Reddit's filters"** — antispam do SITE,
não moderador (a mensagem é diferente de "removed by moderators"). Causa
provável: idade/karma da conta, ou links demais no corpo.

Por isso: **corpo sem link nenhum.** Estes vão no PRIMEIRO COMENTÁRIO:

- Código: https://github.com/iqui27/nuvio-native-legacy
- Download (.ipk, 175 MB): https://github.com/iqui27/nuvio-native-legacy/releases/tag/v1.0.1

Se cair mesmo assim, **modmail** pedindo aprovação — o post fica na fila de
filtrados, não some.

## O vídeo

`~/Desktop/nuvio-demo-1080p60.mp4` — 2 min, 1920x1080, **7194 quadros em
119,98 s = 59,96 fps medidos**. Sobe como vídeo nativo do Reddit; eles
re-codificam, então não conte com os 60fps sobreviverem ao player deles — é por
isso que o número está escrito no corpo do post.

E ele mostra **só a interface**: o build do Mac não tem pipeline de vídeo, e na
TV nenhuma captura por software fotografa o vídeo (plano de hardware). Se
alguém pedir prova de reprodução, só com captura HDMI.

---

## Título (escolha um)

- Native C/SDL2 port of a streaming app for webOS — 60fps on a 2019 C9
- I ported a webOS streaming app from JS to native C — 60fps on a 5-year-old OLED
- webOS native app in C: 22k lines, 60fps, QR login. Notes from the port.

---

## Corpo

Over the past weeks I ported a webOS streaming app from its JavaScript/Chromium
build to a native C app on SDL2 + GLES2. It runs on a 2019 LG C9 (webOS 4.10).

**Where it landed:** 60.0 fps, 0 janks on the home screen, worst frame 18-19 ms.
Video plays through the TV's own pipeline (LS2 → `com.webos.media` + libAcbAPI),
not through a browser.

The clip is a screen recording of the same code running on macOS, so it shows
the UI only — 7194 frames in 119.98 s, 59.96 fps measured. Reddit re-encodes
video, so the number is here rather than left for you to count.

**What it does now**

- Log in to your account with a QR code on the TV, session survives reboots
- Multiple profiles, with the PIN checked server-side
- Addons, layout settings, TMDB key and watch progress all come from the account
- Trakt linked from the TV itself, through Trakt's device-code flow
- Continue Watching, library, search, settings

**Things I learned the hard way, in case they save someone time**

- The TV's video lives on a *hardware plane behind* the GL surface, revealed
  through an alpha hole. `glReadPixels` and the TV's own capture service never
  see it — a screenshot during playback comes out fully black. That's the model,
  not a bug, and it costs an afternoon if you don't know.
- `StarfishMediaAPIs` calls `exit(0)` when the process doesn't match the
  `exeName` in its LS2 role. Not a crash — atexit runs and the journal stays
  silent. Talking to `com.webos.media` over LS2 directly is what actually works.
- When SAM launches a native app, stdout and stderr go to `/dev/null`. Every
  printf is discarded until you `freopen` a log file.
- The Back button never arrives as a key. It shows up as FOCUS_LOST →
  FOCUS_GAINED within a few ms, because the compositor swallows it. Real exit
  (Home) is FOCUS_LOST with no return, and that's what separates the two.
- Don't link libcurl. The SDK ships `.so.4`, the TV only has `.so.5`, and the
  binary won't even start. `dlopen` at runtime, trying both.
- A `.ipk` is a Debian package. `tar tzf pkg.ipk` lists exactly three names
  without error and never the app files — so a "did my secret leak?" check
  written that way passes every time, including when it did.

**Honest limits**

- Only tested on a rooted C9. It *should* install through Developer Mode or the
  Homebrew Channel with no root, but the app needs LS2 access to
  `com.webos.media`, and I have not verified that a non-rooted install grants
  it. If it doesn't, expect the UI to run and video to be a black screen. If
  anyone tries it on a non-rooted TV I would genuinely like to know.
- The package is 172 MB, most of it prebaked artwork I still need to strip.
- Developer Mode sessions expire every 50 hours. LG's limit, not mine.

Happy to answer anything about the webOS side — the pipeline, the LS2 roles, or
the SDL/GLES setup on a TV this old.

---

## Se pedirem provas ou imagens

Anexe capturas reais, não montagem. As que já existem nesta sessão: a tela de
login com QR, a escolha de perfil, a home com Continuar Assistindo, e Ajustes →
Conta.

**Cuidado óbvio, e vale repetir:** as capturas da home mostram o SEU acervo,
os nomes dos SEUS perfis e as atividades dos SEUS amigos no Trakt. Recorte ou
troque de perfil antes de postar.
