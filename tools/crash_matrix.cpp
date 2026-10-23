// Crash campaign: SIGKILLs a running engine at many random points and checks
// that every recovery keeps all acknowledged commands and rebuilds the exact book.
//
//   ./build-rel/harbor_crash_matrix [trials=10000] [seed=1]

#include <cstdio>
#include <cstdlib>
#include "crash_harness.hpp"
#include "temp_dir.hpp"

int main(int argc, char** argv) {
    std::uint64_t trials = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10'000;
    std::uint64_t seedBase = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    std::uint64_t failures = 0;
    std::uint64_t maxRecovered = 0;
    std::uint64_t pastSnapshot = 0;
    for (std::uint64_t t = 0; t < trials; ++t) {
        harbor::testing::TempDir dir;
        std::uint64_t seed = seedBase + t;
        auto delay = static_cast<std::uint32_t>(200 + (seed * 7919) % 30'000);
        auto r = harbor::testing::runCrashTrial(dir.path(), seed, delay);
        if (!r.ok()) {
            ++failures;
            std::printf("FAIL trial %llu seed %llu delay %uus: reported %llu recovered %llu match %d %s\n",
                        static_cast<unsigned long long>(t), static_cast<unsigned long long>(seed), delay,
                        static_cast<unsigned long long>(r.reportedDurable),
                        static_cast<unsigned long long>(r.recoveredDurable), r.stateMatches ? 1 : 0,
                        r.error.c_str());
        }
        maxRecovered = std::max(maxRecovered, r.recoveredDurable);
        pastSnapshot += r.recoveredDurable >= 5'000 ? 1 : 0;
        if ((t + 1) % 500 == 0) {
            std::printf("%llu/%llu trials, %llu failures\n", static_cast<unsigned long long>(t + 1),
                        static_cast<unsigned long long>(trials), static_cast<unsigned long long>(failures));
            std::fflush(stdout);
        }
    }
    std::printf("done: %llu trials, %llu failures, %llu recovered past a snapshot, max recovered seq %llu\n",
                static_cast<unsigned long long>(trials), static_cast<unsigned long long>(failures),
                static_cast<unsigned long long>(pastSnapshot), static_cast<unsigned long long>(maxRecovered));
    return failures == 0 ? 0 : 1;
}
