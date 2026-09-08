#include "storage.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;
using json = nlohmann::json;

/* ---------------- APPEND ---------------- */
void Storage::appendDocument(const std::string& file, const json& doc) {

    fs::path p(file);
    fs::create_directories(p.parent_path()); // 🔥 ensure folders exist

    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) {
        std::cerr << "[STORAGE][ERROR] Cannot open file for append: "
                  << file << "\n";
        return;
    }

    std::string data = doc.dump();
    uint32_t len = static_cast<uint32_t>(data.size());

    out.write(reinterpret_cast<char*>(&len), sizeof(len));
    out.write(data.data(), len);
    out.flush();

    std::cout << "[STORAGE][APPEND] " << data << "\n";
}

/* ---------------- READ ALL ---------------- */
std::vector<json> Storage::readAll(const std::string& file) {

    std::vector<json> docs;

    if (!fs::exists(file)) {
        std::cout << "[STORAGE][READ] File not found (empty collection): "
                  << file << "\n";
        return docs;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::cerr << "[STORAGE][ERROR] Cannot open file for read: "
                  << file << "\n";
        return docs;
    }

    while (true) {
        uint32_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&len), sizeof(len)))
            break;

        if (len == 0) continue;

        std::string buf(len, '\0');
        if (!in.read(buf.data(), len))
            break;

        try {
            docs.push_back(json::parse(buf));
        } catch (...) {
            std::cerr << "[STORAGE][ERROR] Corrupted document skipped\n";
        }
    }

    std::cout << "[STORAGE][READ] Loaded " << docs.size()
              << " docs from " << file << "\n";

    return docs;
}

/* ---------------- WRITE ALL (ATOMIC) ---------------- */
void Storage::writeAll(const std::string& file,
                       const std::vector<json>& docs) {

    fs::path p(file);
    fs::create_directories(p.parent_path());

    fs::path tmp = p;
    tmp += ".tmp";

    std::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[STORAGE][ERROR] Cannot open temp file for write: "
                  << tmp << "\n";
        return;
    }

    for (const auto& doc : docs) {
        std::string data = doc.dump();
        uint32_t len = static_cast<uint32_t>(data.size());
        out.write(reinterpret_cast<char*>(&len), sizeof(len));
        out.write(data.data(), len);
    }

    out.flush();
    out.close();

    fs::rename(tmp, p); // 🔥 atomic replace

    std::cout << "[STORAGE][WRITEALL] Persisted "
              << docs.size() << " docs to " << file << "\n";
}
