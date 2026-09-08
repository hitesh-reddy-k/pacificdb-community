#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
classes=$(mktemp -d /tmp/pacificdb-binding-test-XXXXXX)
classpath_file=$(mktemp /tmp/pacificdb-binding-classpath-XXXXXX)
trap 'rm -rf "$classes" "$classpath_file"' EXIT
mvn -q -f benchmarks/ycsb/pacificdb-binding/pom.xml \
  dependency:build-classpath -Dmdep.outputFile="$classpath_file"
classpath=$(cat "$classpath_file")
javac -cp "$classpath" -d "$classes" \
  benchmarks/ycsb/pacificdb-binding/src/main/java/site/ycsb/db/PacificDBClient.java \
  benchmarks/ycsb/PacificDBClientTest.java
java -cp "$classpath:$classes" site.ycsb.db.PacificDBClientTest
