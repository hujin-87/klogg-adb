FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

# Base host tools needed to configure/build and to fetch CPM deps at configure time
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates gnupg2 wget curl git \
        cmake make ragel python3 pkg-config \
        lsb-release software-properties-common \
    && rm -rf /var/lib/apt/lists/*

# Add MXE apt repository (prebuilt Qt5 static cross toolchain) for focal.
# Fetch the signing key over HTTPS (hkp keyserver port is blocked here).
RUN mkdir -p /etc/apt/keyrings \
    && curl -fsSL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x86B72ED9" \
        | gpg --dearmor -o /etc/apt/keyrings/mxeapt.gpg \
    && echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/mxeapt.gpg] https://pkg.mxe.cc/repos/apt focal main" \
        > /etc/apt/sources.list.d/mxeapt.list

# Install the x86-64 static MinGW toolchain + Qt5 + boost (prebuilt, no long compile)
RUN apt-get update && apt-get install -y --no-install-recommends \
        mxe-x86-64-w64-mingw32.static-cc \
        mxe-x86-64-w64-mingw32.static-qtbase \
        mxe-x86-64-w64-mingw32.static-qttools \
        mxe-x86-64-w64-mingw32.static-boost \
    && rm -rf /var/lib/apt/lists/*

ENV PATH="/usr/lib/mxe/usr/bin:${PATH}"
ENV MXE_TARGET=x86-64-w64-mingw32.static

WORKDIR /src
