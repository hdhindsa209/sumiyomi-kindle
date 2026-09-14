# Kindle cross-build environment: koxtoolchain `kindlehf` prebuilt (Linux x86_64 only).
# On Apple Silicon this runs under Rosetta via Colima (--vz-rosetta).
# The toolchain lives in the image, not on the macOS filesystem: the sysroot has
# headers that differ only by case, which a case-insensitive APFS volume would clobber.
FROM --platform=linux/amd64 debian:bookworm-slim

ARG KOXTC_TAG=2026.08

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl zstd make cmake file git python3 \
 && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/x-tools \
 && curl -fsSL "https://github.com/koreader/koxtoolchain/releases/download/${KOXTC_TAG}/kindlehf.tar.zst" \
    | tar --zstd -x -C /opt

ENV KINDLE_TC_ROOT=/opt/x-tools/arm-kindlehf-linux-gnueabihf
ENV PATH="${KINDLE_TC_ROOT}/bin:${PATH}"

WORKDIR /src
