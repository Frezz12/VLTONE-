#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Engine/RealtimeEngine.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include "Nodes/MeterNode.hpp"
#include "Job/BackgroundExecutor.hpp"
#include "Memory/PcmReadCache.hpp"
#include "ScopedNoDenormals.hpp"
#include "RealtimeMetrics.hpp"
#include "Device/AudioDeviceManager.hpp"
#include <atomic>
#include <bit>
#include <cstdio>
#include <future>
#include <limits>
#include <thread>
using namespace daw::engine;
using namespace std::chrono_literals;
static int failures = 0;
static void check(bool ok, const char* text) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }
struct Output {
    std::vector<float> l, r; float* pointers[2];
    explicit Output(unsigned frames) : l(frames), r(frames), pointers{l.data(), r.data()} {}
    AudioBlock block() { return {pointers, 2, FrameCount(l.size())}; }
};
struct Source : Node {
    std::string_view name() const noexcept override { return "test source"; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }
    void process(const ProcessContext& c) override {
        for (auto ch = 0u; ch < c.output.numChannels(); ++ch)
            for (auto f = 0u; f < c.frames; ++f) c.output.data(ch)[f] = float(f + ch) / 8192.f;
    }
};
int main() {
    {
        LockFreeSPSCQueue<std::uint32_t, 256> queue;
        std::atomic<bool> done{false}, ordered{true};
        std::atomic<unsigned> consumed{0};
        std::thread consumer([&] {
            std::uint32_t expected = 1, value;
            while (!done.load(std::memory_order_acquire) || !queue.empty()) {
                if (queue.pop(value)) {
                    if (value != expected++) ordered = false;
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else std::this_thread::yield();
            }
        });
        bool snapshotsOrdered = true;
        for (std::uint32_t value = 1; value <= 20000; ++value) {
            while (!queue.push(value)) std::this_thread::yield();
            if (value % 13 == 0) {
                const auto snapshot = queue.snapshotFromProducer();
                for (std::size_t i = 0; i < snapshot.size(); ++i)
                    snapshotsOrdered &= snapshot[i] <= value && (!i || snapshot[i] == snapshot[i - 1] + 1);
            }
        }
        done.store(true, std::memory_order_release); consumer.join();
        check(ordered && snapshotsOrdered && consumed == 20000 && queue.snapshotFromProducer().empty(),
              "producer snapshots preserve event order without stealing or duplicating concurrent audio input");
    }
    {
        JobSystem dormant(4);
        daw::rt::ProfileEvent event;
        check(dormant.poolThreadCount() == 0 && !dormant.popProfile(0, event) &&
              dormant.droppedProfileEvents() == 0,
              "unprepared runtimes allocate no DSP threads or profiling buffers");
        daw::rt::AudioWorkerConfig inactive;
        inactive.maxParallelThreads = 2;
        dormant.configureAudioWorkers(inactive);
        check(dormant.poolThreadCount() == 0, "inactive worker configuration keeps the pool dormant");
        dormant.prepare(64);
        check(dormant.poolThreadCount() == 3 && dormant.workerCount() == 2,
              "first graph preparation starts the pool and applies its retained configuration");
        dormant.prepare(32);
        check(dormant.poolThreadCount() == 3, "repeated preparation reuses existing workers");
    }
    {
        JobSystem jobs(3);
        jobs.configureAudioWorkers({true, 48000, 128, {}});
        jobs.prepare(64);
        unsigned completed = 0;
        jobs.setSink({[](void* state, std::uint32_t, unsigned) -> std::uint32_t {
            ++*static_cast<unsigned*>(state); return 1;
        }, &completed});
        jobs.beginPass(1, 1); jobs.submit(0, 0); jobs.waitForPass();
        check(jobs.poolThreadCount() == 2 && completed == 1,
              "device configuration before graph preparation starts a usable pool");
    }
    {
        JobSystem jobs(8);
        std::atomic<unsigned> completed{0};
        jobs.setSink({[](void* state, std::uint32_t, unsigned) -> std::uint32_t {
            static_cast<std::atomic<unsigned>*>(state)->fetch_add(1, std::memory_order_relaxed);
            return 1;
        }, &completed});
        jobs.prepare(65);
        check(!jobs.needsGrowthFor(128) && jobs.needsGrowthFor(129),
              "graph growth reuses the deque capacity already allocated");
        unsigned passes = 0;
        for (std::size_t capacity = 128; capacity <= 8192; capacity *= 2) {
            for (unsigned i = 0; i < 16; ++i) {
                jobs.beginPass(64, 7);
                for (unsigned item = 0; item < 64; ++item) jobs.submit(0, item);
                jobs.waitForPass(); ++passes;
            }
            // Helpers can still be leaving empty-queue probes at completion.
            jobs.prepare(capacity + 1);
        }
        check(completed == passes * 64 && jobs.poolThreadCount() == 7,
              "growing immediately after a pass drains helper probes and preserves every job");
    }
    {
        struct Payload {
            std::promise<void>& entered;
            std::shared_future<void> release;
            Payload(std::promise<void>& e, std::shared_future<void> r) : entered(e), release(std::move(r)) {}
            ~Payload() { entered.set_value(); release.wait(); }
        };
        std::promise<void> entered, release;
        RealtimeSnapshot<Payload> snapshot;
        snapshot.publish(std::make_shared<Payload>(entered, release.get_future().share()));
        std::thread publisher([&] { snapshot.publish({}); });
        entered.get_future().wait();
        auto observer = std::async(std::launch::async, [&] { return !snapshot.controlCopy(); });
        const bool nonblocking = observer.wait_for(250ms) == std::future_status::ready;
        release.set_value(); publisher.join();
        check(observer.get() && nonblocking, "snapshot observers never wait for a retired payload destructor");
    }
    {
        struct RetiringSource : Source {
            std::promise<void>& entered;
            std::shared_future<void> release;
            RetiringSource(std::promise<void>& e, std::shared_future<void> r) : entered(e), release(std::move(r)) {}
            ~RetiringSource() override { entered.set_value(); release.wait(); }
        };
        std::promise<void> entered, release;
        GraphProcessor processor(2);
        {
            AudioGraph graph;
            graph.setSink(graph.addNode(std::make_unique<RetiringSource>(entered, release.get_future().share())));
            auto compiled = graph.compile({48000, 64, 2});
            processor.setGraph(*compiled);
        }
        std::thread publisher([&] { processor.setGraph({}); });
        entered.get_future().wait();
        auto observer = std::async(std::launch::async, [&] { return !processor.graph(); });
        const bool nonblocking = observer.wait_for(250ms) == std::future_status::ready;
        release.set_value(); publisher.join();
        check(observer.get() && nonblocking, "UI graph snapshots never wait for retired DSP destruction");
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    {
        WorkStealingDeque queue; queue.reserve(64);
        std::uint32_t item = 0;
        bool correct = true;
        for (unsigned pass = 0; pass < 10000; ++pass) {
            correct &= !queue.pop(item);
            queue.push(pass);
            correct &= queue.steal(item) && item == pass && !queue.pop(item);
            queue.push(pass + 1);
            correct &= queue.pop(item) && item == pass + 1 && !queue.steal(item);
        }
        check(correct, "empty deque fast path survives last-item steals and repeated reuse");
    }
    {
        constexpr unsigned batches = 512, batchSize = 128, count = batches * batchSize;
        WorkStealingDeque queue; queue.reserve(256);
        auto seen = std::make_unique<std::atomic<unsigned>[]>(count);
        std::atomic<unsigned> consumed{0};
        std::atomic<bool> done{false}, valid{true};
        const auto record = [&](std::uint32_t item) {
            if (item >= count || seen[item].fetch_add(1, std::memory_order_relaxed) != 0)
                valid.store(false, std::memory_order_relaxed);
            consumed.fetch_add(1, std::memory_order_release);
        };
        std::array<std::thread, 3> thieves;
        for (auto& thief : thieves) thief = std::thread([&] {
            while (!done.load(std::memory_order_acquire)) {
                std::uint32_t item;
                if (queue.steal(item)) record(item);
                else std::this_thread::yield();
            }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (unsigned batch = 0; batch < batches && valid.load(); ++batch) {
            for (unsigned i = 0; i < batchSize; ++i) queue.push(batch * batchSize + i);
            while (consumed.load(std::memory_order_acquire) < (batch + 1) * batchSize) {
                std::uint32_t item;
                if (queue.pop(item)) record(item);
                else std::this_thread::yield();
                if (std::chrono::steady_clock::now() > deadline) { valid = false; break; }
            }
        }
        done.store(true, std::memory_order_release);
        for (auto& thief : thieves) thief.join();
        for (unsigned i = 0; i < count; ++i) valid = valid.load() && seen[i].load() == 1;
        check(valid && consumed == count,
              "owner and concurrent thieves deliver every task once across empty/refill cycles");
    }
    {
        RealtimeSnapshot<std::array<unsigned, 2>> snapshot;
        check(!snapshot.read(), "an unpublished snapshot has an empty reader");
        auto value = std::make_shared<const std::array<unsigned, 2>>(std::array{7u, ~7u});
        std::weak_ptr<const std::array<unsigned, 2>> lifetime = value;
        snapshot.publish(value); value.reset();
        {
            auto reader = snapshot.read();
            snapshot.publish({});
            check(!lifetime.expired() && (*reader)[0] == 7,
                  "clearing a snapshot preserves an outstanding reader");
        }
        snapshot.publish({});
        check(lifetime.expired() && !snapshot.read(), "released snapshots are reclaimed off the reader thread");
        std::atomic<bool> done{false}, consistent{true};
        std::thread reader([&] {
            while (!done.load(std::memory_order_acquire)) {
                auto current = snapshot.read();
                if (current && (*current)[1] != ~(*current)[0]) consistent = false;
            }
        });
        for (unsigned i = 0; i < 10000; ++i) {
            snapshot.publish(std::make_shared<const std::array<unsigned, 2>>(std::array{i, ~i}));
            snapshot.publish({});
        }
        done.store(true, std::memory_order_release); reader.join();
        check(consistent, "empty/nonempty snapshot publication stays coherent with a concurrent reader");
    }
    {
        daw::rt::DiagnosticRing<daw::rt::BlockTiming, 4> ring;
        check(ring.push({1}) && ring.push({2}) && ring.push({3}) && !ring.push({4}), "bounded telemetry drops without overwriting");
        daw::rt::BlockTiming event;
        bool ordered = true;
        for (unsigned i = 1; i < 4; ++i) ordered &= ring.pop(event) && event.elapsedNs == i;
        check(ordered && !ring.pop(event) && ring.dropped() == 1, "telemetry preserves FIFO and dropped count");
        auto metrics = std::make_unique<daw::rt::BlockMetrics>();
        for (unsigned i = 1; i <= 1000; ++i) metrics->record(i * 1000, 1, 2000);
        daw::rt::TimingAccumulator samples; samples.drain(*metrics);
        auto summary = samples.summary();
        check(summary.count == 1000 && summary.p95Ms == .95 && summary.p99Ms == .99 &&
            summary.p999Ms == .999 && summary.maximumMs == 1.0 && summary.overruns == 500,
            "per-block quantiles and budget overruns are exact");
        for (unsigned i = 0; i < 9000; ++i) metrics->record(2000000, 1, 2000);
        check(metrics->counters().maximumNs == 2000000 && metrics->counters().overruns == 9500 &&
            metrics->counters().dropped > 0, "lifetime peaks and overruns survive telemetry overflow");
    }
    {
        auto device = std::make_unique<audio::AudioDeviceManager>();
        device->processStream(nullptr, nullptr, 256, 0x0f); // PA's four xrun bits.
        const auto counts = device->xruns();
        check(counts.inputUnderflow == 1 && counts.inputOverflow == 1 && counts.outputUnderflow == 1 && counts.outputOverflow == 1 && device->callbackMetrics().counters().blocks == 1,
            "PortAudio xrun flags and complete callback are counted even on early return");
    }
    {
        std::array<float, 19> values{};
        bool correct = true;
        const daw::rt::ScopedNoDenormals outer;
        { const daw::rt::ScopedNoDenormals nested; check(dsp::isSilent(values), "exact zero remains silent with FTZ"); }
        for (std::uint32_t bits : {1u, 0x80000001u, 0x7fc00000u, 0x7f800000u, 0x3f800000u})
            for (unsigned i = 0; i < values.size(); ++i) {
                values.fill(0); values[i] = std::bit_cast<float>(bits);
                correct &= !dsp::isSilent(values);
            }
        values.fill(-0.f);
        check(correct && dsp::isSilent(values), "FTZ silence contract preserves subnormals, NaN, infinity and signed zero");
    }
    {
        auto ownedCache = std::make_unique<PcmReadCache>(8 * PcmReadCache::kPageSamples * sizeof(float));
        auto& cache = *ownedCache;
        auto backing = std::make_shared<std::vector<float>>(32 * PcmReadCache::kPageSamples, .375f);
        auto& original = *backing;
        const auto source = cache.addSource({backing, original.data()}, original.size());
        cache.warm(source, 0, PcmReadCache::kPageSamples);
        { PcmReadScope scope(true); check(scope.view(cache, source, 12, 7).front() == .375f, "warm PCM reads pinned RAM"); }
        { PcmReadScope scope(true); check(scope.view(cache, source, 24 * PcmReadCache::kPageSamples, 1).front() == 0, "cold PCM returns silence without reading its mapping"); }
        bool ready = false;
        for (int i = 0; i < 200 && !ready; ++i) {
            std::this_thread::sleep_for(1ms);
            PcmReadScope scope(true); ready = scope.view(cache, source, 24 * PcmReadCache::kPageSamples, 1).front() == .375f;
        }
        check(ready && cache.counters().misses > 0, "background read resolves a measured PCM miss");
        {
            PcmReadScope scope(true);
            const auto first = 16 * PcmReadCache::kPageSamples;
            (void)scope.view(cache, source, first, 1);
            cache.warm(source, first, PcmReadCache::kPageSamples);
            bool recovered = false;
            for (int i=0;i<100 && !recovered;++i) {
                std::this_thread::sleep_for(1ms);
                recovered = scope.view(cache, source, first, 1).front() == .375f;
            }
            check(recovered,"a PCM miss can recover within the same node block");
        }
        const auto budget = cache.counters().capacityBytes;
        for (unsigned page = 0; page < 32; ++page) cache.warm(source, page * PcmReadCache::kPageSamples, PcmReadCache::kPageSamples);
        check(cache.counters().capacityBytes == budget, "PCM cache capacity stays fixed under eviction");
        cache.warm(source, 0, 4 * PcmReadCache::kPageSamples);
        const auto missesBefore = cache.counters().misses;
        std::atomic<bool> allReady{true}; std::vector<std::thread> readers;
        for (unsigned n = 0; n < 8; ++n) readers.emplace_back([&] {
            for (unsigned i = 0; i < 2000; ++i) {
                PcmReadScope scope(true);
                if (scope.view(cache, source, 0, 16).front() != .375f) allReady = false;
            }
        });
        for (auto& reader : readers) reader.join();
        check(allReady && cache.counters().misses == missesBefore, "concurrent PCM readers do not manufacture cache misses");
        cache.invalidateRequests(); cache.removeSource(source);
        const auto other = cache.addSource({backing, original.data()}, original.size());
        check(other != source, "retired PCM requests cannot alias a new mapping");
        cache.removeSource(other);
    }
    {
        PcmReadCache cache;
        constexpr auto pageBytes = PcmReadCache::kPageSamples * sizeof(float);
        check(cache.counters().allocatedBytes == 0 && cache.counters().capacityBytes == 256u * 1024u * 1024u,
              "an unused PCM cache commits no audio pages without reducing its budget");
        auto data = std::make_shared<std::vector<float>>(PcmReadCache::kPageSamples, .375f);
        const auto id = cache.addSource({data, data->data()}, data->size());
        cache.warm(id, 0, data->size());
        check(cache.counters().allocatedBytes == pageBytes, "warming one PCM page allocates only one page");
        {
            PcmReadScope scope(true);
            const auto samples = scope.view(cache, id, 0, data->size());
            cache.removeSource(id);
            cache.warm(id, 0, data->size()); // retired IDs cannot resurrect a source
            check(samples.front() == .375f && samples.back() == .375f && cache.counters().allocatedBytes == pageBytes,
                  "retirement preserves a page pinned by an in-flight audio reader");
        }
        for (unsigned i = 0; i < 200 && cache.counters().allocatedBytes; ++i) std::this_thread::sleep_for(1ms);
        check(cache.counters().allocatedBytes == 0 && cache.counters().lockedBytes == 0,
              "retired PCM pages are freed after their last reader without another project load");
        bool bounded = true;
        for (unsigned round = 0; round < 64; ++round) {
            const auto source = cache.addSource({data, data->data()}, data->size());
            cache.warm(source, 0, data->size());
            cache.hint(source, 0);
            cache.removeSource(source);
            bounded &= cache.counters().allocatedBytes == 0;
        }
        std::this_thread::sleep_for(10ms);
        check(bounded && cache.counters().allocatedBytes == 0,
              "repeated source retirement releases PCM even with queued background requests");
    }
    {
        auto cache = std::make_unique<PcmReadCache>(8 * PcmReadCache::kPageSamples * sizeof(float));
        auto backing = std::make_shared<std::vector<float>>(24 * PcmReadCache::kPageSamples, .625f);
        auto& original = *backing;
        const auto source = cache->addSource({backing, original.data()}, original.size());
        bool ready = true;
        for (unsigned round = 0; round < 512; ++round) {
            const auto firstPage = (round % 3) * 8;
            cache->invalidateRequests();
            cache->warm(source, firstPage * PcmReadCache::kPageSamples,
                        4 * PcmReadCache::kPageSamples);
            PcmReadScope scope(true);
            for (unsigned page = firstPage; page < firstPage + 4; ++page)
                ready &= scope.view(*cache, source, page * PcmReadCache::kPageSamples, 1).front() == .625f;
        }
        check(ready && cache->counters().misses == 0,
              "warming a page range does not evict earlier pages in the same warm-up");
        cache->removeSource(source);
    }
    {
        auto cache = std::make_unique<PcmReadCache>(128 * PcmReadCache::kPageSamples * sizeof(float));
        auto backing = std::make_shared<std::vector<float>>(12 * PcmReadCache::kPageSamples);
        auto& original = *backing;
        for (unsigned page = 0; page < 12; ++page)
            std::fill_n(original.data() + page * PcmReadCache::kPageSamples,
                        PcmReadCache::kPageSamples, float(page + 1));
        const auto source = cache->addSource({backing, original.data()}, original.size());
        cache->warm(source, 0, original.size());
        bool correct = PcmReadScope::current() == nullptr;
        {
            PcmReadScope scope(true);
            { PcmReadScope offline(false); correct &= PcmReadScope::current() == nullptr; }
            correct &= PcmReadScope::current() == &scope;
            // Fill, reuse, then replace all eight lazily initialized cursors.
            for (unsigned round = 0; round < 3; ++round) for (unsigned page = 0; page < 12; ++page) {
                const auto first = page * PcmReadCache::kPageSamples;
                correct &= scope.view(*cache, source, first, 7).front() == float(page + 1);
                correct &= scope.view(*cache, source, first + 3, 2).back() == float(page + 1);
            }
        }
        check(correct && !PcmReadScope::current() && cache->counters().misses == 0,
              "lazy PCM cursors preserve hits, replacement and nested realtime/offline scopes");
        cache->removeSource(source);
    }
    {
        PcmReadCache cache(8 * PcmReadCache::kPageSamples * sizeof(float));
        constexpr std::size_t samples = 1024 * 1024 / sizeof(float);
        std::shared_ptr<const float> mapping;
        {
            SampleStorage storage(samples);
            std::fill_n(storage.data(), samples, .75f);
            mapping = storage.mappedSamples();
        }
        std::weak_ptr<const float> lifetime = mapping;
        const auto source = cache.addSource(mapping, samples);
        mapping.reset();
        cache.warm(source, 0, PcmReadCache::kPageSamples);
        {
            PcmReadScope scope(true);
            check(!lifetime.expired() && scope.view(cache, source, 0, 1).front() == .75f,
                  "PCM cache retains mapped storage after its document releases it");
        }
        cache.invalidateRequests(); cache.removeSource(source);
        for (unsigned i = 0; i < 200 && !lifetime.expired(); ++i) std::this_thread::sleep_for(1ms);
        check(lifetime.expired(), "retired PCM storage is released after in-flight copies finish");
    }
    {
        PcmReadCache cache(8 * PcmReadCache::kPageSamples * sizeof(float));
        std::promise<void> entered, release;
        auto done = release.get_future().share();
        auto mapping = std::shared_ptr<const float>(new float[PcmReadCache::kPageSamples]{},
            [&](const float* data) { entered.set_value(); done.wait(); delete[] data; });
        const auto source = cache.addSource(std::move(mapping), PcmReadCache::kPageSamples);
        std::thread retirement([&] { cache.removeSource(source); });
        entered.get_future().wait();
        auto other = std::make_shared<std::vector<float>>(PcmReadCache::kPageSamples, 1.f);
        auto registration = std::async(std::launch::async, [&] {
            return cache.addSource({other, other->data()}, other->size());
        });
        const bool nonblocking = registration.wait_for(250ms) == std::future_status::ready;
        release.set_value(); retirement.join();
        cache.removeSource(registration.get());
        check(nonblocking, "source registration never waits for another mapping's retirement");
    }
    {
        auto& pool = BackgroundExecutor::instance();
        BackgroundPlaybackLease playing; playing.setPlaying(true);
        std::atomic<int> active{0}, peak{0}, done{0};
        std::vector<BackgroundExecutor::Handle> tasks;
        for (int i = 0; i < 8; ++i) tasks.push_back(pool.submit([&] {
            const int count = ++active; int seen = peak.load();
            while (seen < count && !peak.compare_exchange_weak(seen, count)) {}
            std::this_thread::sleep_for(2ms); --active; ++done;
        }));
        for (int n = 0; n < 2000 && done != 8; ++n) std::this_thread::sleep_for(1ms);
        for (const auto& task : tasks) pool.cancelAndWait(task);
        check(done == 8 && peak == 1, "sampler admission limits playback to one preparation");
        playing.setPlaying(false);
        std::promise<void> release; auto wait = release.get_future().share();
        std::atomic<unsigned> started{0};
        auto a = pool.submit([&] { ++started; wait.wait(); });
        auto b = pool.submit([&] { ++started; wait.wait(); });
        for (int n = 0; n < 1000 && started != 2; ++n) std::this_thread::sleep_for(1ms);
        auto cancelled = pool.submit([&] { ++started; }); pool.cancel(cancelled);
        check(started == 2, "stopped transport admits two workers and cancels queued destruction");
        release.set_value(); pool.cancelAndWait(a); pool.cancelAndWait(b);
        check(started == 2, "nonblocking cancellation removes queued work before admission");

        std::promise<void> ownedStarted, ownedRelease;
        auto ownedWait = ownedRelease.get_future().share();
        auto payload = std::make_shared<int>(42);
        std::weak_ptr<int> payloadLifetime = payload;
        auto owned = pool.submit([payload, &ownedStarted, ownedWait] {
            ownedStarted.set_value();
            ownedWait.wait();
        });
        ownedStarted.get_future().wait();
        auto cancellation = std::async(std::launch::async, [&] { pool.cancel(owned); });
        const bool cancelledPromptly = cancellation.wait_for(250ms) == std::future_status::ready;
        if (cancelledPromptly) {
            cancellation.get();
            owned.reset(); payload.reset();
        }
        check(cancelledPromptly && !payloadLifetime.expired(),
              "cancelling running work returns promptly and retains its owned inputs");
        ownedRelease.set_value();
        if (!cancelledPromptly) cancellation.get();
        pool.cancelAndWait(owned); owned.reset(); payload.reset();
        for (int n = 0; n < 1000 && !payloadLifetime.expired(); ++n) std::this_thread::sleep_for(1ms);
        check(payloadLifetime.expired(), "cancelled work releases its inputs after the worker finishes");

        // A queue can remain nonempty while playback restricts admission. Stop
        // must admit its second task without relying on another submit/finish.
        playing.setPlaying(true);
        std::promise<void> finish; auto finishWait = finish.get_future().share();
        started = 0;
        auto held = pool.submit([&] { ++started; finishWait.wait(); });
        for (int n = 0; n < 1000 && started != 1; ++n) std::this_thread::sleep_for(1ms);
        auto pending = pool.submit([&] { ++started; });
        std::this_thread::sleep_for(10ms);
        check(started == 1, "queued background work respects active playback admission");
        playing.setPlaying(false);
        for (int n = 0; n < 1000 && started != 2; ++n) std::this_thread::sleep_for(1ms);
        check(started == 2, "stopping playback admits queued work while another task is still running");
        finish.set_value(); pool.cancelAndWait(held); pool.cancelAndWait(pending);
        std::this_thread::sleep_for(10ms);
        auto afterIdle = pool.submit([&] { ++started; });
        for (int n = 0; n < 1000 && started != 3; ++n) std::this_thread::sleep_for(1ms);
        pool.cancelAndWait(afterIdle);
        check(started == 3, "a sleeping empty executor wakes for its next submission");
    }
    for (double rate : {48000., 96000.}) for (unsigned frames : {8u, 16u, 32u, 256u, 512u}) {
        AudioGraph graph; const auto sink = graph.addNode(std::make_unique<SumNode>()); graph.setSink(sink);
        for (int i = 0; i < 64; ++i) {
            auto previous = graph.addNode(std::make_unique<Source>());
            for (int j = 0; j < 4; ++j) {
                auto node = graph.addNode(std::make_unique<GainNode>());
                graph.connect(previous, node); previous = node;
            }
            const auto meter = graph.addNode(std::make_unique<MeterNode>());
            graph.connect(previous, meter); graph.connect(meter, sink);
        }
        const auto compiled = graph.compile({rate, frames, 2});
        if (!compiled) { check(false, "fusion graph compiles"); continue; }
        GraphProcessor processor(4); processor.setGraph(*compiled);
        Output fused(frames), plain(frames), serial(frames);
        processor.process(fused.block(), frames, 0, true);
        processor.setTaskFusion(false); processor.process(plain.block(), frames, 0, true);
        processor.processSerial(serial.block(), frames, 0, true);
        check((*compiled)->taskCount < (*compiled)->nodes.size() && fused.l == plain.l && fused.r == plain.r && plain.l == serial.l,
              "fused, unfused and serial graph outputs agree at 48/96k and small/large buffers");
        processor.configureAudioWorkers({true, rate, frames, {}});
        processor.configureAudioWorkers({});
    }
    {
        bool correct = true;
        for (unsigned channels : {1u, 2u}) for (unsigned frames : {8u, 16u, 32u, 256u})
            for (bool fading : {false, true}) {
                auto sample = std::make_shared<SampleBuffer>(channels, 262144, 48000);
                for (unsigned ch = 0; ch < channels; ++ch)
                    for (unsigned i = 0; i < sample->frames(); ++i)
                        sample->writableChannel(ch)[i] = float(int((i + ch * 17) % 127) - 63) / 128.f;
                auto player = std::make_shared<ClipPlayerNode>();
                auto clips = std::make_shared<ClipPlayerNode::ClipList>();
                ClipPlacement clip;
                clip.audio = sample;
                clip.startSample = 17;
                clip.lengthSamples = 64;
                clip.sourceStartFrame = 4092; // Cross a pinned page boundary.
                clip.sourceEndFrame = 4147.25; // Last integral frame remains audible.
                clip.gain = .5f;
                clip.pan = .25f;
                clip.fadeInSamples = fading ? 7 : 0;
                clip.fadeOutSamples = fading ? 11 : 0;
                clips->push_back(clip);
                player->setClips(clips);
                AudioGraph graph;
                const auto id = graph.adoptNode(player); graph.setSink(id);
                const auto compiled = graph.compile({48000, frames, 2});
                if (!compiled) { correct = false; continue; }
                GraphProcessor processor(1); processor.setGraph(*compiled);
                player->preparePlayback(0);
                Output realtime(frames), offline(frames), expectedBlock(frames);
                dsp::DeClick expectedEdges;
                std::vector<float> renderedLeft, renderedRight;
                for (SamplePos position = 0; position < 100; position += frames) {
                    correct &= bool(processor.process(realtime.block(), frames, position, true, false));
                    for (unsigned ch = 0; ch < 2; ++ch) for (unsigned i = 0; i < frames; ++i) {
                        const auto relative = position + i - clip.startSample;
                        float expected = 0;
                        if (relative >= 0 && relative < 64 && 4092 + relative < clip.sourceEndFrame) {
                            const auto sourceCh = std::min(ch, channels - 1);
                            const float gain = ch == 0 ? .375f : .5f;
                            const float fade = fading ? float(std::clamp(std::min(
                                double(relative) / 7, double(64 - relative) / 11), 0., 1.)) : 1.f;
                            expected = sample->channel(sourceCh)[4092 + relative] * gain * fade;
                        }
                        expectedBlock.block().data(ch)[i] = expected;
                    }
                    // Clip-edge protection now also applies offline. Check the
                    // scalar PCM reference through the same independently tested
                    // edge filter, preserving its history across callbacks.
                    FrameCount offset = 0;
                    for (const auto edge : {clip.startSample, clip.startSample + clip.lengthSamples}) {
                        if (edge < position || edge >= position + frames) continue;
                        const auto at = FrameCount(edge - position);
                        expectedEdges.process(expectedBlock.block(), at - offset, offset);
                        expectedEdges.begin(48000);
                        offset = at;
                    }
                    expectedEdges.process(expectedBlock.block(), frames - offset, offset);
                    for (unsigned ch = 0; ch < 2; ++ch) for (unsigned i = 0; i < frames; ++i)
                        correct &= std::abs(realtime.block().data(ch)[i] - expectedBlock.block().data(ch)[i]) < 1e-7f;
                    renderedLeft.insert(renderedLeft.end(), realtime.l.begin(), realtime.l.end());
                    renderedRight.insert(renderedRight.end(), realtime.r.begin(), realtime.r.end());
                }
                // Start both render modes with the same DSP history instead of
                // seeking the same stateful player backwards twice per block.
                player->reset();
                for (SamplePos position = 0; position < 100; position += frames) {
                    correct &= bool(processor.processSerial(offline.block(), frames, position, true, true));
                    correct &= std::equal(offline.l.begin(), offline.l.end(), renderedLeft.begin() + position);
                    correct &= std::equal(offline.r.begin(), offline.r.end(), renderedRight.begin() + position);
                }
            }
        check(correct, "ordinary bounded clips preserve page crossings, trim ends, fades, pan and mono at 8/16/32/256 frames");
    }
    {
        ClipPlayerNode player;
        player.prepare({48000, 256, 2});
        auto sample = std::make_shared<SampleBuffer>(2, 256, 48000);
        for (unsigned ch = 0; ch < 2; ++ch) std::fill_n(sample->writableChannel(ch), 256, .001f);
        auto clips = std::make_shared<ClipPlayerNode::ClipList>();
        for (int i = 0; i < 100000; ++i) { ClipPlacement c; c.audio = sample; c.startSample = i * 512; c.lengthSamples = 256; clips->push_back(c); }
        for (int i = 0; i < 3000; ++i) { ClipPlacement c; c.audio = sample; c.startSample = 60000000; c.lengthSamples = 256; clips->push_back(c); }
        player.setClips(clips);
        Output output(256); ProcessContext c; c.output = output.block(); c.frames = 256; c.playing = true; c.offline = true;
        c.timelinePosition = 99999 * 512; player.process(c);
        check(output.l.back() == .001f, "seek through 100k past clips finds only intersecting material after the edge fade");
        c.timelinePosition = 60000000; player.process(c);
        float expected = 0; for (int i = 0; i < 3000; ++i) expected += .001f;
        check(output.l.back() == expected, "3000 overlapping clips preserve exact mixing order without overflow fallback");
        player.reset();
        c.timelinePosition = 256; player.process(c); check(dsp::isSilent(output.l), "interval index honours exclusive clip ends on a fresh seek");
    }
    {
        auto sample = std::make_shared<SampleBuffer>(2, 262144, 48000);
        for (unsigned ch = 0; ch < 2; ++ch) for (unsigned frame = 0; frame < sample->frames(); ++frame)
            sample->writableChannel(ch)[frame] = float((frame + ch) % 127) / 256.f;
        AudioGraph graph;
        auto player = std::make_shared<ClipPlayerNode>();
        auto clips = std::make_shared<ClipPlayerNode::ClipList>();
        ClipPlacement placement; placement.audio = sample; placement.lengthSamples = sample->frames();
        clips->push_back(placement); player->setClips(clips);
        const auto id = graph.adoptNode(player); graph.setSink(id);
        auto compiled = graph.compile({48000, 256, 2});
        GraphProcessor processor(2); processor.setGraph(*compiled);
        player->preparePlayback(200000);
        Output realtime(256), offline(256);
        processor.process(realtime.block(), 256, 200000, true, false);
        player->reset();
        processor.processSerial(offline.block(), 256, 200000, true, true);
        check(sample->fileBacked() && realtime.l == offline.l && realtime.r == offline.r,
            "mapped PCM seek is sample-identical in prepared realtime and direct offline paths");
    }
    {
        RealtimeEngine engine(2); engine.prepare(48000, 256, 2); Output output(256);
        { RealtimeEngine::RenderGate gate(engine); engine.renderBlock(output.block(), nullptr, 0, 256); }
        check(engine.gatedBlocks() == 1 && dsp::isSilent(output.l), "RenderGate silence is counted separately");
    }
    return failures ? 1 : 0;
}
