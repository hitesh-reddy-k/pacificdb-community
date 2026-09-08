#pragma once

#include <cstdint>

namespace pacificdb::test {

// Production builds compile this function to a no-op. Test builds additionally
// require three explicit environment values before a failpoint can activate:
// PACIFICDB_TEST_MODE is one of the explicitly allowed destructive test modes,
// PACIFICDB_TEST_FAILPOINT_CONFIRM=I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE,
// and an exact PACIFICDB_TEST_FAILPOINT name/index selection.
void hitFailpoint(const char* name, std::uint64_t raftIndex);
bool failpointsCompiled();
void requestShutdown() noexcept;

// V11.4-DIV-001 progress guard. Returns true when the caller should simulate a storage
// mutation failure for this exact Raft index. Unlike hitFailpoint(), this does not
// terminate or pause the process: it lets the REAL apply path return a real failure so a
// test can prove the failure propagates back through applyReplicatedEntry() as false and
// that no progress marker advances. Production builds compile it to a constant false, and
// test builds still require the same explicit mode/confirmation environment.
bool injectApplyFailure(const char* name, std::uint64_t raftIndex);

}  // namespace pacificdb::test
