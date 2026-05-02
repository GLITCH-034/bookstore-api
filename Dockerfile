FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    cmake build-essential \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    libasio-dev \
    wget && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN rm -rf build && mkdir build && cd build && cmake .. && make -j4

EXPOSE 8080
CMD ["./build/bookstore_api"]
