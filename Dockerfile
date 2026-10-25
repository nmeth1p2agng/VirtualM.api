FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build --parallel 2

FROM debian:bookworm-slim
RUN useradd --system --uid 10001 appuser
WORKDIR /app
COPY --from=build /app/build/virtualm_api /app/virtualm_api
USER appuser
ENV PORT=10000
EXPOSE 10000
CMD ["/app/virtualm_api"]
