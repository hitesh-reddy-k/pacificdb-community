# syntax=docker/dockerfile:1.7
FROM debian:bookworm-slim AS builder

ARG PACIFICDB_ENGINE_VERSION=dev
ARG PACIFICDB_GIT_COMMIT=unknown
ARG PACIFICDB_BUILD_TIMESTAMP=unknown
ARG PACIFICDB_SOURCE_URL=https://github.com/hitesh-reddy-k/pacificdb-community

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates cmake g++ liblz4-dev libssl-dev make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY engine/ ./engine/
RUN cmake -S engine -B /build \
      -DCMAKE_BUILD_TYPE=Release \
      -DPACIFICDB_ENGINE_VERSION="${PACIFICDB_ENGINE_VERSION}" \
      -DPACIFICDB_GIT_COMMIT="${PACIFICDB_GIT_COMMIT}" \
      -DPACIFICDB_BUILD_TIMESTAMP="${PACIFICDB_BUILD_TIMESTAMP}" \
      -DPACIFICDB_ENABLE_TEST_FAILPOINTS=OFF \
      -DENABLE_ASAN=OFF \
      -DENABLE_TSAN=OFF \
    && cmake --build /build --target db_engine pacificdb -j4 \
    && test -x /build/db_engine \
    && test -x /build/pacificdb \
    && strip /build/db_engine /build/pacificdb

FROM debian:bookworm-slim AS runtime

ARG PACIFICDB_ENGINE_VERSION=dev
ARG PACIFICDB_GIT_COMMIT=unknown
ARG PACIFICDB_BUILD_TIMESTAMP=unknown
ARG PACIFICDB_SOURCE_URL=https://github.com/hitesh-reddy-k/pacificdb-community
LABEL org.opencontainers.image.title="PacificDB Engine" \
      org.opencontainers.image.version="${PACIFICDB_ENGINE_VERSION}" \
      org.opencontainers.image.revision="${PACIFICDB_GIT_COMMIT}" \
      org.opencontainers.image.created="${PACIFICDB_BUILD_TIMESTAMP}" \
      org.opencontainers.image.source="${PACIFICDB_SOURCE_URL}" \
      org.opencontainers.image.licenses="AGPL-3.0-only"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates liblz4-1 libssl3 netcat-openbsd openssl \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system --gid 10001 pacificdb \
    && useradd --system --uid 10001 --gid pacificdb \
       --home-dir /var/lib/pacificdb --shell /usr/sbin/nologin pacificdb \
    && install -d -o pacificdb -g pacificdb -m 0750 \
       /var/lib/pacificdb/data /var/lib/pacificdb/backup /var/lib/pacificdb/restore \
       /var/log/pacificdb

COPY --from=builder --chown=root:root /build/db_engine /build/pacificdb /usr/local/bin/
COPY --chmod=0555 deploy/docker/healthcheck.sh /usr/local/bin/pacificdb-healthcheck
COPY --chown=root:root LICENSE THIRD_PARTY_NOTICES.md /usr/share/licenses/pacificdb/

ENV DATA_ROOT=/var/lib/pacificdb/data \
    BACKUP_ROOT=/var/lib/pacificdb/backup \
    RESTORE_DIR=/var/lib/pacificdb/restore \
    ENGINE_BIND_HOST=0.0.0.0 \
    ENGINE_PORT=9000 \
    RAFT_BIND_HOST=0.0.0.0 \
    RAFT_LISTEN_PORT=9001 \
    ENGINE_AUTH_REQUIRED=1 \
    WAL_FLUSH_ON_COMMIT=1 \
    RAFT_ASYNC_LOG_FSYNC=0 \
    RAFT_CLUSTER_ID=pacificdb-community \
    RAFT_NODE_ID=node-1

VOLUME ["/var/lib/pacificdb/data", "/var/lib/pacificdb/backup", "/var/lib/pacificdb/restore", "/var/log/pacificdb"]
EXPOSE 9000 9001
USER 10001:10001
WORKDIR /var/lib/pacificdb
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=10s --timeout=5s --start-period=20s --retries=5 \
  CMD ["/usr/local/bin/pacificdb-healthcheck", "liveness"]

ENTRYPOINT ["/usr/local/bin/db_engine"]
