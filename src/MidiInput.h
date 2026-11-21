#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

#include "Arranger.h"

// JUCE serialises callbacks for the one open input. The UI is the only consumer.
class MidiInput final : private juce::MidiInputCallback
{
public:
    explicit MidiInput(LiveControls& controls) : live(controls) {}
    ~MidiInput() override { close(); }

    bool open(const juce::String& id)
    {
        close();
        input = juce::MidiInput::openDevice(id, this);
        if (input == nullptr)
            return false;
        input->start();
        return true;
    }

    void close()
    {
        if (input != nullptr)
        {
            input->stop();
            input.reset();
        }
        fifo.reset();
        live.resetNotes();
    }

    template <typename Consume>
    void drain(Consume consume)
    {
        const auto read = fifo.read(fifo.getNumReady());
        read.forEach([&](int index) { consume(events[static_cast<std::size_t>(index)]); });
    }

    unsigned takeDropped() { return dropped.exchange(0, std::memory_order_relaxed); }

private:
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override
    {
        MidiEvent event;
        event.time = message.getTimeStamp();
        event.size = message.getRawDataSize();
        if (event.size == 3 && live.recording.load(std::memory_order_relaxed) != 0)
            event.arrivalMs = juce::Time::getMillisecondCounterHiRes();
        std::copy_n(message.getRawData(),
                    std::min(event.size, static_cast<int>(event.bytes.size())),
                    event.bytes.begin());
        live.handle(event);

        const auto write = fifo.write(1);
        if (write.blockSize1 + write.blockSize2 == 0)
        {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        write.forEach([&](int index) { events[static_cast<std::size_t>(index)] = event; });
    }

    LiveControls& live;
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    std::array<MidiEvent, 2048> events {};
    juce::AbstractFifo fifo { static_cast<int>(events.size()) };
    std::atomic<unsigned> dropped { 0 };
    std::unique_ptr<juce::MidiInput> input;
};
