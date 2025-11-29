FROM alpine:3.22.2

ARG ZIG_VERSION=0.15.2
ARG ZIG_ARCHIVE="zig-x86_64-linux-${ZIG_VERSION}.tar.xz"
ARG ZIG_URL="https://ziglang.org/download/${ZIG_VERSION}/${ZIG_ARCHIVE}"

ARG ZIG_HOME="/usr/local/zig"
ENV PATH="${ZIG_HOME}:${PATH}"

RUN apk add --no-cache cmake ninja wget tar xz bash git build-base

RUN wget "${ZIG_URL}"
RUN tar -xf "${ZIG_ARCHIVE}" -C /usr/local/
RUN mv "/usr/local/zig-x86_64-linux-${ZIG_VERSION}" /usr/local/zig
RUN rm "${ZIG_ARCHIVE}"

ENV CXX="zig c++ -s -O3 -target x86_64-linux-musl"
ENV CC="zig cc -s -O3 -target x86_64-linux-musl"

CMD ["bash"]
