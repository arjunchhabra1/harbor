#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace harbor::tools {

// Minimal --flag value parsing; remaining words become positional arguments.
struct Args {
    std::map<std::string, std::string> flags;
    std::vector<std::string> positional;

    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) == 0) {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("missing value for " + a);
                }
                flags[a.substr(2)] = argv[++i];
            } else {
                positional.push_back(a);
            }
        }
    }

    std::string str(const std::string& name, const std::string& fallback) const {
        auto it = flags.find(name);
        return it == flags.end() ? fallback : it->second;
    }
    std::uint64_t num(const std::string& name, std::uint64_t fallback) const {
        auto it = flags.find(name);
        return it == flags.end() ? fallback : std::stoull(it->second);
    }
    bool has(const std::string& name) const { return flags.contains(name); }
};

}  // namespace harbor::tools
