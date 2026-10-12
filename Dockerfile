# PS2BUILD currently supports glibc-based Linux distributions, not Alpine/musl.
FROM ubuntu:24.04

ARG NET_TEST_HOST=192.168.1.2
ARG NET_TEST_PORT=7777
ARG LOGIN_HOST=192.168.1.2

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        bash \
        ca-certificates \
        curl \
        git \
        make \
    && rm -rf /var/lib/apt/lists/*

# Install the official PS2BUILD SDK and bootstrap its toolchain/packages.
RUN curl -sSfL https://git.techwritescode.dev/ps2/install/raw/branch/master/install.sh | sh

ENV PATH="/root/.local/share/ps2build:${PATH}"

WORKDIR /src
COPY . /src

# The assembly assets use paths relative to the assembler's working directory,
# which is /src/build under PS2BUILD.
RUN mkdir -p build \
    && cp Tibia.dat Tibia.spr build/ \
    && sed -i \
        -e "s|LOGIN_HOST=\\\\\"192.168.1.2\\\\\"|LOGIN_HOST=\\\\\"${LOGIN_HOST}\\\\\"|" \
        -e "s|192.168.1.2|${NET_TEST_HOST}|g" \
        -e "s|NET_TEST_PORT=7777|NET_TEST_PORT=${NET_TEST_PORT}|g" \
        ps2.yaml \
    && ps2build build \
    && ls -lh build/bin/*.elf

CMD ["ps2build", "build"]