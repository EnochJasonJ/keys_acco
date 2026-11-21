#pragma once

#include "ChordDetector.h"
#include "MidiEvent.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <atomic>

enum Section { mainASection, mainBSection, introSection, fillSection, endingSection, mainCSection, breakSection };

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
                next = playingBit | (introSection << 1);
            else if ((previous & playingBit) == 0)
                return;
            else
            {
                const auto section = command == fill ? fillSection : command == breakBar ? breakSection : endingSection;
                next = playingBit | (section << 1);
            }
        }
        while (!transport.compare_exchange_weak(previous, next, std::memory_order_relaxed));
    }

    bool isPlaying() const noexcept { return (transport.load(std::memory_order_relaxed) & playingBit) != 0; }
    int queuedSection() const noexcept { return transport.load(std::memory_order_relaxed) >> 1; }
    void stop() noexcept { transport.store(0, std::memory_order_relaxed); }

    int takeNextSection() noexcept
    {
        const auto state = transport.fetch_and(playingBit, std::memory_order_relaxed);
        if ((state & playingBit) == 0)
            return -1;
        const auto queued = state >> 1;
        return queued != 0 ? queued : requested.load(std::memory_order_relaxed);
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

private:
    static int encode(MidiTrigger trigger) noexcept { return (trigger.status << 8) | trigger.number; }

    static constexpr int playingBit = 1;
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
    explicit Arranger(LiveControls& controls) : live(controls) {}

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs,
                                         int channels, int samples,
                                         const juce::AudioIODeviceCallbackContext&) override;
    int currentBeat() const noexcept { return beat.load(std::memory_order_relaxed); }
    int activeSection() const noexcept { return active.load(std::memory_order_relaxed); }

private:
    void reset() noexcept;
    void beginStep(int step) noexcept;
    float nextSample() noexcept;

    LiveControls& live;
    std::atomic<int> active { -1 };
    std::atomic<int> beat { 0 };
    double sampleRate = 48000;
    double stepPhase = 0;
    int step = 0;
    int section = mainASection;
    bool running = false;

    float kickPhase = 0, kick = 0, snare = 0, hat = 0;
    float bassPhase = 0, bass = 0, bassAdvance = 0;
    float kickDecay = 0, snareDecay = 0, hatDecay = 0, bassDecay = 0;
    float kickBaseAdvance = 0, kickSweepAdvance = 0;
    std::array<float, 12> bassRates {};
    std::uint32_t noise = 0x37fa267bu;
};
