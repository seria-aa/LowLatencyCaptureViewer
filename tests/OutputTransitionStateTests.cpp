#include "video/OutputTransitionState.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <type_traits>

using llcv::video::OutputTransitionState;
static_assert(std::is_trivially_copyable_v<OutputTransitionState>);
static_assert(sizeof(OutputTransitionState) <= 16);

// Frozen pre-extraction policy, intentionally independent of the new class.
// Replay unusual/reentrant message orders too, not only normal F11 sequences.
struct Reference {
    unsigned depth = 0;
    bool pending = false, manual = false;
    int width = -1, height = -1;
    bool Size(int w, int h) {
        if (w <= 0 || h <= 0 || (width == w && height == h)) return false;
        width = w; height = h;
        if (manual || depth > 0) { pending = true; return false; }
        return true;
    }
    bool End(bool force) {
        if (force) pending = true;
        if (depth > 0) --depth;
        if (depth == 0 && pending && !manual) {
            pending = false;
            return true;
        }
        return false;
    }
    bool Flush() {
        if (pending && depth == 0) { pending = false; return true; }
        return false;
    }
};

int main() {
    unsigned long long events = 0;
    for (unsigned seed = 0; seed < 32; ++seed) {
        std::mt19937 random(20260911 + seed);
        OutputTransitionState actual;
        Reference expected;
        for (unsigned i = 0; i < 100000; ++i) {
            bool got = false, want = false;
            switch (random() % 9) {
            case 0:
                actual.Begin(); ++expected.depth;
                break;
            case 1: case 2: {
                const bool force = (random() & 1) != 0;
                got = actual.End(force); want = expected.End(force);
                break;
            }
            case 3: {
                int w = static_cast<int>(random() % 4098) - 1;
                int h = static_cast<int>(random() % 2162) - 1;
                got = actual.OnClientSize(w, h); want = expected.Size(w, h);
                break;
            }
            case 4:
                got = actual.OnClientSize(expected.width, expected.height);
                want = expected.Size(expected.width, expected.height);
                break;
            case 5:
                expected.manual = (random() & 1) != 0;
                actual.SetManualResize(expected.manual);
                break;
            case 6:
                // WM_EXITSIZEMOVE clears manual state before normalization/flush.
                expected.manual = false; actual.SetManualResize(false);
                got = actual.TakePendingUpdate(); want = expected.Flush();
                break;
            case 7:
                actual.ResetClientSize(); expected.width = expected.height = -1;
                break;
            case 8:
                if (i % 7 == 0) { actual = {}; expected = {}; }
                break;
            }
            ++events;
            if (got != want || actual.Depth() != expected.depth ||
                actual.Pending() != expected.pending ||
                actual.ManualResize() != expected.manual ||
                actual.ClientWidth() != expected.width ||
                actual.ClientHeight() != expected.height) {
                std::fprintf(stderr, "Transition mismatch: seed=%u event=%u\n", seed, i);
                return EXIT_FAILURE;
            }
        }
    }
    std::printf("Output state: %llu events across 32 seeds match pre-extraction policy.\n", events);
    return EXIT_SUCCESS;
}
