FROM mcr.microsoft.com/devcontainers/cpp:1-debian-12

RUN if [ "$(uname -m)" = "aarch64" ]; then \
        dpkg --add-architecture amd64 \
        && apt-get update \
        && apt-get -y -install --no-install-recommends x86_64-linux-gnu-g++ libssl-dev:amd64 \
        fi
