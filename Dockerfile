# =============================================================================
# DAWN Multi-Stage Dockerfile (Multi-Arch: amd64 + arm64)
# =============================================================================
# Stage 1: dawn-deps    — apt packages + from-source dependency builds (cached)
# Stage 2: dawn-builder — compile DAWN server binary
# Stage 3: dawn-runtime — minimal deployment image
#
# Usage (native build):
#   docker build -t dawn .
#
# Usage (cross-platform via buildx):
#   docker buildx build --platform linux/amd64,linux/arm64 -t dawn .
#
# Running:
#   docker run -v /path/to/models:/var/lib/dawn/models \
#              -v /path/to/dawn.toml:/var/lib/dawn/dawn.toml:ro \
#              -p 3000:3000 dawn
#
# For --read-only deployments:
#   docker run --read-only --tmpfs /tmp \
#              -v /path/to/data:/var/lib/dawn ...
#
# Note: This image uses CPU-only ONNX Runtime on both architectures.
# For GPU-accelerated Whisper ASR, use the CUDA variant instead:
#   docker build -f Dockerfile.cuda -t dawn:cuda .
# =============================================================================

# -----------------------------------------------------------------------------
# Stage 1: Dependencies
# -----------------------------------------------------------------------------
FROM debian:bookworm-slim AS dawn-deps

ENV DEBIAN_FRONTEND=noninteractive

# Build tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake build-essential pkg-config git wget ca-certificates \
    autotools-dev automake autoconf libtool \
    # Core required
    libcurl4-openssl-dev libjson-c-dev libssl-dev libsqlite3-dev \
    libsodium-dev libspdlog-dev libwebsockets-dev libopus-dev \
    libmosquitto-dev libflac-dev libsamplerate0-dev uuid-dev libncurses-dev \
    libasound2-dev libpulse-dev libsndfile1-dev \
    # Optional features (full-feature image)
    libmpg123-dev libvorbis-dev \
    libmupdf-dev libfreetype-dev libharfbuzz-dev libzip-dev libxml2-dev \
    libmujs-dev libgumbo-dev libopenjp2-7-dev libjbig2dec0-dev libjpeg-dev \
    libical-dev \
    # BM25 keyword retrieval — Porter2 stemming (v48)
    libstemmer-dev \
    && rm -rf /var/lib/apt/lists/*

# --- ONNX Runtime (pre-built, arch-specific) ---
# amd64: v1.22.0, arm64: v1.19.2 — arm64 uses older version because
# v1.22.0 has no pre-built aarch64 tarball. Matches scripts/lib/libs.sh.
ARG TARGETARCH
ARG ONNX_VERSION_AMD64=1.22.0
ARG ONNX_SHA256_AMD64=8344d55f93d5bc5021ce342db50f62079daf39aaafb5d311a451846228be49b3
ARG ONNX_VERSION_ARM64=1.19.2
ARG ONNX_SHA256_ARM64=5e30145277d6d6fcb0e8f14f0d0ab5048af7b13ffd608023bb1e2875621fab07
RUN set -e; \
    if [ "$TARGETARCH" = "arm64" ]; then \
        ONNX_VERSION="${ONNX_VERSION_ARM64}"; \
        ONNX_SHA256="${ONNX_SHA256_ARM64}"; \
        ONNX_ARCH="aarch64"; \
    else \
        ONNX_VERSION="${ONNX_VERSION_AMD64}"; \
        ONNX_SHA256="${ONNX_SHA256_AMD64}"; \
        ONNX_ARCH="x64"; \
    fi; \
    ONNX_TARBALL="onnxruntime-linux-${ONNX_ARCH}-${ONNX_VERSION}.tgz"; \
    ONNX_DIR="onnxruntime-linux-${ONNX_ARCH}-${ONNX_VERSION}"; \
    cd /tmp \
    && wget -q "https://github.com/microsoft/onnxruntime/releases/download/v${ONNX_VERSION}/${ONNX_TARBALL}" \
    && echo "${ONNX_SHA256}  ${ONNX_TARBALL}" | sha256sum -c \
    && tar xzf "${ONNX_TARBALL}" \
    && cp -a "${ONNX_DIR}/lib/"* /usr/local/lib/ \
    && cp -r "${ONNX_DIR}/include/"* /usr/local/include/ \
    && ldconfig \
    && rm -rf /tmp/onnxruntime-*

# --- espeak-ng (rhasspy fork, pinned commit) ---
ARG ESPEAK_COMMIT=8593723f10cfd9befd50de447f14bf0a9d2a14a4
RUN cd /tmp \
    && git init espeak-ng && cd espeak-ng \
    && git fetch --depth 1 https://github.com/rhasspy/espeak-ng.git ${ESPEAK_COMMIT} \
    && git checkout FETCH_HEAD \
    && ./autogen.sh \
    && ./configure --prefix=/usr/local \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig \
    && rm -rf /tmp/espeak-ng

# --- piper-phonemize (pinned commit) ---
ARG PIPER_PHONEMIZE_COMMIT=ba3cc06c5248215928821f1393b2b854a936991a
RUN cd /tmp \
    && git init piper-phonemize && cd piper-phonemize \
    && git fetch --depth 1 https://github.com/rhasspy/piper-phonemize.git ${PIPER_PHONEMIZE_COMMIT} \
    && git checkout FETCH_HEAD \
    && mkdir build && cd build \
    && cmake .. -DONNXRUNTIME_DIR=/usr/local -DESPEAK_NG_DIR=/usr/local \
    && make -j"$(nproc)" \
    && cp -a libpiper_phonemize.so* /usr/local/lib/ \
    && mkdir -p /usr/local/include/piper-phonemize \
    && cp ../src/*.hpp /usr/local/include/piper-phonemize/ \
    && cp ../src/uni_algo.h /usr/local/include/piper-phonemize/ \
    && ldconfig \
    && rm -rf /tmp/piper-phonemize

# -----------------------------------------------------------------------------
# Stage 2: Build DAWN
# -----------------------------------------------------------------------------
FROM dawn-deps AS dawn-builder

COPY . /src
WORKDIR /src

# whisper.cpp submodule must be present in the build context.
# When building via CI, use: git checkout --recurse-submodules
# When building locally: git submodule update --init whisper.cpp

# Git SHA stamped into the binary's version string. The build context excludes
# .git (see .dockerignore), so CMake can't derive it here — pass it in with
# --build-arg GIT_SHA=$(git rev-parse --short HEAD). Defaults to "unknown".
ARG GIT_SHA=unknown

RUN cmake --preset server -DGIT_SHA="${GIT_SHA}" \
    && make -C build-server -j"$(nproc)"

RUN cmake --install build-server --prefix /opt/dawn

# -----------------------------------------------------------------------------
# Stage 3: Runtime
# -----------------------------------------------------------------------------
FROM debian:bookworm-slim AS dawn-runtime

ENV DEBIAN_FRONTEND=noninteractive

# Non-root user
RUN groupadd -r dawn && useradd -r -g dawn -d /var/lib/dawn -s /sbin/nologin dawn

# Runtime libraries only (no -dev packages)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 libjson-c5 libssl3 libsqlite3-0 \
    libsodium23 libsndfile1 libspdlog1.10 libfmt9 libwebsockets17 libopus0 \
    libmosquitto1 libflac12 libsamplerate0 libuuid1 libncurses6 \
    libasound2 libpulse0 \
    libmpg123-0 libvorbis0a libvorbisfile3 \
    # MuPDF is statically linked; these are its transitive shared deps + other link deps
    libfreetype6 libharfbuzz0b libzip4 libxml2 \
    libgumbo1 libopenjp2-7 libjbig2dec0 libmujs2 libjpeg62-turbo \
    libgomp1 \
    libical3 \
    # BM25 Porter2 stemming runtime (v48)
    libstemmer0d \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# From-source libraries (not available via apt)
COPY --from=dawn-deps /usr/local/lib/libonnxruntime* /usr/local/lib/
COPY --from=dawn-deps /usr/local/lib/libpiper_phonemize* /usr/local/lib/
COPY --from=dawn-deps /usr/local/lib/libespeak-ng* /usr/local/lib/
COPY --from=dawn-deps /usr/local/share/espeak-ng-data /usr/local/share/espeak-ng-data
RUN ldconfig

# Application binary, admin tool, and shared libraries (whisper, ggml)
COPY --from=dawn-builder /opt/dawn /opt/dawn
RUN echo "/opt/dawn/lib" > /etc/ld.so.conf.d/dawn.conf && ldconfig

# WebUI static files
COPY www/ /opt/dawn/www/

# Tool instruction files (needed by instruction_loader at runtime)
COPY tool_instructions/ /opt/dawn/tool_instructions/

# Create data directory with correct ownership
RUN mkdir -p /var/lib/dawn/models /var/lib/dawn/db \
    && chown -R dawn:dawn /var/lib/dawn

# Add binaries to PATH
ENV PATH="/opt/dawn/bin:${PATH}"

EXPOSE 3000
VOLUME /var/lib/dawn
USER dawn

HEALTHCHECK --interval=30s --timeout=5s \
    CMD /opt/dawn/bin/dawn-admin ping || exit 1

ENTRYPOINT ["/opt/dawn/bin/dawn"]
