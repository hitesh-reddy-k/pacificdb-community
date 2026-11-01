#!/usr/bin/env bash
# Build local candidate artifacts only; no registry or external-system writes.
set -euo pipefail
REPOSITORY=$(cd "$(dirname "$0")/.." && pwd)
STAGING=$(realpath -m "${1:-$REPOSITORY/build-v1.2-release/staging/sdk}")
EVIDENCE=$(realpath -m "${2:-$REPOSITORY/build-v1.2-release/evidence/sdk}")
mkdir -p "$STAGING/npm" "$STAGING/python" "$STAGING/java" "$EVIDENCE"
# Refuse stale bytes rather than accidentally mixing candidate versions.
if test -n "$(find "$STAGING" -type f -print -quit)"; then
  echo 'Staging already contains artifacts; use a fresh directory.' >&2; exit 1
fi
cd "$REPOSITORY"
VERSION=$(python3 scripts/release_identity.py --field releaseVersion)
python3 scripts/check-sdk-release-tag.py "v$VERSION"
npm pack --workspace @pacificdb/client --pack-destination "$STAGING/npm"
npm pack --workspace @pacificdb/cli --pack-destination "$STAGING/npm"
python3 -m build sdk/python --outdir "$STAGING/python"
python3 -m twine check "$STAGING/python/"*
mvn -B -q -f sdk/java/pom.xml package
cp "sdk/java/target/pacificdb-client-$VERSION"*.jar "$STAGING/java/"
cp sdk/java/pom.xml "$STAGING/java/pacificdb-client-$VERSION.pom"
python3 scripts/verify-sdk-packages.py --npm-dist "$STAGING/npm" --python-dist "$STAGING/python" --java-target "$STAGING/java" --allow-unsigned-local --output "$EVIDENCE/inspection.json"
printf 'PASS: locally staged %s; signatures and publication not performed\n' "$VERSION"
