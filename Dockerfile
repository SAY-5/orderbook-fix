# syntax=docker/dockerfile:1
FROM debian:stable-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ca-certificates git && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

FROM debian:stable-slim
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/obfix_server /usr/local/bin/obfix_server
EXPOSE 9876
USER nobody
ENTRYPOINT ["/usr/local/bin/obfix_server", "--host", "0.0.0.0", "--port", "9876"]
