# Toolchain de compilacao cruzada para o app nativo (webOS 4 / ARMv7).
#
# Esta receita existe porque a imagem foi montada a mao em 30/08 e se perdeu:
# sem Dockerfile versionado nao havia como reconstruir o ambiente, e sem
# binario ARM nao ha o que instalar na TV. Nao apague.
#
#   docker build --platform linux/arm64 -t nuvio-webos-sdk tools/
#
# O SDK e o buildroot da openlgtv. `relocate-sdk.sh` precisa rodar uma vez
# depois de extrair: o buildroot grava caminhos absolutos do host onde foi
# gerado, e sem relocar o gcc nao acha o proprio sysroot.
FROM debian:bookworm-slim

ARG SDK_URL=https://github.com/openlgtv/buildroot-nc4/releases/download/webos-a38c582/arm-webos-linux-gnueabi_sdk-buildroot-aarch64.tar.gz

RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates curl file xz-utils \
  && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "$SDK_URL" -o /tmp/sdk.tar.gz \
  && mkdir -p /opt \
  && tar -xzf /tmp/sdk.tar.gz -C /opt \
  && rm /tmp/sdk.tar.gz \
  && /opt/arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh

ENV PATH=/opt/arm-webos-linux-gnueabi_sdk-buildroot/bin:$PATH
ENV NUVIO_SYSROOT=/opt/arm-webos-linux-gnueabi_sdk-buildroot/arm-webos-linux-gnueabi/sysroot

WORKDIR /work
