#!/usr/bin/env bash
set -euo pipefail

image=${1:?usage: test-container-image.sh IMAGE [EVIDENCE.json]}
evidence=${2:-}
container="pacificdb-image-smoke-$$"

cleanup() {
  docker rm -fv "$container" >/dev/null 2>&1 || true
}
trap cleanup EXIT

test "$(docker image inspect "$image" --format '{{.Config.User}}')" = 10001:10001
test "$(docker image inspect "$image" --format '{{index .Config.Labels "org.opencontainers.image.licenses"}}')" = AGPL-3.0-only
test -n "$(docker image inspect "$image" --format '{{index .Config.Labels "org.opencontainers.image.version"}}')"
test -n "$(docker image inspect "$image" --format '{{index .Config.Labels "org.opencontainers.image.revision"}}')"

docker run --detach --name "$container" --read-only \
  --tmpfs /tmp:uid=10001,gid=10001,mode=0700 \
  --tmpfs /var/log/pacificdb:uid=10001,gid=10001,mode=0750 \
  --mount type=volume,destination=/var/lib/pacificdb/data \
  --mount type=volume,destination=/var/lib/pacificdb/backup \
  --mount type=volume,destination=/var/lib/pacificdb/restore \
  -e PACIFICDB_ENVIRONMENT=development \
  -e ENGINE_AUTH_REQUIRED=0 -e ENGINE_BIND_HOST=0.0.0.0 \
  -e RAFT_BIND_HOST=0.0.0.0 -e RAFT_IS_LEADER=1 -e MIN_QUORUM_SIZE=1 \
  "$image" >/dev/null

for _ in {1..60}; do
  health=$(docker inspect "$container" --format '{{if .State.Health}}{{.State.Health.Status}}{{end}}')
  if [[ "$health" == healthy ]]; then break; fi
  if [[ "$health" == unhealthy || "$(docker inspect "$container" --format '{{.State.Running}}')" != true ]]; then
    docker logs "$container" >&2
    exit 1
  fi
  sleep 1
done
test "$(docker inspect "$container" --format '{{.State.Health.Status}}')" = healthy
docker exec "$container" /usr/local/bin/pacificdb --no-start --port 9000 request \
  '{"action":"createDatabase","dbName":"container_smoke"}' | grep -q '"status": "ok"'
docker exec "$container" /usr/local/bin/pacificdb --no-start --port 9000 request \
  '{"action":"listDatabases"}' | grep -q 'container_smoke'

if [[ -n "$evidence" ]]; then
  image_id=$(docker image inspect "$image" --format '{{.Id}}')
  version=$(docker image inspect "$image" --format '{{index .Config.Labels "org.opencontainers.image.version"}}')
  revision=$(docker image inspect "$image" --format '{{index .Config.Labels "org.opencontainers.image.revision"}}')
  registry_digest=""
  if [[ "$image" == *@sha256:* ]]; then registry_digest=${image#*@}; fi
  jq -n --arg image "$image" --arg image_id "$image_id" \
    --arg version "$version" --arg revision "$revision" \
    --arg registry_digest "$registry_digest" \
    '{status:"PASS", image:$image, image_id:$image_id, version:$version,
      revision:$revision, registry_digest:$registry_digest,
      non_root:true, read_only_root:true,
      application_ping:true, write_read:true}' >"$evidence"
fi

printf 'CONTAINER_IMAGE_SMOKE_PASS\n'
