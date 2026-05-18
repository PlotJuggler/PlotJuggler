FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build ccache git ca-certificates \
        wget curl lsb-release file fuse libfuse2 patchelf \
        software-properties-common pkg-config \
        qtbase5-dev libqt5svg5-dev libqt5websockets5-dev libqt5opengl5-dev \
        libqt5x11extras5-dev libqt5serialport5-dev qttools5-dev qttools5-dev-tools \
        libprotoc-dev protobuf-compiler libzmq3-dev liblz4-dev libzstd-dev \
        libmosquitto-dev libcurl4-openssl-dev \
        python3 python3-dev python3-pip \
    && rm -rf /var/lib/apt/lists/*

RUN wget -q https://packages.apache.org/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-jammy.deb \
    && apt-get update && apt-get install -y --no-install-recommends ./apache-arrow-apt-source-latest-jammy.deb \
    && apt-get update && apt-get install -y --no-install-recommends \
        libarrow-dev libarrow-flight-dev libparquet-dev \
    && rm -rf /var/lib/apt/lists/* apache-arrow-apt-source-latest-jammy.deb

WORKDIR /work
