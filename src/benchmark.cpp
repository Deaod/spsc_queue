#include <benchmark/benchmark.h>
#include "platform.hpp"

#include <deaod/spsc_queue.hpp>
#include <readerwriterqueue/readerwriterqueue.h>
#include <rigtorp/SPSCQueue.h>
#include <dro/spsc-queue.hpp>

int main(int argc, char** argv) {
    prepare_process();

    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    ::benchmark::RunSpecifiedBenchmarks();
}

constexpr const size_t ItemsPerBatch = 10'000'000;

template<std::size_t N>
struct dummy {
    int idx;
    std::byte n[N-sizeof(idx)]{};
    dummy(int idx = 0) : idx(idx), n{} {}
};

template<>
struct dummy<sizeof(int)> {
    int idx;
    dummy(int idx = 0) : idx(idx) {}
};

static void configure_queue(benchmark::internal::Benchmark* bench) {
    bench->MeasureProcessCPUTime();
    bench->UseRealTime();
    bench->Threads(2);
    bench->Repetitions(20);
}
static constexpr std::size_t qs(std::size_t l2, std::size_t es) {
    return (std::size_t(1) << l2) / es;
}

#define QUEUE_BENCH_FOR_SIZES(F, T, ES, QS)                  \
    BENCHMARK_TEMPLATE(F, T<dummy<ES>, QS>)                  \
        ->Name(std::format("{}<{}<{},{}>>", #F, #T, ES, QS)) \
        ->Apply(configure_queue)

#define QUEUE_BENCH_FOR_SIZE(Func, Template, Size)             \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(12, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(13, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(14, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(15, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(16, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(17, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(18, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(19, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(20, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(21, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(22, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(23, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(24, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(25, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(26, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(27, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(28, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(29, Size)); \
    QUEUE_BENCH_FOR_SIZES(Func, Template, Size, qs(30, Size));

#define QUEUE_BENCH(Func, Template)          \
    QUEUE_BENCH_FOR_SIZE(Func, Template,  4) \
    QUEUE_BENCH_FOR_SIZE(Func, Template,  8) \
    QUEUE_BENCH_FOR_SIZE(Func, Template, 16) \
    QUEUE_BENCH_FOR_SIZE(Func, Template, 24) \
    QUEUE_BENCH_FOR_SIZE(Func, Template, 32) \
    QUEUE_BENCH_FOR_SIZE(Func, Template, 40) \
    QUEUE_BENCH_FOR_SIZE(Func, Template, 48) \
    QUEUE_BENCH_FOR_SIZE(Func, Template, 56) \
    QUEUE_BENCH_FOR_SIZE(Func, Template, 64)

template<typename T, size_t S>
struct moodycamel_adapter {
    using base_type = moodycamel::ReaderWriterQueue<T>;
    using value_type = T;
    static constexpr const size_t size = S;
};

template<typename T, size_t S>
struct rigtorp_adapter {
    using base_type = rigtorp::SPSCQueue<T>;
    using value_type = T;
    static constexpr const size_t size = S;
};

template<typename T, size_t S>
struct drogalis_adapter {
    using base_type = dro::SPSCQueue<T>;
    using value_type = T;
    static constexpr const size_t size = S;
};

template<typename type>
static void bench_emplace_pop_deaod(benchmark::State& state) {
    static std::atomic<type*> queue = nullptr;

    if (state.thread_index() == 0) {
        auto p = new type{};
        queue = p;
    } else {
        while (queue.load(std::memory_order_relaxed) == nullptr) {}
    }

    type& q = *queue;
    if (state.thread_index() == 0) {
        prepare_current_thread(Thread1Affinity);
        while (state.KeepRunningBatch(ItemsPerBatch)) {
            int counter = ItemsPerBatch;
            while (counter > 0) {
                counter -= int(q.emplace());
            }
        }
        state.SetItemsProcessed(state.iterations());
        state.SetBytesProcessed(state.iterations() * sizeof(typename type::value_type));
    } else if (state.thread_index() == 1) {
        prepare_current_thread(Thread2Affinity);
        typename type::value_type elem{};
        while (state.KeepRunningBatch(ItemsPerBatch)) {
            int counter = ItemsPerBatch;
            while (counter > 0) {
                counter -= int(q.pop(elem));
            }
        }
        state.SetItemsProcessed(state.iterations());
        state.SetBytesProcessed(state.iterations() * sizeof(typename type::value_type));

        delete queue;
        queue = nullptr;
    } else {
        prepare_current_thread(Thread2Affinity);
        while (queue.load() != nullptr) {}
    }
}

template<typename type>
static void bench_emplace_pop_moodycamel(benchmark::State& state) {
    static std::atomic<typename type::base_type*> queue = nullptr;

    if (state.thread_index() == 0) {
        auto p = new type::base_type{type::size};
        queue = p;
    } else {
        while (queue.load(std::memory_order_relaxed) == nullptr) {}
    }

    typename type::base_type& q = *queue;
    if (state.thread_index() == 0) {
        prepare_current_thread(Thread1Affinity);
        while (state.KeepRunningBatch(ItemsPerBatch)) {
            int counter = ItemsPerBatch;
            while (counter > 0) {
                counter -= int(q.try_emplace());
            }
        }
        state.SetItemsProcessed(state.iterations());
        state.SetBytesProcessed(state.iterations() * sizeof(typename type::value_type));
    } else if (state.thread_index() == 1) {
        prepare_current_thread(Thread2Affinity);
        typename type::value_type elem{};
        while (state.KeepRunningBatch(ItemsPerBatch)) {
            int counter = ItemsPerBatch;
            while (counter > 0) {
                counter -= int(q.try_dequeue(elem));
            }
        }
        state.SetItemsProcessed(state.iterations());
        state.SetBytesProcessed(state.iterations() * sizeof(typename type::value_type));

        delete queue;
        queue = nullptr;
    } else {
        prepare_current_thread(Thread2Affinity);
        while (queue.load() != nullptr) {}
    }
}

template<typename type>
static void bench_emplace_pop_rigtorp(benchmark::State& state) {
    static std::atomic<typename type::base_type*> queue = nullptr;

    if (state.thread_index() == 0) {
        auto p = new type::base_type{type::size};
        queue = p;
    } else {
        while (queue.load(std::memory_order_relaxed) == nullptr) {}
    }

    typename type::base_type& q = *queue;
    if (state.thread_index() == 0) {
        prepare_current_thread(Thread1Affinity);
        while (state.KeepRunningBatch(ItemsPerBatch)) {
            int counter = ItemsPerBatch;
            while (counter > 0) {
                counter -= int(q.try_emplace());
            }
        }
        state.SetItemsProcessed(state.iterations());
        state.SetBytesProcessed(state.iterations() * sizeof(typename type::value_type));
    } else if (state.thread_index() == 1) {
        prepare_current_thread(Thread2Affinity);
        while (state.KeepRunningBatch(ItemsPerBatch)) {
            int counter = ItemsPerBatch;
            while (counter > 0) {
                auto e = q.front();
                if (e) {
                    q.pop();
                    counter -= 1;
                }
            }
        }
        state.SetItemsProcessed(state.iterations());
        state.SetBytesProcessed(state.iterations() * sizeof(typename type::value_type));

        delete queue;
        queue = nullptr;
    } else {
        prepare_current_thread(Thread2Affinity);
        while (queue.load() != nullptr) {}
    }
}

template<typename type>
static void bench_emplace_pop_drogalis(benchmark::State& state) {
    static std::atomic<typename type::base_type*> queue = nullptr;

    if (state.thread_index() == 0) {
        auto p = new type::base_type{type::size};
        queue = p;
    } else {
        while (queue.load(std::memory_order_relaxed) == nullptr) {}
    }

    typename type::base_type& q = *queue;
    if (state.thread_index() == 0) {
        prepare_current_thread(Thread1Affinity);
        while (state.KeepRunningBatch(ItemsPerBatch)) {
            int counter = ItemsPerBatch;
            while (counter > 0) {
                counter -= int(q.try_emplace());
            }
        }
        state.SetItemsProcessed(state.iterations());
        state.SetBytesProcessed(state.iterations() * sizeof(typename type::value_type));
    } else if (state.thread_index() == 1) {
        prepare_current_thread(Thread2Affinity);
        typename type::value_type elem{};
        while (state.KeepRunningBatch(ItemsPerBatch)) {
            int counter = ItemsPerBatch;
            while (counter > 0) {
                counter -= int(q.try_pop(elem));
            }
        }
        state.SetItemsProcessed(state.iterations());
        state.SetBytesProcessed(state.iterations() * sizeof(typename type::value_type));

        delete queue;
        queue = nullptr;
    } else {
        prepare_current_thread(Thread2Affinity);
        while (queue.load() != nullptr) {}
    }
}

QUEUE_BENCH(bench_emplace_pop_deaod, deaod::spsc_queue);
QUEUE_BENCH(bench_emplace_pop_moodycamel, moodycamel_adapter);
QUEUE_BENCH(bench_emplace_pop_rigtorp, rigtorp_adapter);
QUEUE_BENCH(bench_emplace_pop_drogalis, drogalis_adapter);
