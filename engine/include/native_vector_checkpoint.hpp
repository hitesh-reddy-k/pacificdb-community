#pragma once
#include "vector_index_generation.hpp"
#include <filesystem>
#include <string>

namespace pacificdb {
// Loss of this cache never loses a committed record. Restore always scans and
// fingerprints current authoritative records before importing graph topology.
VectorIndexGeneration::Checkpoints nativeVectorCheckpoints(
    const std::filesystem::path& dataRoot,const std::string& identity,
    const json& canonicalDefinition,size_t maximumBytes);
}
