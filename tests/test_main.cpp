#include <cstring>

#include "check.h"

int main(int argc, char** argv) {
    int ran = 0, failedTests = 0;
    for (const auto& t : check::registry()) {
        if (argc > 1 && !std::strstr(t.name, argv[1])) continue;
        int before = check::failures();
        std::printf("  %-48s", t.name);
        std::fflush(stdout);
        t.fn();
        bool ok = check::failures() == before;
        std::printf("%s\n", ok ? "ok" : "");
        ran++;
        failedTests += ok ? 0 : 1;
    }
    std::printf("\n%d tests, %d failed\n", ran, failedTests);
    return failedTests ? 1 : 0;
}
