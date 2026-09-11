#include <random>
#include <string>

static int RunMailboxStress() {
    using namespace llcv::capture;
    const auto before = failures;
    uint64_t totalConsumed = 0, totalReplaced = 0, totalRejected = 0;
    for (unsigned seed = 1; seed <= 8; ++seed) {
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(ready != nullptr, "stress: create mailbox event");
        if (!ready) return 1;
        std::atomic<uint64_t> tracking{0}, captured{0}, replaced{0};
        {
            LatestVideoSample slot(100, ready, {&tracking, &captured, &replaced});
            auto* callback = new VideoSampleGrabberCallback(&slot);
            constexpr unsigned count = 20000;
            std::atomic<bool> done{false}, checkpointFailed{false};
            std::atomic<unsigned> consumedThrough{0};
            unsigned expectedValid = 0, expectedRejected = 0;
            std::thread producer([&] {
                std::mt19937 random(seed);
                for (unsigned i = 1; i <= count; ++i) {
                    const bool valid = i == count || i == count / 2 || (random() % 5) != 0;
                    const long sizes[]{-1, 0, 99};
                    auto* sample = new Sample(valid ? 100 : sizes[random() % 3], i);
                    if (valid) ++expectedValid; else ++expectedRejected;
                    callback->SampleCB(0, sample); sample->Release();
                    if ((random() % 7) == 0) std::this_thread::yield();
                    if (i == count / 2) {
                        const auto deadline = GetTickCount64() + 5000;
                        while (consumedThrough.load() < i && GetTickCount64() < deadline)
                            std::this_thread::yield();
                        if (consumedThrough.load() < i) checkpointFailed.store(true);
                    }
                }
                done.store(true, std::memory_order_release); SetEvent(ready);
            });
            unsigned consumed = 0, lastSequence = 0;
            int64_t lastArrival = 0;
            std::mt19937 random(seed + 1024);
            const auto take = [&] {
                int64_t arrival = 0;
                auto* frame = slot.TakeLatest(arrival);
                if (!frame) return;
                const auto sequence = static_cast<Sample*>(frame)->Sequence();
                Check(sequence > lastSequence && arrival >= lastArrival && arrival > 0,
                      "stress: no duplicate/reordered identities or timestamps");
                lastSequence = sequence; lastArrival = arrival; ++consumed;
                consumedThrough.store(sequence); frame->Release();
            };
            while (!done.load(std::memory_order_acquire)) {
                // Alternate event consumption, immediate draining, and slow consumers.
                if ((random() & 1) != 0) WaitForSingleObject(ready, 1);
                take();
                if ((random() % 3) == 0) std::this_thread::yield();
            }
            producer.join(); take();
            Check(!checkpointFailed && lastSequence == count,
                  "stress: consume while producer runs and recover the final valid frame");
            Check(captured.load() == expectedValid && consumed + replaced.load() == expectedValid,
                  "stress: every valid frame is consumed or replaced exactly once");
            Check(slot.RejectedSamples() == expectedRejected,
                  "stress: negative/empty/short payloads counted exactly");
            totalConsumed += consumed; totalReplaced += replaced.load();
            totalRejected += slot.RejectedSamples();
            callback->Release();
        }
        CloseHandle(ready);
        Check(liveSamples == 0, "stress: no retained COM samples after each seed");
    }
    std::printf("Mailbox stress: 160000 submissions, eight seeds; consumed=%llu replaced=%llu rejected=%llu failures=%u\n",
        static_cast<unsigned long long>(totalConsumed), static_cast<unsigned long long>(totalReplaced),
        static_cast<unsigned long long>(totalRejected), failures - before);
    return failures != before ? 1 : 0;
}
