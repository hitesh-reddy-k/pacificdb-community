#!/usr/bin/env bash
set -euo pipefail
REPOSITORY=$(cd "$(dirname "$0")/.." && pwd)
TEST_ROOT=$(mktemp -d)
trap 'rm -rf "$TEST_ROOT"' EXIT
cp -R "$REPOSITORY/sdk/contracts/installed-java" "$TEST_ROOT/consumer"
MAVEN_REPOSITORY="${PACIFICDB_INSTALLED_MAVEN_REPO:-$TEST_ROOT/repository}"
mvn -B -q -f "$REPOSITORY/sdk/java/pom.xml" -Dmaven.repo.local="$MAVEN_REPOSITORY" install
cd "$TEST_ROOT/consumer"
mvn -B -q -Dmaven.repo.local="$MAVEN_REPOSITORY" package dependency:build-classpath -Dmdep.outputFile="$TEST_ROOT/classpath.txt"
CLASSPATH="$(pwd)/target/classes:$(cat "$TEST_ROOT/classpath.txt")"
java -cp "$CLASSPATH" io.pacificdb.consumer.InstalledClient
if test -n "${PACIFICDB_TEST_ENGINE_BUILD:-}"; then
  # Test harness classes are copied separately; the runtime client resolves solely from the installed JAR.
  mkdir "$TEST_ROOT/harness"
  javac --release 11 -cp "$(cat "$TEST_ROOT/classpath.txt")" -d "$TEST_ROOT/harness" \
    "$REPOSITORY/sdk/java/src/test/java/io/pacificdb/IntegrationClient.java"
  PACIFICDB_TEST_JAVA_CLASSPATH="$TEST_ROOT/harness:$(cat "$TEST_ROOT/classpath.txt")" \
    node "$REPOSITORY/scripts/test-cross-sdk-e2e.mjs" "$PACIFICDB_TEST_ENGINE_BUILD"
fi
