#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace sampledex
{
    class RealtimeGraphScheduler final
    {
    public:
        using JobFn = void(*)(void*, int);

        RealtimeGraphScheduler() = default;
        ~RealtimeGraphScheduler()
        {
            shutdown();
        }

        void setWorkerCount(int requestedWorkers)
        {
            const int clampedWorkers = juce::jlimit(0, maxWorkerCount, requestedWorkers);
            if (clampedWorkers == getWorkerCount())
                return;

            shutdown();
            if (clampedWorkers <= 0)
                return;

            workers.reserve(static_cast<size_t>(clampedWorkers));
            for (int i = 0; i < clampedWorkers; ++i)
            {
                auto worker = std::make_unique<Worker>(*this);
                worker->thread = std::thread([rawWorker = worker.get()]
                {
                    rawWorker->run();
                });
                workers.push_back(std::move(worker));
            }
        }

        int getWorkerCount() const noexcept
        {
            return static_cast<int>(workers.size());
        }

        void run(int jobCount, void* context, JobFn jobFn) noexcept
        {
            if (jobCount <= 0 || context == nullptr || jobFn == nullptr)
                return;

            const int workerCount = getWorkerCount();
            if (workerCount <= 0 || jobCount <= 1)
            {
                for (int i = 0; i < jobCount; ++i)
                    jobFn(context, i);
                return;
            }

            nextJobIndex.store(0, std::memory_order_release);
            totalJobs.store(jobCount, std::memory_order_release);
            activeContext.store(context, std::memory_order_release);
            activeJobFn.store(jobFn, std::memory_order_release);
            completedWorkers.store(0, std::memory_order_release);

            const uint64_t generation = dispatchGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
            for (auto& worker : workers)
            {
                worker->requestedGeneration.store(generation, std::memory_order_release);
                worker->startEvent.signal();
            }

            processJobs();

            while (completedWorkers.load(std::memory_order_acquire) < workerCount)
                workerDoneEvent.wait(1);
        }

    private:
        struct Worker
        {
            explicit Worker(RealtimeGraphScheduler& ownerIn)
                : owner(ownerIn)
            {
            }

            void run() noexcept
            {
                uint64_t lastGeneration = 0;
                while (!owner.shutdownRequested.load(std::memory_order_acquire))
                {
                    startEvent.wait();
                    if (owner.shutdownRequested.load(std::memory_order_acquire))
                        break;

                    const uint64_t generation = requestedGeneration.load(std::memory_order_acquire);
                    if (generation == lastGeneration)
                        continue;

                    lastGeneration = generation;
                    owner.processJobs();
                    owner.completedWorkers.fetch_add(1, std::memory_order_acq_rel);
                    owner.workerDoneEvent.signal();
                }
            }

            RealtimeGraphScheduler& owner;
            juce::WaitableEvent startEvent;
            std::atomic<std::uint64_t> requestedGeneration { 0 };
            std::thread thread;
        };

        void processJobs() noexcept
        {
            const int jobCount = totalJobs.load(std::memory_order_acquire);
            auto* context = activeContext.load(std::memory_order_acquire);
            auto fn = activeJobFn.load(std::memory_order_acquire);
            if (jobCount <= 0 || context == nullptr || fn == nullptr)
                return;

            while (true)
            {
                const int index = nextJobIndex.fetch_add(1, std::memory_order_acq_rel);
                if (index >= jobCount)
                    break;
                fn(context, index);
            }
        }

        void shutdown() noexcept
        {
            if (workers.empty())
                return;

            shutdownRequested.store(true, std::memory_order_release);
            for (auto& worker : workers)
                worker->startEvent.signal();

            for (auto& worker : workers)
            {
                if (worker->thread.joinable())
                    worker->thread.join();
            }
            workers.clear();

            shutdownRequested.store(false, std::memory_order_release);
            nextJobIndex.store(0, std::memory_order_relaxed);
            totalJobs.store(0, std::memory_order_relaxed);
            activeContext.store(nullptr, std::memory_order_relaxed);
            activeJobFn.store(nullptr, std::memory_order_relaxed);
            completedWorkers.store(0, std::memory_order_relaxed);
            dispatchGeneration.store(0, std::memory_order_relaxed);
        }

        static constexpr int maxWorkerCount = 8;
        std::vector<std::unique_ptr<Worker>> workers;
        std::atomic<bool> shutdownRequested { false };
        std::atomic<int> nextJobIndex { 0 };
        std::atomic<int> totalJobs { 0 };
        std::atomic<void*> activeContext { nullptr };
        std::atomic<JobFn> activeJobFn { nullptr };
        std::atomic<int> completedWorkers { 0 };
        std::atomic<std::uint64_t> dispatchGeneration { 0 };
        juce::WaitableEvent workerDoneEvent;
    };
}
