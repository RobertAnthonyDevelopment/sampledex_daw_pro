#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <vector>

#include "TimelineModel.h"
#include "Track.h"
#include "StreamingClipSource.h"

namespace sampledex
{
    struct TempoEvent
    {
        double beat = 0.0;
        double bpm = 120.0;
    };

    struct RealtimeStateSnapshot
    {
        std::vector<Clip> arrangement;
        std::vector<Track*> trackPointers;
        std::vector<TempoEvent> tempoEvents;
        std::vector<AutomationLane> automationLanes;
        int globalTransposeSemitones = 0;
        std::vector<std::shared_ptr<StreamingClipSource>> audioClipStreams;
    };

    class RealtimeSnapshotStateManager
    {
    public:
        using SnapshotPtr = std::shared_ptr<const RealtimeStateSnapshot>;

        void storeSnapshot(SnapshotPtr snapshot);
        SnapshotPtr getSnapshot() const;
        void clear();
        void drainRetiredSnapshots();

    private:
        void retireSnapshot(SnapshotPtr snapshot);

        std::shared_ptr<const RealtimeStateSnapshot> currentSnapshot;
        std::vector<SnapshotPtr> retiredSnapshots;
        juce::CriticalSection retiredSnapshotLock;
    };
}
