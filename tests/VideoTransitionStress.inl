// Test-only replay of real transition helpers/message handlers. No visible HWND.
#include <random>

static int RunTransitionStress(bool reentrantFault) {
    const auto savedGeneration = g_outputConfigurationGeneration.load();
    const auto savedTransition = g_outputTransition;

    const auto savedHost = g_videoHost;
    const auto savedSettings = g_settings;
    const auto savedMonitor = g_relativeMoveMonitor;
    const auto savedSnap = g_windowSnapState;
    g_videoHost = nullptr; g_settings.audioOnly = false;
    g_outputTransition = {};
    std::mt19937 random(20260911);
    unsigned failures = 0;
    const auto check = [&](bool value) { if (!value) ++failures; };
    if (reentrantFault) {
        const auto before = g_outputConfigurationGeneration.load();
        BeginOutputTransition();
        g_outputTransition.SetManualResize(true);
        WndProc(nullptr, WM_SIZE, SIZE_RESTORED, MAKELPARAM(1280, 720));
        WndProc(nullptr, WM_EXITSIZEMOVE, 0, 0);
        const auto early = g_outputConfigurationGeneration.load() - before;
        check(early == 0);
        EndOutputTransition(true);
        const auto total = g_outputConfigurationGeneration.load() - before;
        check(total == 1);
        std::printf("PROBE exit-size-inside-transition: early-rebuild=%llu total=%llu failures=%u\n",
            static_cast<unsigned long long>(early), static_cast<unsigned long long>(total), failures);
    } else for (int cycle = 0; cycle < 10000; ++cycle) {
        const auto before = g_outputConfigurationGeneration.load();
        const bool manual = (random() & 1) != 0;
        g_outputTransition.SetManualResize(manual);
        const unsigned depth = 1 + random() % 4;
        for (unsigned i = 0; i < depth; ++i) BeginOutputTransition();
        bool changed = false;
        for (unsigned event = 0, count = 1 + random() % 20; event < count; ++event) {
            const unsigned kind = random() % 4;
            if (kind == 0) {
                WndProc(nullptr, WM_SIZE, SIZE_MINIMIZED, 0);
            } else {
                int width = kind == 1 ? g_outputTransition.ClientWidth() : 640 + 2 * (random() % 1601);
                int height = kind == 1 ? g_outputTransition.ClientHeight() : 360 + 2 * (random() % 901);
                if (width <= 0 || height <= 0) { width = 640; height = 360; }
                changed |= width != g_outputTransition.ClientWidth() || height != g_outputTransition.ClientHeight();
                WndProc(nullptr, WM_SIZE, SIZE_RESTORED, MAKELPARAM(width, height));
            }
            check(g_outputConfigurationGeneration.load() == before);
        }
        for (unsigned i = depth; i > 0; --i) {
            const bool force = (random() % 4) == 0;
            changed |= force;
            EndOutputTransition(force);
            check(g_outputConfigurationGeneration.load() == before +
                ((i == 1 && !manual && changed) ? 1 : 0));
        }
        if (manual) WndProc(nullptr, WM_EXITSIZEMOVE, 0, 0);
        check(g_outputConfigurationGeneration.load() == before + (changed ? 1 : 0));
        check(g_outputTransition.Depth() == 0 && !g_outputTransition.Pending() && !g_outputTransition.ManualResize());
        const auto stable = g_outputConfigurationGeneration.load();
        WndProc(nullptr, WM_SIZE, SIZE_RESTORED,
            MAKELPARAM(g_outputTransition.ClientWidth(), g_outputTransition.ClientHeight()));
        check(g_outputConfigurationGeneration.load() == stable);
    }
    g_outputConfigurationGeneration.store(savedGeneration);
    g_outputTransition = savedTransition;
    g_videoHost = savedHost; g_settings = savedSettings;
    g_relativeMoveMonitor = savedMonitor; g_windowSnapState = savedSnap;
    if (!reentrantFault) std::printf("Transition stress: 10000 randomized sequences; failures=%u\n", failures);
    return failures ? 1 : 0;
}
