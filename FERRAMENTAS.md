# Ferramentas de desenvolvimento — app nativo

Três coisas que não existiam e sem as quais era impossível trabalhar sozinho
nesta TV. Todas nasceram de um obstáculo concreto, não de gosto.

## 1. Log em arquivo

Quando o SAM lança o app, `/proc/<pid>/fd/1` e `fd/2` apontam para
`/dev/null`. **Todo `printf` era descartado** — FPS, erros de shader, tudo.
Por isso `main.c` faz `freopen("/tmp/nuvio.log", ...)` logo no início.

```bash
(sleep 3; printf 'grep -a FPS /tmp/nuvio.log | tail -5\n'; sleep 3; printf 'exit\n') \
  | nc -w25 192.168.1.32 23 | tr -d '\0'
```

## 2. Captura de tela

O framebuffer não pode ser lido nem como root (`/dev/fb0` →
"Operation not permitted") e `com.webos.service.capture` responde erro. A
única saída é o app se fotografar: `glReadPixels` + BMP escrito à mão.

- BMP e não PNG porque o SDL2_image da TV **não tem escrita** (gravava 0 byte).
- BMP escrito à mão e não `SDL_SaveBMP` porque este converte pixel a pixel
  quando as máscaras não batem, e nessa CPU leva segundos — o arquivo era lido
  pela metade.
- Grava em temporário e renomeia, para nunca existir arquivo parcial.

```bash
bash /tmp/shot.sh 6          # build + deploy + captura + baixa + converte
```

## 3. Injeção de teclas

Permite abrir o detalhe e navegar sem ninguém no sofá com o controle.
Escreva teclas (`up`,`down`,`left`,`right`,`ok`,`back`) em `/tmp/nuvio-key`.

```bash
bash /tmp/grab.sh nome_do_arquivo down down
```

### A armadilha do sticky bit

`/tmp` é `drwxrwxrwt` e os arquivos criados pelo shell pertencem ao **root**,
enquanto o app roda como uid 5410. Ou seja: **o app não consegue apagá-los.**
Enquanto isso não foi visto, cada pedido era reprocessado a cada quadro — uma
única tecla `down` virava centenas e o foco corria até o fim da página.

Por isso os arquivos são **consumidos truncando** (`fopen("w")`), nunca
removendo, e um pedido só vale enquanto tiver conteúdo. Quem escreve precisa
dar `chmod 666` para que o app possa truncar.

## Tecla Back

O Back do controle **não chega ao app como tecla**. Medido logando todos os
eventos SDL: setas e Enter chegam; o Back produz apenas
`FOCUS_LOST` → `FOCUS_GAINED` em poucos milissegundos — o compositor engole a
tecla, tira o foco para tentar fechar o app e devolve. `main.c` trata esse par
dentro de 600 ms como Back. Sair de verdade (tecla Home) produz `FOCUS_LOST`
sem retorno, e é isso que separa os dois casos.

Não há documentação pública sobre entregar o Back a um app nativo no webOS. Se
aparecer caminho melhor (luna-service2, `webos_shell_surface`), trocar.
