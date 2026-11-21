#pragma once

#include "ChordDetector.h"
#include "MidiEvent.h"
#include "StyleTimeline.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <atomic>

class LiveControls
{
public:
    enum Command { startStop, mainA, mainB, intro, fill, ending, mainC, breakBar, tempoUp, tempoDown, commandCount };

    LiveControls()
    {
        for (auto& binding : bindings)
            binding.store(-1, std::memory_order_relaxed);
    }

    int bpm() const noexcept { return tempo.load(std::memory_order_relaxed); }

    void setBinding(Command command, std::optional<MidiTrigger> trigger) noexcept
    {
        bindings[command].store(trigger ? encode(*trigger) : -1, std::memory_order_relaxed);
    }

    void command(Command command) noexcept
    {
        if (command == tempoUp || command == tempoDown)
        {
            auto previous = bpm();
            const int delta = command == tempoUp ? 1 : -1;
            while (!tempo.compare_exchange_weak(previous, std::clamp(previous + delta, 40, 240),
                                                 std::memory_order_relaxed)) {}
            return;
        }
        if (command == mainA || command == mainB || command == mainC)
        {
            const auto section = command == mainA ? mainASection : command == mainB ? mainBSection : mainCSection;
            requested.store(section, std::memory_order_relaxed);
            return;
        }

        auto previous = transport.load(std::memory_order_relaxed);
        int next;
        do
        {
            if (command == startStop)
                next = (previous & playingBit) == 0 ? playingBit : 0;
            else if (command == intro)
                next = playingBit | (previous & immediateFillBit) | (introSection << sectionShift);
            else if ((previous & playingBit) == 0)
                return;
            else if (command == fill)
                next = previous | immediateFillBit;
            else
            {
                const auto section = command == breakBar ? breakSection : endingSection;
                next = playingBit | (previous & immediateFillBit) | (section << sectionShift);
            }
        }
        while (!transport.compare_exchange_weak(previous, next, std::memory_order_relaxed));
    }

    bool isPlaying() const noexcept { return (transport.load(std::memory_order_relaxed) & playingBit) != 0; }
    int queuedSection() const noexcept { return transport.load(std::memory_order_relaxed) >> sectionShift; }
    void stop() noexcept { transport.store(0, std::memory_order_relaxed); }

    bool takeImmediateFill() noexcept
    {
        if ((transport.load(std::memory_order_relaxed) & immediateFillBit) == 0)
            return false;
        const auto state = transport.fetch_and(~immediateFillBit, std::memory_order_relaxed);
        return (state & (playingBit | immediateFillBit)) == (playingBit | immediateFillBit);
    }

    int takeQueuedSection() noexcept
    {
        const auto state = transport.fetch_and(playingBit | immediateFillBit, std::memory_order_relaxed);
        if ((state & playingBit) == 0)
            return -1;
        return state >> sectionShift;
    }

    using HeldNotes = std::array<std::uint32_t, 4>;

    HeldNotes heldNotes() const noexcept
    {
        HeldNotes result {};
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = held[i].load(std::memory_order_relaxed);
        return result;
    }

    void handle(const MidiEvent& event) noexcept
    {
        const auto status = event.bytes[0];
        const auto type = status & 0xf0;
        const auto number = event.bytes[1];
        if (event.size == 3 && number < 128)
        {
            if (type == 0x80 || type == 0x90)
            {
                detector.note(status & 15, number, type == 0x90 && event.bytes[2] != 0);
                const auto bit = std::uint32_t { 1 } << (number % 32);
                auto& word = held[number / 32];
                const auto previous = word.load(std::memory_order_relaxed);
                word.store(detector.isHeld(number) ? previous | bit : previous & ~bit,
                           std::memory_order_relaxed);
            }
            else if (type == 0xb0 && (number == 120 || number >= 123))
            {
                detector.releaseChannel(status & 15);
                for (std::size_t i = 0; i < held.size(); ++i)
                {
                    std::uint32_t word = 0;
                    for (int bit = 0; bit < 32; ++bit)
                        if (detector.isHeld(static_cast<int>(i) * 32 + bit))
                            word |= std::uint32_t { 1 } << bit;
                    held[i].store(word, std::memory_order_relaxed);
                }
            }
            chord.store(detector.chord(), std::memory_order_relaxed);
        }

        if (learning.load(std::memory_order_acquire) != 0)
            return;
        if (const auto trigger = MidiTrigger::from(event))
        {
            const auto key = encode(*trigger);
            if (trigger->isTransport())
            {
                if (bindings[startStop].load(std::memory_order_relaxed) == key)
                {
                    if (status == 0xfc)
                        stop();
                    else
                        transport.fetch_or(playingBit, std::memory_order_relaxed);
                }
                return;
            }
            for (int i = 0; i < commandCount; ++i)
            {
                if (bindings[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) == key)
                {
                    command(static_cast<Command>(i));
                    break;
                }
            }
        }
    }

    // Call after closing the MIDI input and joining its in-flight callback.
    void resetNotes() noexcept
    {
        detector = {};
        for (auto& word : held)
            word.store(0, std::memory_order_relaxed);
        chord.store(-1, std::memory_order_relaxed);
    }

    static_assert(std::atomic<int>::is_always_lock_free);
    std::atomic<int> chord { -1 };
    std::atomic<int> requested { 0 };
    std::atomic<int> learning { 0 };
    std::atomic<int> recording { 0 };

private:
    static int encode(MidiTrigger trigger) noexcept { return (trigger.status << 8) | trigger.number; }

    static constexpr int playingBit = 1;
    static constexpr int immediateFillBit = 2;
    static constexpr int sectionShift = 2;
    std::atomic<int> transport { 0 };
    std::atomic<int> tempo { 108 };
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::array<std::atomic<std::uint32_t>, 4> held {};
    ChordDetector detector;
    std::array<std::atomic<int>, commandCount> bindings {};
};

class Arranger final : public juce::AudioIODeviceCallback
{
public:
    explicit Arranger(LiveControls& controls) : live(controls)
    {
        replaceTimeline(StyleTimeline::starter());
    }

    // Replacing the source data requires a disconnected audio callback.
    void replaceTimeline(const StyleTimeline& timeline);
    void prepareRecording(BarRange range, int track);
    double recordingEpoch() const noexcept { return recordEpoch.load(std::memory_order_acquire); }
    bool recordingEnded() const noexcept
    {
        const auto end = recordEndEpoch.load(std::memory_order_acquire);
        return end > 0 && juce::Time::getMillisecondCounterHiRes() >= end;
    }

    bool isCountingIn() const noexcept
    {
        const auto start = recordingEpoch();
        return start == 0 || juce::Time::getMillisecondCounterHiRes() < start;
    }

    void setRange(std::size_t sectionIndex, BarRange range) noexcept
    {
        if (sectionIndex < ranges.size() && range.first >= 0 && range.last >= range.first
            && range.last < static_cast<int>(bars.size()))
            ranges[sectionIndex].store(range.first * StyleTimeline::maxBars + range.last,
                                       std::memory_order_relaxed);
    }

    int timelineBar() const noexcept { return visibleBar.load(std::memory_order_relaxed); }
    int timelineTick() const noexcept { return visibleTick.load(std::memory_order_relaxed); }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs,
                                         int channels, int samples,
                                         const juce::AudioIODeviceCallbackContext&) override;
    int currentBeat() const noexcept { return beat.load(std::memory_order_relaxed); }
    int activeSection() const noexcept { return active.load(std::memory_order_relaxed); }

private:
    void reset() noexcept;
    void enterSection(int next) noexcept;
    void beginBar(bool starting) noexcept;
    void beginStep(int step) noexcept;
    void updateTimelineBar(bool contiguous) noexcept;
    void playRecordedEvents(double tick) noexcept;
    void beginRecordingBar(bool starting, double outputTimeMs) noexcept;
    void triggerClick(int step) noexcept;
    bool trackEnabled(int track) const noexcept;
    float nextSample() noexcept;

    LiveControls& live;
    struct PlaybackEvent { int tick, track, pitch, velocity, id; };

    std::vector<StyleBar> bars;
    std::array<std::vector<PlaybackEvent>, StyleTimeline::maxBars> events;
    std::size_t eventCursor = 0;
    int bassVoice = -1;
    std::array<float, 128> noteRates {};

    BarRange recordingRange { 0, 0 };
    int recordingTrack = drumsTrack;
    bool renderingRecording = false;
    bool countingIn = false;
    static_assert(std::atomic<double>::is_always_lock_free);
    std::atomic<double> recordEpoch { 0 };
    std::atomic<double> recordEndEpoch { 0 };
    std::array<std::atomic<int>, sectionCount> ranges {};
    std::atomic<int> visibleBar { -1 };
    std::atomic<int> visibleTick { -1 };
    BarRange sectionRange { 0, 0 };
    int bar = 0;
    std::atomic<int> active { -1 };
    std::atomic<int> beat { 0 };
    double sampleRate = 48000;
    double outputLatencyMs = 0;
    double stepPhase = 0;
    int step = 0;
    int section = mainASection;
    bool running = false;

    float click = 0, clickPhase = 0, clickAdvance = 0, clickDecay = 0;
    float kickPhase = 0, kick = 0, snare = 0, hat = 0;
    float bassPhase = 0, bass = 0, bassAdvance = 0;
    float kickDecay = 0, snareDecay = 0, hatDecay = 0, bassDecay = 0;
    float kickBaseAdvance = 0, kickSweepAdvance = 0;
    std::array<float, 12> bassRates {};
    std::uint32_t noise = 0x37fa267bu;
};
