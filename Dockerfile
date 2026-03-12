FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    make \
    dos2unix \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY . .

RUN find . -name '*.sh' -exec dos2unix {} + && \
    chmod +x scripts/build.sh && \
    CROSS_PREFIX=aarch64-linux-gnu- bash scripts/build.sh
