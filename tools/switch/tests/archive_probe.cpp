#include <chrono>
#include <iostream>
#include <vector>

#include "platform/zip_archive.hpp"

/**
 * Validates archive indexing and requested file reads for the fixture driver.
 */
int main(int argc, char **argv) {
    if (argc < 2) {
        return 64;
    }
    try {
        const auto          start = std::chrono::steady_clock::now();
        kazarov::ZipArchive archive(argv[1]);
        const auto          end = std::chrono::steady_clock::now();
        std::cout << "entries " << archive.Entries().size() << " index_ms " << std::chrono::duration<double, std::milli>(end - start).count() << '\n';
        for (int n = 2; n < argc; ++n) {
            const auto index = static_cast<std::size_t>(std::stoull(argv[n]));
            if (index >= archive.Entries().size()) {
                return 4;
            }
            const auto               &entry = archive.Entries()[index];
            std::vector<std::uint8_t> data(static_cast<std::size_t>(entry.size));
            const auto                read_start = std::chrono::steady_clock::now();
            if (!archive.Read(index, data)) {
                std::cerr << "read_failed " << index << '\n';
                return 2;
            }
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto byte : data) {
                hash = (hash ^ byte) * 1099511628211ULL;
            }
            std::cout << index << ' ' << entry.size << ' ' << hash << ' ' << entry.name << " read_ms " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - read_start).count() << '\n';
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
