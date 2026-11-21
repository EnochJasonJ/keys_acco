#pragma once

#include "MidiEvent.h"
#include "StyleTimeline.h"
#include <algorithm>
#include <cmath>

class MidiTake
{
public:
    MidiTake(BarRange range, int trackToRecord, int tempo)
        : firstTick(range.first * StyleTimeline::ticksPerBar),
          lastTick((range.last + 1) * StyleTimeline::ticksPerBar),
          track(trackToRecord), ticksPerMs(tempo * StyleTimeline::ticksPerBeat / 60000.0)
    {
        for (auto& channel : held)
            channel.fill(-1);
    }

    int tickAt(double time, double epoch) const
    {
        const auto elapsed = std::clamp((time - epoch) * ticksPerMs, 0.0,
                                       static_cast<double>(lastTick - firstTick));
        return firstTick + static_cast<int>(std::lround(elapsed));
    }

    void capture(const MidiEvent& event, double epoch)
    {
        if (event.size != 3 || epoch <= 0 || event.arrivalMs < epoch || event.bytes[1] > 127)
            return;
        const auto status = event.bytes[0];
        const auto type = status & 0xf0;
        if (type != 0x80 && type != 0x90)
            return;
        const auto tick = tickAt(event.arrivalMs, epoch);
        auto& active = held[status & 15][event.bytes[1]];
        if (active >= 0)
        {
            finishNote(active, tick);
            active = -1;
        }

        if (type == 0x90 && event.bytes[2] > 0 && tick < lastTick)
        {
            if (notes.size() == StyleTimeline::maxNotes)
            {
                overflow = true;
                return;
            }
            active = static_cast<int>(notes.size());
            notes.push_back({ track, tick, 1, event.bytes[1], event.bytes[2] });
        }
    }

    // Stage a replacement so an empty or oversized take leaves the style untouched.
    bool apply(StyleTimeline& style, int stopTick)
    {
        if (notes.empty() || overflow)
            return false;
        const int end = std::clamp(stopTick, firstTick + 1, lastTick);
        if (std::none_of(notes.begin(), notes.end(), [end](const auto& note) { return note.tick < end; }))
            return false;
        for (auto& channel : held)
            for (const auto active : channel)
                if (active >= 0)
                    finishNote(active, end);

        const int replaceEnd = std::min(lastTick, ((end - 1) / StyleTimeline::ticksPerBar + 1) * StyleTimeline::ticksPerBar);
        std::vector<StyleNote> combined;
        combined.reserve(style.notes.size() + notes.size());
        for (auto note : style.notes)
        {
            const int noteEnd = note.tick + note.length;
            if (note.track != track || noteEnd <= firstTick || note.tick >= replaceEnd)
            {
                combined.push_back(note);
                continue;
            }
            if (note.tick < firstTick)
            {
                auto left = note;
                left.length = firstTick - note.tick;
                combined.push_back(left);
            }
            if (noteEnd > replaceEnd)
            {
                note.tick = replaceEnd;
                note.length = noteEnd - replaceEnd;
                combined.push_back(note);
            }
        }
        for (auto note : notes)
            if (note.tick < end)
            {
                note.length = std::min(note.length, end - note.tick);
                combined.push_back(note);
            }
        if (combined.size() > StyleTimeline::maxNotes)
            return false;

        for (int tick = firstTick; tick < replaceEnd; tick += StyleTimeline::ticksPerBar)
            style.bars[static_cast<std::size_t>(tick / StyleTimeline::ticksPerBar)].recordedTracks |= 1u << track;
        style.notes = std::move(combined);
        return true;
    }

private:
    void finishNote(int index, int tick)
    {
        auto& note = notes[static_cast<std::size_t>(index)];
        note.length = std::max(1, tick - note.tick);
    }

    int firstTick, lastTick, track;
    double ticksPerMs;
    std::array<std::array<int, 128>, 16> held {};
    std::vector<StyleNote> notes;
    bool overflow = false;
};
