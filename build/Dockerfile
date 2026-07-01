FROM emscripten/emsdk:5.0.7 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build pkg-config \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

ARG VERSION=1.9.3
ARG SOURCE_SHA256=d532172d7ab24d2a25944e2434212d63ee85f3650e97b5f7579e7f201a78ad64
ENV VERSION=${VERSION}

WORKDIR /src

ADD https://github.com/libgit2/libgit2/archive/refs/tags/v${VERSION}.tar.gz libgit2.tar.gz
RUN echo "${SOURCE_SHA256}  libgit2.tar.gz" | sha256sum -c - \
    && tar -xzf libgit2.tar.gz && rm libgit2.tar.gz

WORKDIR /src/libgit2-${VERSION}

ENV CFLAGS="-Wno-error=incompatible-pointer-types"
RUN emcmake cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_INSTALL_PREFIX=/out \
      -DCMAKE_C_FLAGS="$CFLAGS" \
      -DBUILD_TESTS=OFF \
      -DBUILD_CLI=OFF \
      -DBUILD_EXAMPLES=OFF \
      -DBUILD_SHARED_LIBS=OFF \
      -DUSE_HTTPS=OFF \
      -DUSE_SSH=OFF \
      -DUSE_HTTP_PARSER=builtin \
      -DUSE_BUNDLED_ZLIB=ON \
      -DUSE_ICONV=OFF \
      -DUSE_THREADS=OFF \
      -DREGEX_BACKEND=builtin \
    && cmake --build build --target install -j"$(nproc)"

RUN test -f /out/lib/libgit2.a \
    && test -f /out/include/git2.h

COPY src /src/cli
COPY js  /src/js

ENV GIT_LDFLAGS="\
    -lnodefs.js -lworkerfs.js \
    --post-js /src/js/post.js \
    -sWASM_BIGINT=1 \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=33554432 \
    -sSTACK_SIZE=5242880 \
    -sEXIT_RUNTIME=1 \
    -sEXPORTED_RUNTIME_METHODS=callMain,FS,NODEFS,WORKERFS,ENV \
    -sINCOMING_MODULE_JS_API=noInitialRun,wasmBinary,locateFile,preRun,postRun,instantiateWasm,print,printErr,arguments \
    -sINVOKE_RUN=0 \
    -sMODULARIZE=1 \
    -sEXPORT_ES6=1 \
"

RUN cd /src/cli \
    && emcc -O2 -I/out/include -c bridge_transport.c -o bridge_transport.o \
    && emcc -O2 -I/out/include -c git_main.c          -o git_main.o \
    && emcc $GIT_LDFLAGS \
         -o git.mjs \
         git_main.o bridge_transport.o /out/lib/libgit2.a

RUN mkdir -p /dist \
    && cp /src/cli/git.mjs  /dist/git.mjs \
    && cp /src/cli/git.wasm /dist/git.wasm \
    && ls -lh /dist \
    && [ "$(head -c4 /dist/git.wasm | od -An -tx1 | tr -d ' \n')" = "0061736d" ] \
    && [ "$(stat -c%s /dist/git.wasm)" -gt 262144 ]

FROM scratch AS exportor
COPY --from=builder /dist /dist
