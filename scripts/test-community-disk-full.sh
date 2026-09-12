#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=$(cd "${1:-$repository_root/build}" && pwd)
mount_point=$(mktemp -d /tmp/pacificdb-enospc-mount-XXXXXX)
trap 'rmdir "$mount_point" 2>/dev/null || true' EXIT

unshare -Urnm bash -c '
  set -euo pipefail
  mount_point=$1
  repository_root=$2
  build_dir=$3
  ip link set lo up
  mount -t tmpfs -o size=128m,nr_inodes=32768 tmpfs "$mount_point"
  trap '\''umount "$mount_point" 2>/dev/null || true'\'' EXIT
  node "$repository_root/scripts/test-community-disk-full.mjs" "$build_dir" "$mount_point"
' bash "$mount_point" "$repository_root" "$build_dir"
