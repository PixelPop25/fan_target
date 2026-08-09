# Build fan_target_pxp with the official PS5 payload SDK binary release.
#   docker build -t fan_target_pxp .
#   docker run --rm -v "$PWD/dist:/out" fan_target_pxp
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl unzip python3 make cmake git \
      clang-18 lld-18 \
    && rm -rf /var/lib/apt/lists/*

# Binary SDK
RUN curl -fsSL -o /tmp/sdk.zip \
      https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip \
    && unzip -q /tmp/sdk.zip -d /opt \
    && mv /opt/ps5-payload-sdk /opt/ps5-payload-sdk-tmp 2>/dev/null || true \
    && if [ -d /opt/ps5-payload-sdk ]; then true; \
       elif [ -d /opt/ps5-payload-sdk-tmp ]; then mv /opt/ps5-payload-sdk-tmp /opt/ps5-payload-sdk; \
       else dir=$(find /opt -maxdepth 2 -name prospero.mk | head -1 | xargs dirname | xargs dirname); \
            mv "$dir" /opt/ps5-payload-sdk; fi \
    && rm -f /tmp/sdk.zip

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
WORKDIR /src
COPY . /src

RUN bash build.sh || (make overlay_elf && make blob && make all)

CMD ["sh", "-c", "cp -a dist/. /out/ 2>/dev/null || cp -a dist/. /src/dist_out/ || true"]
