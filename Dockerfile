FROM buildpack-deps:buster

# get llvm
RUN apt-get update && \
    apt-get install -y \
    apt-utils \
    gnupg \
    wget \
    lsb-release \ 
    software-properties-common && \
    wget https://apt.llvm.org/llvm.sh && \
    chmod +x llvm.sh && \
    wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - && \
    ./llvm.sh 12 

RUN apt-get install -y cmake

ADD CMakeLists.txt split.cpp /split/

RUN mkdir split-build && \
      cd split-build && \
      cmake /split -DCMAKE_INSTALL_PREFIX=/usr/bin -DCMAKE_BUILD_TYPE=Release && \
      make install
