#include "RealtimeStateSnapshot.h"

namespace sampledex
{
    void RealtimeSnapshotStateManager::storeSnapshot(SnapshotPtr snapshot)
    {
        auto previousSnapshot = std::atomic_exchange_explicit(&currentSnapshot,
                                                              std::move(snapshot),
                                                              std::memory_order_acq_rel);
        if (previousSnapshot != nullptr)
            retireSnapshot(std::move(previousSnapshot));
    }

    RealtimeSnapshotStateManager::SnapshotPtr RealtimeSnapshotStateManager::getSnapshot() const
    {
        return std::atomic_load_explicit(&currentSnapshot, std::memory_order_acquire);
    }

    void RealtimeSnapshotStateManager::clear()
    {
        auto previousSnapshot = std::atomic_exchange_explicit(&currentSnapshot,
                                                              SnapshotPtr {},
                                                              std::memory_order_acq_rel);
        if (previousSnapshot != nullptr)
            retireSnapshot(std::move(previousSnapshot));

        const juce::ScopedLock sl(retiredSnapshotLock);
        retiredSnapshots.clear();
    }

    void RealtimeSnapshotStateManager::retireSnapshot(SnapshotPtr snapshot)
    {
        if (snapshot == nullptr)
            return;

        const juce::ScopedLock sl(retiredSnapshotLock);
        retiredSnapshots.push_back(std::move(snapshot));
    }

    void RealtimeSnapshotStateManager::drainRetiredSnapshots()
    {
        std::vector<SnapshotPtr> releasableSnapshots;
        {
            const juce::ScopedLock sl(retiredSnapshotLock);
            if (retiredSnapshots.empty())
                return;

            std::vector<SnapshotPtr> survivors;
            survivors.reserve(retiredSnapshots.size());
            releasableSnapshots.reserve(retiredSnapshots.size());
            for (auto& snapshot : retiredSnapshots)
            {
                if (snapshot != nullptr && snapshot.use_count() > 1)
                    survivors.push_back(std::move(snapshot));
                else
                    releasableSnapshots.push_back(std::move(snapshot));
            }

            retiredSnapshots.swap(survivors);
        }

        for (auto& snapshot : releasableSnapshots)
            snapshot.reset();
    }
}
