# WAL Fuzzing

This folder contains the WAL fuzz target for libFuzzer.

## Build (libFuzzer)

```bash
cd backend/engine/build
cmake .. -DENABLE_LIBFUZZER=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc) wal_fuzz
```

Run:

```bash
./wal_fuzz -max_total_time=120 ./fuzz_corpus
```

## AFL++ (optional)

Build a standalone fuzzer with AFL++ and run it against `wal_fuzz` or a separate harness.

Example:

```bash
AFL_USE_ASAN=1 CC=afl-clang-fast CXX=afl-clang-fast++ \
  cmake .. -DENABLE_LIBFUZZER=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc) wal_fuzz

afl-fuzz -i fuzz_corpus -o fuzz_out -- ./wal_fuzz @@
```

Notes:
- The fuzz target exercises WAL binary parsing, WAL integrity JSON parsing, and compression round-trips.
- Use `scripts/chaos_wal_corrupt.js` for quick mutation-based smoke tests.
