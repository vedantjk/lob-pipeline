#pragma once
// Offline percentile reporting, matching the Rung 1-5 methodology: collect raw
// samples into a preallocated vector on the hot path, sort once at the end.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

inline uint64_t pct(std::vector<uint64_t>& v, double q) {
    if (v.empty()) return 0;
    std::size_t i = static_cast<std::size_t>(q * (v.size() - 1) + 0.5);
    return v[i];
}

// Assumes v is already sorted. Prints p50/p99/p99.9 with a label + unit.
inline void report(const char* label, std::vector<uint64_t>& v, const char* unit) {
    std::sort(v.begin(), v.end());
    std::fprintf(stderr, "%-28s p50=%-6llu p99=%-6llu p99.9=%-7llu %s  (n=%zu)\n",
                 label,
                 (unsigned long long)pct(v, 0.50),
                 (unsigned long long)pct(v, 0.99),
                 (unsigned long long)pct(v, 0.999),
                 unit, v.size());
}
