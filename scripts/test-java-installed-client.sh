#!/usr/bin/env bash
set -euo pipefail
REPOSITORY=$(cd "$(dirname "$0")/.." && pwd)
ARTIFACTS=$(realpath "${1:?usage: test-java-installed-client.sh staged-java-directory}")
VERSION=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["javaVersion"])' "$REPOSITORY/release.json")
TEST_ROOT=$(mktemp -d)
trap 'result=$?; if test "$result" = 0; then rm -rf "$TEST_ROOT"; else echo "preserved failed consumer: $TEST_ROOT" >&2; fi' EXIT
cp -R "$REPOSITORY/sdk/contracts/installed-java" "$TEST_ROOT/consumer"
MAVEN_REPOSITORY="${PACIFICDB_INSTALLED_MAVEN_REPO:-$TEST_ROOT/repository}"
mvn -B -q -Dmaven.repo.local="$MAVEN_REPOSITORY" org.apache.maven.plugins:maven-install-plugin:3.1.2:install-file \
  -Dfile="$ARTIFACTS/pacificdb-client-$VERSION.jar" -DpomFile="$ARTIFACTS/pacificdb-client-$VERSION.pom"
cd "$TEST_ROOT/consumer"
mvn -B -q -Dmaven.repo.local="$MAVEN_REPOSITORY" package dependency:build-classpath -Dmdep.outputFile="$TEST_ROOT/classpath.txt"
CLASSPATH="$(pwd)/target/classes:$(cat "$TEST_ROOT/classpath.txt")"
java -cp "$CLASSPATH" io.pacificdb.consumer.InstalledClient
if test -n "${PACIFICDB_TEST_ENGINE_BUILD:-}"; then
  : "${PACIFICDB_TEST_NODE_MODULE:?installed Node module required for engine qualification}"
  mkdir "$TEST_ROOT/harness"
  javac --release 11 -cp "$(cat "$TEST_ROOT/classpath.txt")" -d "$TEST_ROOT/harness" \
    "$REPOSITORY/sdk/java/src/test/java/io/pacificdb/IntegrationClient.java"
  PACIFICDB_TEST_SKIP_JAVA_BUILD=1 PACIFICDB_TEST_LANGUAGES=java \
  PACIFICDB_TEST_JAVA_CLASSPATH="$TEST_ROOT/harness:$(cat "$TEST_ROOT/classpath.txt")" \
    node "$REPOSITORY/scripts/test-cross-sdk-e2e.mjs" "$PACIFICDB_TEST_ENGINE_BUILD"
fi
