# PacificDB Community production-certification expansion design

## Goal

Extend the existing isolated Community certification with genuine Linux
out-of-space failures, TCP partition and election recovery, sustained RF3
loads at 64 and 128 clients, exhaustive public-contract edge cases, complete
restart sequences, a real disposable package-manager installation, and the
exact reported project recovery scenario.

The work must not alter or open the installed PacificDB data root. A successful
Linux run is evidence for the tested Linux artifact only. Physical-host power
loss, a physical storage controller, Windows execution/signing, and macOS
execution/signing/notarization require separate environments and credentials;
they must never be reported as passed from Linux simulations.

## Selected approach

Use several focused certification programs rather than adding a single large
script:

1. Run the engine in a rootless mount namespace with a deliberately small
   `tmpfs`, exhaust that filesystem, and exercise each Community durable-write
   category. Every acknowledged pre-fault object must recover after space is
   released, and every failed write must return an error rather than false
   success.
2. Put each directed Raft peer connection behind a disposable user-space TCP
   proxy. Dropping all four links to the current leader creates a bidirectional
   partition without host firewall changes. The majority must elect a leader,
   the isolated leader must not acknowledge a quorum write, and all replicas
   must converge after healing.
3. Reuse the same RF3 cluster machinery for three sequential 10-minute runs at
   64 clients and three at 128 clients. Record request counts, errors, Raft
   terms, commit/apply indexes, and final convergence. Do not describe these
   correctness/load runs as performance comparisons.
4. Add focused contract and restart scenarios for every edge requested in the
   P0 prompt. A fake TCP server supplies timeout and malformed-response cases
   for every engine-backed shell category.
5. Install the generated Debian package with `apt` inside a disposable Ubuntu
   container, start the packaged engine, and exercise the packaged CLI.
6. Probe separately supplied Windows and macOS hosts only through explicit,
   working access. Signing requires a Windows Authenticode certificate;
   notarization requires an Apple Developer ID certificate and Apple
   notarization credentials. Missing hosts or credentials are blocking facts,
   not test failures and not permission to bypass platform security.

## Fault boundaries

The disk-full test uses an isolated mount namespace and `tmpfs`; it cannot fill
the host filesystem. It covers project metadata, database/collection metadata,
documents, API-key records, backup output, media manifests/chunks, vectors,
and local CLI context/history. Each destructive case receives a fresh root.

The network test proxies all six directed links between three local nodes.
Partition control closes active sockets and rejects new ones. Client ports stay
reachable so the test can prove that the isolated former leader refuses or
fails a write while the two-node majority continues.

The virtual hard-power test, if a disposable VM can be provisioned, terminates
the VM process without guest shutdown after acknowledged writes. This models a
guest power cut and virtual controller disappearance. It does not certify a
physical disk controller or its volatile cache.

## Success criteria

- Disk-full cases produce explicit errors and no acknowledged pre-fault data
  disappears after recovery.
- The RF3 majority elects a new leader during partition, the minority does not
  acknowledge a quorum write, and all nodes converge after healing.
- All six sustained RF3 runs finish with zero request errors, no unexplained
  election, no lost or duplicate logical writes, and commit/apply convergence.
- Every requested edge case has an executable assertion and a recorded result.
- Every persistent type completes create/read, graceful restart/read,
  modify/read, graceful restart/read, abrupt termination/recovery/read.
- The exact original project scenario passes through two normal restarts and a
  fresh shell, while an unknown project ID fails without changing context.
- `apt` installs, starts, and removes beta.7 inside a disposable container.
- The final report distinguishes PASS, FAIL, NOT TESTED, PARTIALLY TESTED, and
  BLOCKED without converting missing hardware or credentials into a pass.

## Repository and release constraints

- Do not push, publish, tag, or create a release during certification;
  repository publication requires separate explicit authorization after
  verification.
- Do not modify paid/private control-plane code.
- Do not install beta.7 over the user's beta.3 host package.
- Add production-code changes only when a new test exposes a real defect, and
  add a focused failing regression before the fix.
- Remove physical-host and native-platform gates from the Linux verdict only;
  retain them as separate platform/hardware certification gates.
