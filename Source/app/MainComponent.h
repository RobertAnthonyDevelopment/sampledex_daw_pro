#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <map>
#include <vector>
#include "Track.h"
#include "Mixer.h"
#include "TimelineModel.h"
#include "TransportEngine.h"
#include "TimelineComponent.h"
#include "PianoRollComponent.h"
#include "StepSequencerComponent.h"
#include "TransportBar.h"
#include "TrackList.h"
#include "TimelineView.h"
#include "BrowserPanel.h"
#include "MidiDeviceRouter.h"
#include "ChordEngine.h"
#include "ScheduledMidiOutput.h"
#include "StreamingClipSource.h"
#include "ProjectSerializer.h"
#include "RealtimeGraphScheduler.h"
#include "Theme.h"

namespace sampledex { class LcdDisplay; } 

namespace sampledex
{
    class MainComponent : public juce::AudioAppComponent,
                          public juce::MenuBarModel,
                          public juce::FileDragAndDropTarget, 
                          public juce::MidiInputCallback,    
                          public juce::AudioIODeviceCallback,
                          private juce::Timer 
    {
    public:
        MainComponent(bool startInSafeMode = false);
        ~MainComponent() override;

        void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
        void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
        void releaseResources() override;
        
        void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
        void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                              int numInputChannels,
                                              float* const* outputChannelData,
                                              int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext& context) override;
        void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
        void audioDeviceStopped() override;

        void paint(juce::Graphics& g) override;
        void resized() override;
        bool keyPressed(const juce::KeyPress& key) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void mouseMove(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent& e) override;
        
        juce::StringArray getMenuBarNames() override;
        juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
        void menuItemSelected(int menuID, int index) override;
        
        bool isInterestedInFileDrag (const juce::StringArray& files) override;
        void filesDropped (const juce::StringArray& files, int x, int y) override;
        
        void timerCallback() override;
        void requestApplicationClose();
        bool handleApplicationCloseRequest();

    private:
        enum class MidiLearnTarget : int
        {
            TrackVolume = 0,
            TrackPan,
            TrackSend,
            TrackMute,
            TrackSolo,
            TrackArm,
            TrackMonitor,
            MasterOutput,
            TransportPlay,
            TransportStop,
            TransportRecord
        };

        enum class ToolbarSection : int
        {
            Transport = 0,
            Timing = 1,
            AudioMidiIO = 2,
            Editing = 3,
            Render = 4,
            Utility = 5,
            Count = 6
        };

        enum class ToolbarProfile : int
        {
            Producer = 0,
            Recording = 1,
            Mixing = 2
        };

        struct ToolbarOverflowItem
        {
            juce::String label;
            std::function<void()> invoke;
        };

        struct MidiLearnMapping
        {
            juce::String sourceIdentifier;
            int channel = -1;
            int controller = -1;
            MidiLearnTarget target = MidiLearnTarget::TrackVolume;
            int trackIndex = -1;
            bool isToggle = false;
            int lastValue = -1;
        };

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

        struct ExportSettings
        {
            double sampleRate = 48000.0;
            int bitDepth = 24;
            bool loopRangeOnly = false;
            bool includeMasterProcessing = true;
            bool enableDither = true;
        };

        class RecordingDiskThread : public juce::Thread
        {
        public:
            explicit RecordingDiskThread(MainComponent& ownerRef)
                : juce::Thread("Sampledex Audio Record Disk Writer"), owner(ownerRef) {}

            void run() override
            {
                while (!threadShouldExit())
                {
                    owner.flushAudioTakeRingBuffers(false);
                    wait(4);
                }
                owner.flushAudioTakeRingBuffers(true);
            }

        private:
            MainComponent& owner;
        };

        struct AudioTakeWriterState
        {
            std::unique_ptr<juce::AudioFormatWriter> writer;
            juce::File file;
            bool active = false;
            int inputPair = -1;
            int64 samplesWritten = 0;
            double startBeat = 0.0;
            double sampleRate = 44100.0;
            int trackIndex = -1;
            std::vector<float> ringLeft;
            std::vector<float> ringRight;
            std::unique_ptr<juce::AbstractFifo> ringFifo;
            std::atomic<int64> droppedSamples { 0 };
            std::atomic<bool> hadWriteError { false };
            juce::String writeErrorMessage;
        };

        static constexpr int maxRealtimeTracks = 128;
        static constexpr int maxRealtimeBlockSize = 8192;
        static constexpr int auxBusCount = Track::maxSendBuses;
        static constexpr int monitorAnalyzerFftOrder = 11;
        static constexpr int monitorAnalyzerFftSize = 1 << monitorAnalyzerFftOrder;

        void scanForPlugins();
        void createNewTrack(Track::ChannelType type = Track::ChannelType::Instrument);
        void showAddTrackMenu();
        void showExportMenu();
        void showTempoMenu();
        void showClipToolsMenu();
        void showPluginListMenu(int trackIndex = -1,
                                juce::Component* targetComponent = nullptr,
                                int targetSlotIndex = Track::instrumentSlotIndex);
        void togglePlayback();
        void saveProject();
        void saveProjectAs();
        void openProject();
        void exportMixdown();
        void exportStems();
        void createMidiClipAt(int trackIndex, double startBeat, double lengthBeats, bool selectClip);
        int findClipIndex(const Clip* clip) const;
        void setSelectedClipIndex(int clipIndex, bool showMidiEditorsTab);
        void applyArrangementEdit(const juce::String& actionName, std::function<void(std::vector<Clip>&, int&)> mutator);
        void applyClipEdit(int clipIndex, const juce::String& actionName, std::function<void(Clip&)> mutator);
        double gridStepFromSelectorId(int selectorId) const;
        int selectorIdFromGridStep(double beats) const;
        void applyGridStep(double beats);
        void toggleRecord();
        void toggleMetronome();
        void toggleLoop();
        void showAudioSettings();
        void showProjectSettingsMenu();
        void showToolbarConfigMenu();
        void refreshAudioEngineSelectors();
        void applySampleRateSelection(int selectedId);
        void applyBufferSizeSelection(int selectedId);
        void applyLowLatencyMode(bool enabled);
        void applyToolbarProfile(ToolbarProfile profile, bool persistSettings);
        void setToolbarSectionVisible(ToolbarSection section, bool visible, bool persistSettings);
        bool isToolbarSectionVisible(ToolbarSection section) const;
        ToolbarProfile toolbarProfileFromString(const juce::String& value) const;
        juce::String toolbarProfileToString(ToolbarProfile profile) const;
        void loadToolbarLayoutSettings();
        void saveToolbarLayoutSettings() const;
        void showHelpGuide();
        void beginPluginScan();
        bool startPluginScanPass();
        void finishPluginScan(bool success, const juce::String& detailMessage);
        bool loadProjectFromFile(const juce::File& fileToLoad);
        void resetStreamingStateForProjectSwitch();
        void rebuildRealtimeSnapshot(bool markDirty = true);
        std::shared_ptr<const RealtimeStateSnapshot> getRealtimeSnapshot() const;
        void retireRealtimeSnapshot(std::shared_ptr<const RealtimeStateSnapshot> snapshot);
        void drainRetiredRealtimeSnapshots();
        void rebuildTempoEventMap();
        double getTempoAtBeat(double beat) const;
        void addTempoEvent(double beat, double tempoBpm);
        void removeTempoEventNear(double beat, double maxDistanceBeats);
        void clearTempoEvents();
        void jumpToPreviousTempoEvent();
        void jumpToNextTempoEvent();
        void updateDetectedSyncSource();
        void applyProjectScaleToEngines();
        void applyGlobalTransposeToSelection(int semitones);
        bool isSupportedAudioFile(const juce::File& file) const;
        bool isSupportedMidiFile(const juce::File& file) const;
        void collectDroppedFilesRecursively(const juce::File& candidate, juce::Array<juce::File>& output) const;
        bool importMidiFileToClip(const juce::File& file, int targetTrack, double startBeat, int& outClipIndex, double& outTempoBpm);
        bool importAudioFileToClip(const juce::File& file, int targetTrack, double startBeat, int& outClipIndex, double& outDetectedTempoBpm);
        double tryExtractTempoFromFilename(const juce::String& fileName) const;
        int resolveMidiImportTargetTrack(int preferredTrackIndex, bool createTrackIfNeeded);
        void ensureTrackHasPlayableInstrument(int trackIndex);
        void enqueuePreviewMidiEvent(int trackIndex, const juce::MidiMessage& message);
        void auditionPianoRollNote(int trackIndex, int noteNumber, int velocity);
        void normalizeSelectedAudioClip();
        void normalizeSelectedAudioClipWithOptions(float targetPeakDb, bool removeDc, bool preserveDynamics);
        void setSelectedAudioClipFades(double fadeInBeats, double fadeOutBeats);
        void adjustSelectedAudioClipGain(float multiplier);
        void setSelectedTrackIndex(int idx);
        void setTempoBpm(double newBpm);
        void syncTransportLoopFromUi();
        void updateRealtimeFlagsFromUi();
        void refreshMidiInputSelector();
        void applyMidiInputSelection(int selectedId);
        void refreshMidiOutputSelector();
        void applyMidiOutputSelection(int selectedId);
        void refreshControlSurfaceInputSelector();
        void applyControlSurfaceInputSelection(int selectedId);
        void refreshMidiDeviceCallbacks();
        juce::String getMidiSourceIdentifier(const juce::MidiInput* source) const;
        void handleControlSurfaceMidi(const juce::MidiMessage& message);
        bool isTrackScopedMidiLearnTarget(MidiLearnTarget target) const;
        juce::String midiLearnTargetToLabel(MidiLearnTarget target) const;
        MidiLearnTarget midiLearnTargetFromId(int selectedId) const;
        void clearMidiLearnArm();
        void armMidiLearnForSelectedTarget();
        void loadMidiLearnMappings();
        void saveMidiLearnMappings() const;
        void captureMidiLearnMapping(const juce::String& sourceIdentifier, const juce::MidiMessage& message);
        bool applyMappedMidiLearn(const juce::String& sourceIdentifier, const juce::MidiMessage& message);
        void applyMidiLearnTargetValue(const MidiLearnMapping& mapping, int value, bool risingEdge);
        void reorderTracks(int sourceTrackIndex, int targetTrackIndex);
        void renameTrack(int trackIndex);
        void duplicateTrack(int trackIndex);
        void deleteTrack(int trackIndex);
        void panicAllNotes();
        void openPluginEditorWindowForTrack(int trackIndex, int slotIndex = 0);
        void closePluginEditorWindow();
        void openEqWindowForTrack(int trackIndex);
        void closeEqWindow();
        void beginMixdownExportForFormat(const juce::String& formatExtension);
        void beginStemExportForFormat(const juce::String& formatExtension);
        bool promptForExportSettings(const juce::String& formatExtension,
                                     bool exportingStems,
                                     ExportSettings& outSettings);
        bool runOfflineExport(const juce::File& destination,
                              bool exportStems,
                              const juce::String& formatExtension,
                              double exportSampleRate,
                              int bitDepth,
                              bool loopRangeOnly,
                              bool includeMasterProcessing,
                              bool enableDither);
        bool renderOfflinePassToWriter(juce::AudioFormatWriter& writer,
                                       double startBeat,
                                       double endBeat,
                                       double exportSampleRate,
                                       int targetBitDepth,
                                       bool enableDither,
                                       std::atomic<bool>* cancelFlag = nullptr,
                                       std::function<void(float)> progressCallback = {});
        double getProjectEndBeat() const;
        juce::AudioFormat* findWritableExportFormatForExtension(const juce::String& extension) const;
        void refreshStatusText();
        void applyUiStyling();
        void moveTrackPluginSlot(int trackIndex, int fromSlot, int toSlot);
        void openChannelRackForTrack(int trackIndex);
        void closeChannelRackWindow();
        void refreshChannelRackWindow();
        void freezeTrackToAudio(int trackIndex);
        void unfreezeTrack(int trackIndex);
        void commitTrackToAudio(int trackIndex);
        bool renderTrackToAudioFile(int trackIndex,
                                    const juce::File& outputFile,
                                    double startBeat,
                                    double endBeat,
                                    double renderSampleRate,
                                    bool includeMasterProcessing,
                                    juce::String& errorMessage);
        void finishFreezeTrack(int trackIndex,
                               const juce::File& renderedFile,
                               double startBeat,
                               double endBeat,
                               double renderSampleRate);
        void finishCommitTrack(int trackIndex,
                               const juce::File& renderedFile,
                               double startBeat,
                               double endBeat,
                               double renderSampleRate);
        void runRenderTask(const juce::String& taskName,
                           std::function<void()> task,
                           int renderTrackIndex = -1,
                           Track::RenderTaskType taskType = Track::RenderTaskType::None);
        void cancelActiveRenderTask();
        void startCaptureRecordingNow();
        void startCaptureRecordingNow(double requestedStartBeat, int64_t requestedStartSample);
        void stopCaptureRecordingAndCommit(bool stopTransportAfterCommit);
        void startAudioTakeWriters(double startBeat);
        void stopAudioTakeWriters(double endBeat, std::vector<Clip>& destinationClips);
        void flushAudioTakeRingBuffers(bool flushAllPending);
        void applyAutomaticAudioCrossfades(std::vector<Clip>& state) const;
        int chooseBestInputPairForCurrentBlock(int capturedInputChannels, int blockSamples) const;
        void setMonitorSafeMode(bool enabled);
        void calibrateInputMonitoring();
        void clearInputPeakHolds();
        juce::String getLatencySummaryText() const;
        void refreshInputDeviceSafetyState();
        void applyFeedbackSafetyIfRequested();
        bool ensureMicrophonePermissionForInputUse(const juce::String& trigger);
        bool ensureAudioInputChannelsActive(const juce::String& trigger);
        bool ensureAudioInputReadyForMonitoring(const juce::String& trigger);
        bool requestTrackInputMonitoringState(int trackIndex, bool enable, const juce::String& trigger);
        void enforceStartupOutputOnlyMode();
        void logCloseDecision(const juce::String& decision) const;
        juce::String getPluginIdentity(const juce::PluginDescription& desc) const;
        bool isPluginQuarantined(const juce::PluginDescription& desc) const;
        void quarantinePlugin(const juce::PluginDescription& desc, const juce::String& reason);
        void unquarantinePlugin(const juce::PluginDescription& desc);
        void loadQuarantinedPlugins();
        void saveQuarantinedPlugins() const;
        bool runPluginIsolationProbe(const juce::PluginDescription& desc,
                                     bool instrumentPlugin,
                                     juce::String& errorMessage) const;
        int getPluginFormatRank(const juce::String& formatName) const;
        bool pluginDescriptionsShareIdentity(const juce::PluginDescription& a,
                                             const juce::PluginDescription& b) const;
        juce::Array<juce::PluginDescription> getPluginLoadCandidates(const juce::PluginDescription& requested,
                                                                      bool preferRequestedFormatFirst) const;
        juce::StringArray getPluginScanFormatsInPreferredOrder() const;
        void applyDeadMansPedalBlacklist(const juce::String& scanFormatName);
        void handlePluginScanPassResult(int exitCode, const juce::String& output, bool timedOut);
        void recordLastLoadedPlugin(const juce::PluginDescription& desc);
        bool readPluginSessionGuard(bool& cleanState, juce::PluginDescription& lastPlugin) const;
        void writePluginSessionGuard(bool cleanState) const;
        void handleUncleanPluginSessionRecovery();
        ProjectSerializer::ProjectState buildCurrentProjectState() const;
        bool saveProjectStateToFile(const juce::File& destination, juce::String& errorMessage) const;
        bool saveProjectToFile(const juce::File& destination, juce::String& errorMessage);
        void markProjectDirty();
        void maybeRunAutosave();
        void maybePromptRecoveryLoad();
        bool saveProjectSynchronously();
        void loadStartupPreferences();
        void saveStartupPreferences() const;
        void relinkMissingAudioFiles(std::vector<Clip>& clips,
                                     const juce::File& projectFile,
                                     juce::StringArray& warnings);
        void ensureDefaultAutomationLanesForTrack(int trackIndex);
        void ensureAutomationLaneIds();
        AutomationLane* findAutomationLane(AutomationTarget target, int trackIndex);
        const AutomationLane* findAutomationLane(AutomationTarget target, int trackIndex) const;
        AutomationMode getAutomationModeForTrackTarget(int trackIndex, AutomationTarget target) const;
        void setAutomationModeForTrackTarget(int trackIndex, AutomationTarget target, AutomationMode mode);
        float evaluateAutomationLaneValueAtBeat(const AutomationLane& lane, double beat) const;
        void enqueueAutomationWriteEvent(int laneId, double beat, float value);
        void drainAutomationWriteEvents();
        void resetAutomationLatchStates();
        void applyAutomationForBlock(const RealtimeStateSnapshot& snapshot,
                                     double beat,
                                     bool isPlaying);
        void ensureTrackPdcCapacity(int requiredSamples);
        void resetTrackPdcState();
        void applyTrackDelayCompensation(int trackIndex,
                                         int mainDelaySamples,
                                         int sendDelaySamples,
                                         int blockSamples,
                                         juce::AudioBuffer<float>& mainBuffer,
                                         juce::AudioBuffer<float>& sendBuffer);
        void recalculateAuxBusLatencyCache();
        int getAuxBusProcessingLatencySamples(int busIndex) const;
        bool sanitizeRoutingConfiguration(bool showAlert);

        juce::AudioPluginFormatManager formatManager;
        TransportBar transportBar;
        TrackList trackListView;
        TimelineView timelineView;
        BrowserPanel browserPanel;
        juce::AudioFormatManager audioFormatManager;
        std::unique_ptr<juce::ChildProcess> pluginScanProcess;
        juce::KnownPluginList knownPluginList;
        juce::File knownPluginListFile;
        juce::File startupSettingsFile;
        juce::File pluginScanDeadMansPedalFile;
        juce::File pluginSessionGuardFile;
        juce::File quarantinedPluginsFile;
        juce::File midiLearnMappingsFile;
        juce::File toolbarLayoutSettingsFile;
        juce::StringArray quarantinedPluginIds;
        juce::File appDataDir;
        juce::File autosaveProjectFile;
        bool recoveryPromptPending = false;
        bool safeModeStartup = false;
        bool autoScanPluginsOnStartup = true;
        bool autoQuarantineOnUncleanExit = true;
        bool micPermissionPromptedOnce = false;
        juce::String canonicalBuildPath = "/Users/robertclemons/Downloads/sampledex_daw-main/build/SampledexChordLab_artefacts/Release/Sampledex ChordLab.app";
        bool pluginSafetyGuardsEnabled = true;
        juce::String preferredMacPluginFormat = "AudioUnit";
        int pluginScanPassCount = 0;
        int pluginScanTotalPassCount = 0;
        int pluginScanPassTimeoutMs = 45000;
        double pluginScanProgress = 0.0;
        double scanPassStartTimeMs = 0.0;
        juce::StringArray pendingScanFormats;
        juce::String activeScanFormat;
        juce::StringArray pluginScanFailedItems;
        juce::StringArray pluginScanBlacklistedItems;
        juce::PluginDescription lastLoadedPluginDescription;
        bool hasLastLoadedPluginDescription = false;
        std::atomic<bool> closeRequestInProgress { false };
        std::atomic<bool> startupSafetyRampLoggedRt { false };
        std::atomic<bool> startupSafetyFaultLoggedRt { false };

        juce::OwnedArray<Track> tracks;
        int selectedTrackIndex = 0;
        int selectedClipIndex = -1;
        std::atomic<int> selectedTrackIndexRt { 0 };
        std::atomic<double> bpmRt { 120.0 };
        std::atomic<double> sampleRateRt { 44100.0 };
        std::atomic<bool> recordEnabledRt { false };
        std::atomic<bool> metronomeEnabledRt { false };
        std::atomic<bool> loopEnabledRt { false };
        std::atomic<float> masterOutputGainRt { 0.9f };
        std::atomic<bool> masterSoftClipEnabledRt { true };
        std::atomic<bool> masterLimiterEnabledRt { true };
        std::atomic<bool> outputDcHighPassEnabledRt { true };
        std::atomic<float> auxReturnGainRt { 0.5f };
        std::atomic<bool> auxFxEnabledRt { true };
        std::atomic<float> auxMeterRt { 0.0f };
        std::array<std::atomic<float>, static_cast<size_t>(auxBusCount)> auxBusMeterRt {};
        std::array<std::atomic<int>, static_cast<size_t>(auxBusCount)> auxBusInsertLatencyRt {};
        std::atomic<float> masterPeakMeterRt { 0.0f };
        std::atomic<float> masterRmsMeterRt { 0.0f };
        std::atomic<int> masterClipHoldRt { 0 };
        std::atomic<float> masterPhaseCorrelationRt { 0.0f };
        std::atomic<float> masterLoudnessLufsRt { -120.0f };
        std::atomic<int> activeInputChannelCountRt { 0 };
        std::atomic<float> liveInputPeakRt { 0.0f };
        std::atomic<float> liveInputRmsRt { 0.0f };
        std::atomic<bool> monitorSafeModeRt { true };
        std::atomic<float> inputMonitorSafetyTrimRt { 1.0f };
        std::atomic<bool> usingLikelyBuiltInAudioRt { false };
        std::atomic<bool> panicRequestedRt { false };
        std::atomic<bool> offlineRenderActiveRt { false };
        std::shared_ptr<const RealtimeStateSnapshot> realtimeSnapshot;
        std::vector<std::shared_ptr<const RealtimeStateSnapshot>> retiredRealtimeSnapshots;
        juce::CriticalSection retiredSnapshotLock;
        
        juce::MidiMessageCollector midiCollector;
        TransportEngine transport;
        std::vector<Clip> arrangement;
        std::vector<AutomationLane> automationLanes;
        juce::UndoManager undoManager;
        double bpm = 120.0;
        double gridStepBeats = 0.25;
        double recordingStartBeat = 0.0;
        int64_t recordingStartSample = 0;
        int recordingTakeCounter = 1;
        int recordCountInBars = 1;
        bool recordStartPending = false;
        double recordStartPendingBeat = 0.0;
        bool forcedMetronomeForCountIn = false;
        bool metronomeStateBeforeCountIn = false;
        bool stopTransportAfterAutoPunch = false;
        bool punchEnabled = false;
        double punchInBeat = 8.0;
        double punchOutBeat = 16.0;
        int preRollBars = 1;
        int postRollBars = 1;
        double autoStopAfterBeat = -1.0;
        bool recordOverdubEnabled = true;
        enum class InputMonitoringMode : int
        {
            ArmOnly = 0,
            MonitorOnly = 1,
            AutoMonitor = 2
        };
        InputMonitoringMode inputMonitoringMode = InputMonitoringMode::AutoMonitor;
        double recordingLatencyCompensationBeats = 0.0;
        int recordingManualOffsetSamples = 0;
        int recordingSafetyPreRollBlocks = 2;
        int recordingSafetyPreRollSamples = 0;
        std::atomic<int> recordingLatencyCompensationSamplesRt { 0 };
        std::atomic<double> recordingStartBeatRt { 0.0 };
        std::atomic<int64_t> recordingStartSampleRt { 0 };
        std::atomic<int> recordingStartOffsetSamplesRt { 0 };
        std::atomic<int> recordingSafetyPreRollSamplesRt { 0 };

        std::array<juce::Reverb, static_cast<size_t>(auxBusCount)> auxReverbs;
        juce::Reverb::Parameters reverbParams;
        bool metronomeEnabled = false;
        float masterOutputGain = 0.9f;
        bool masterSoftClipEnabled = true;
        bool masterLimiterEnabled = true;
        bool outputDcHighPassEnabled = true;
        float auxReturnGain = 0.5f;
        bool auxFxEnabled = true;

        std::unique_ptr<LcdDisplay> lcdDisplay;
        std::unique_ptr<juce::TooltipWindow> tooltipWindow;
        
        juce::TextButton playButton { "Play" };
        juce::TextButton stopButton { "Stop" };
        juce::TextButton recordButton { "Rec" };
        juce::TextButton panicButton { "Panic" };
        juce::TextButton loopButton { "Loop" };
        juce::TextButton metroButton { "Click" };
        juce::TextButton tempoMenuButton { "Tempo" };
        juce::TextButton clipToolsButton { "Clip Tools" };
        juce::TextButton transportStartButton { "|<" };
        juce::TextButton transportPrevBarButton { "< Bar" };
        juce::TextButton transportNextBarButton { "Bar >" };
        juce::ToggleButton followPlayheadButton { "Follow" };
        juce::TextButton undoButton { "Undo" };
        juce::TextButton redoButton { "Redo" };
        juce::TextButton timelineZoomOutButton { "-" };
        juce::TextButton timelineZoomInButton { "+" };
        juce::TextButton trackZoomOutButton { "Rows-" };
        juce::TextButton trackZoomInButton { "Rows+" };
        juce::TextButton resetZoomButton { "Zoom Reset" };
        juce::Slider timelineZoomSlider;
        juce::Slider trackZoomSlider;
        juce::ComboBox keySelector;
        juce::ComboBox scaleSelector;
        juce::ComboBox transposeSelector;
        
        juce::TextButton scanButton { "Scan Plugins" };
        juce::TextButton addTrackButton { "Add Track" };
        juce::TextButton showEditorButton { "Show UI" };
        juce::TextButton freezeButton { "Freeze" };
        juce::TextButton rackButton { "Rack" };
        juce::TextButton inspectorButton { "Inspect" };
        juce::TextButton recordSetupButton { "Rec Setup" };
        juce::TextButton projectButton { "Project" };
        juce::TextButton toolbarButton { "Toolbar" };
        juce::TextButton toolbarMoreButton { "More" };
        juce::TextButton settingsButton { "Audio" };
        juce::TextButton helpButton { "Help" };
        juce::TextButton exportButton { "Export" };
        juce::TextButton saveButton { "Save" };
        juce::ToggleButton auxEnableButton { "Aux FX" };
        juce::ToggleButton softClipButton { "Soft Clip" };
        juce::ToggleButton limiterButton { "Limiter" };
        juce::Label masterOutLabel;
        juce::Slider masterOutSlider;
        std::unique_ptr<juce::Component> masterMeterWidget;
        juce::Label auxReturnLabel;
        juce::Slider auxReturnSlider;
        juce::ComboBox gridSelector;
        juce::ComboBox sampleRateSelector;
        juce::ComboBox bufferSizeSelector;
        juce::ComboBox midiInputSelector;
        juce::ComboBox midiOutputSelector;
        juce::ComboBox controlSurfaceInputSelector;
        juce::ComboBox midiLearnTargetSelector;
        juce::ToggleButton midiLearnArmToggle { "MIDI Learn" };
        juce::ToggleButton midiThruToggle { "MIDI Thru" };
        juce::ToggleButton externalClockToggle { "Ext Clock" };
        juce::ToggleButton lowLatencyToggle { "Low Lat" };
        juce::ToggleButton backgroundRenderToggle { "BG Render" };
        juce::ProgressBar pluginScanStatusBar { pluginScanProgress };
        juce::Label pluginScanStatusLabel;
        juce::Label statusLabel;
        juce::StringArray midiInputDeviceIds;
        juce::StringArray midiOutputDeviceIds;
        juce::StringArray controlSurfaceInputDeviceIds;
        juce::String activeMidiInputIdentifier;
        juce::String activeMidiOutputIdentifier;
        juce::String activeControlSurfaceInputIdentifier;
        int projectKeyRoot = 0;
        int projectScaleMode = 0;
        int projectTransposeSemitones = 0;
        std::vector<TempoEvent> tempoEvents;
        mutable juce::CriticalSection midiDeviceSelectionLock;
        mutable juce::CriticalSection midiLearnLock;
        std::vector<MidiLearnMapping> midiLearnMappings;
        std::array<int, 128> controlSurfaceLastCcValue {};
        bool midiLearnArmed = false;
        MidiLearnTarget pendingMidiLearnTarget = MidiLearnTarget::TrackVolume;
        int pendingMidiLearnTrackIndex = -1;
        std::atomic<int> globalTransposeRt { 0 };
        std::atomic<double> cpuUsageRt { 0.0 };
        std::atomic<int> audioGuardDropCountRt { 0 };
        std::atomic<int> audioXrunCountRt { 0 };
        std::atomic<int> audioCallbackOverloadCountRt { 0 };
        std::atomic<float> audioCallbackLoadRt { 0.0f };
        std::atomic<int> xrunRecoveryBlocksRt { 0 };
        std::atomic<double> lastMidiClockMessageMs { -1.0 };
        std::atomic<double> lastMtcMessageMs { -1.0 };
        std::atomic<bool> midiOutputActiveRt { false };
        std::atomic<bool> midiThruEnabledRt { false };
        bool externalMidiClockSyncEnabled = false;
        std::atomic<bool> externalMidiClockSyncEnabledRt { false };
        std::atomic<bool> externalMidiClockTransportRunningRt { false };
        std::atomic<bool> externalMidiClockActiveRt { false };
        std::atomic<double> externalMidiClockBeatRt { 0.0 };
        std::atomic<double> externalMidiClockBeatOffsetRt { 0.0 };
        std::atomic<double> externalMidiClockTempoRt { 120.0 };
        std::atomic<double> externalMidiClockLastTickMsRt { -1.0 };
        std::atomic<int> externalMidiClockTickCounterRt { 0 };
        std::atomic<int64_t> externalMidiClockGenerationRt { 0 };
        bool externalMidiClockWasRunning = false;
        int64_t lastAppliedExternalClockGeneration = -1;
        bool backgroundRenderingEnabled = true;
        std::atomic<bool> backgroundRenderingEnabledRt { true };
        bool lowLatencyMode = false;
        std::atomic<bool> lowLatencyModeRt { false };
        std::atomic<bool> backgroundRenderBusyRt { false };
        std::atomic<bool> renderCancelRequestedRt { false };
        std::atomic<float> renderProgressRt { 0.0f };
        std::atomic<int> renderTrackIndexRt { -1 };
        std::atomic<int> renderTaskTypeRt { static_cast<int>(Track::RenderTaskType::None) };
        std::atomic<bool> builtInMicHardFailSafeRt { false };
        std::atomic<bool> builtInMonitorSafetyNoticeRequestedRt { false };
        std::atomic<int> startupSafetyBlocksRemainingRt { 0 };
        std::atomic<int> startupOutputRampSamplesRemainingRt { 0 };
        std::atomic<int> startupOutputRampTotalSamplesRt { 1 };
        std::atomic<int> feedbackHazardBlocksRt { 0 };
        std::atomic<bool> feedbackAutoMuteRequestedRt { false };
        std::atomic<int> outputSafetyMuteBlocksRt { 0 };
        juce::ThreadPool backgroundRenderPool { 1 };
        int highCpuFrameCount = 0;
        bool feedbackWarningPending = false;
        std::atomic<int> recordStartRequestRt { 0 };
        std::atomic<int> recordStopRequestRt { 0 };
        std::atomic<bool> recordStartPendingRt { false };
        std::atomic<double> recordStartPendingBeatRt { 0.0 };
        std::atomic<double> autoStopAfterBeatRt { -1.0 };
        bool builtInMonitorSafetyNoticeShown = false;

        TimelineComponent timeline { transport, arrangement, tracks };
        juce::TabbedComponent bottomTabs { juce::TabbedButtonBar::TabsAtBottom };
        Mixer mixer;
        PianoRollComponent pianoRoll;
        StepSequencerComponent stepSequencer;

        std::unique_ptr<juce::DocumentWindow> pluginEditorWindow;
        int pluginEditorTrackIndex = -1;
        int pluginEditorSlotIndex = Track::instrumentSlotIndex;
        uint64_t pluginEditorWindowToken = 0;
        std::unique_ptr<juce::DocumentWindow> eqWindow;
        int eqTrackIndex = -1;
        uint64_t eqWindowToken = 0;
        std::unique_ptr<juce::DocumentWindow> channelRackWindow;
        int channelRackTrackIndex = -1;
        uint64_t channelRackWindowToken = 0;
        std::unique_ptr<juce::FileChooser> exportFileChooser;
        std::unique_ptr<juce::FileChooser> projectFileChooser;
        std::unique_ptr<juce::FileChooser> samplerFileChooser;
        juce::File currentProjectFile;
        static constexpr int channelRackSlotCount = 4;
        juce::Component* dockedChannelRack = nullptr;
        juce::Component* trackInspectorView = nullptr;
        juce::Component* recordingPanelView = nullptr;
        bool monitorSafeMode = true;
        ToolbarProfile activeToolbarProfile = ToolbarProfile::Producer;
        std::array<bool, static_cast<size_t>(ToolbarSection::Count)> toolbarSectionVisibility { true, true, true, true, true, true };
        std::vector<ToolbarOverflowItem> toolbarOverflowItems;
        
        // --- CHORD ENGINE INTEGRATION ---
        ChordEngine chordEngine;
        ScheduledMidiOutput midiScheduler;
        MidiDeviceRouter midiRouter;

        // --- REAL-TIME BUFFERS (Pre-allocated) ---
        juce::AudioBuffer<float> tempMixingBuffer;
        std::array<juce::AudioBuffer<float>, static_cast<size_t>(auxBusCount)> auxBusBuffers;
        juce::AudioBuffer<float> trackTempAudio;
        juce::AudioBuffer<float> trackInputAudio;
        juce::AudioBuffer<float> audioStreamScratch;
        juce::AudioBuffer<float> liveInputCaptureBuffer;
        std::array<std::array<float, monitorAnalyzerFftSize>, 2> masterAnalyzerSnapshots {};
        std::array<float, monitorAnalyzerFftSize> masterAnalyzerBuildBuffer {};
        std::atomic<int> masterAnalyzerReadySnapshot { -1 };
        int masterAnalyzerWriteSnapshot = 0;
        int masterAnalyzerBuildPos = 0;
        std::array<juce::AudioBuffer<float>, 2> inputTapBuffers;
        std::array<std::atomic<int>, 2> inputTapNumChannels { 0, 0 };
        std::array<std::atomic<int>, 2> inputTapNumSamples { 0, 0 };
        std::atomic<int> inputTapReadyIndex { -1 };
        RealtimeGraphScheduler realtimeGraphScheduler;
        std::array<juce::MidiBuffer, static_cast<size_t>(maxRealtimeTracks)> trackMidiBuffers;
        std::array<juce::MidiBuffer, static_cast<size_t>(maxRealtimeTracks)> previewMidiBuffers;
        juce::SpinLock previewMidiBuffersLock;
        juce::MidiBuffer liveMidiBuffer;
        juce::MidiBuffer chordEngineOutputBuffer;
        RecordingDiskThread audioRecordDiskThread { *this };
        juce::TimeSliceThread streamingAudioReadThread { "Sampledex Streaming Reader" };
        std::map<juce::String, std::shared_ptr<StreamingClipSource>> streamingClipCache;
        std::array<AudioTakeWriterState, static_cast<size_t>(maxRealtimeTracks)> audioTakeWriters;
        float masterGainSmoothingState = 0.9f;
        float masterGainDezipperCoeff = 0.0f;
        std::array<float, 2> outputDcPrevInput { 0.0f, 0.0f };
        std::array<float, 2> outputDcPrevOutput { 0.0f, 0.0f };
        std::array<float, 2> masterLimiterPrevInput { 0.0f, 0.0f };
        std::array<float, 2> masterTruePeakMidpointPrevInput { 0.0f, 0.0f };
        float masterLimiterGainState = 1.0f;
        bool wasTransportPlayingLastBlock = false;
        std::array<juce::AudioBuffer<float>, static_cast<size_t>(maxRealtimeTracks)> trackMainWorkBuffers;
        std::array<juce::AudioBuffer<float>, static_cast<size_t>(maxRealtimeTracks)> trackTimelineWorkBuffers;
        std::array<juce::AudioBuffer<float>, static_cast<size_t>(maxRealtimeTracks)> trackSendWorkBuffers;
        std::array<juce::AudioBuffer<float>, static_cast<size_t>(maxRealtimeTracks)> trackInputWorkBuffers;
        bool allowDirtyTracking = false;
        bool suppressDirtyTracking = false;
        bool projectDirty = false;
        uint64_t projectMutationSerial = 0;
        uint64_t lastAutosaveSerial = 0;
        juce::String lastAudioDeviceNameSeen;
        int nextAutomationLaneId = 1;

        struct AutomationWriteEvent
        {
            int laneId = 0;
            double beat = 0.0;
            float value = 0.0f;
        };
        static constexpr int automationWriteQueueCapacity = 32768;
        std::array<AutomationWriteEvent, static_cast<size_t>(automationWriteQueueCapacity)> automationWriteQueue;
        std::atomic<int> automationWriteReadIndex { 0 };
        std::atomic<int> automationWriteWriteIndex { 0 };
        std::atomic<int> droppedAutomationWriteEvents { 0 };
        std::array<std::array<std::atomic<bool>, 3>, static_cast<size_t>(maxRealtimeTracks)> automationTouchStateRt {};
        std::array<std::array<std::atomic<bool>, 3>, static_cast<size_t>(maxRealtimeTracks)> automationLatchStateRt {};
        std::atomic<bool> masterAutomationTouchRt { false };
        std::atomic<bool> masterAutomationLatchRt { false };

        juce::AudioBuffer<float> trackSendAudio;
        juce::AudioBuffer<float> trackPdcScratchBuffer;
        std::array<juce::AudioBuffer<float>, static_cast<size_t>(maxRealtimeTracks)> trackPdcScratchBuffers;
        std::array<juce::AudioBuffer<float>, static_cast<size_t>(maxRealtimeTracks)> trackPdcDelayBuffers;
        std::array<int, static_cast<size_t>(maxRealtimeTracks)> trackPdcWritePositions {};
        int trackPdcBufferSamples = 0;
        std::atomic<int> maxPdcLatencySamplesRt { 0 };

        float bottomPanelRatio = 0.56f;
        float browserPanelRatio = 0.18f;
        bool draggingBottomSplitter = false;
        bool draggingBrowserSplitter = false;
        int splitterDragMouseOffset = 0;
        juce::Rectangle<int> bottomSplitterBounds;
        juce::Rectangle<int> browserSplitterBounds;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
    };
}
