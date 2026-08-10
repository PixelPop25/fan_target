FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl unzip python3 python3-pil make cmake git \
      clang-18 lld-18 \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL -o /tmp/sdk.zip \
      https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip \
    && unzip -q /tmp/sdk.zip -d /tmp/sdk_extract \
    && SDK_DIR=$(find /tmp/sdk_extract -name prospero.mk | head -1 | xargs dirname | xargs dirname) \
    && mv "$SDK_DIR" /opt/ps5-payload-sdk \
    && rm -rf /tmp/sdk.zip /tmp/sdk_extract
ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
WORKDIR /src
COPY . /src
RUN bash build.sh || (make overlay_elf; make blob; make all)
# Mount host dist to /out when running:
#   docker run --rm -v "$PWD/dist:/out" fan_target_pxp
CMD ["sh", "-c", "mkdir -p /out && cp -a /src/dist/. /out/ && ls -la /out"]
