#pragma once

#include "StyleTimeline.h"
#include <juce_core/juce_core.h>
#include <optional>

inline juce::var encodeStyle(const StyleTimeline& style)
{
    juce::Array<juce::var> bars;
    for (const auto& bar : style.bars)
    {
        juce::var entry(new juce::DynamicObject());
        auto* object = entry.getDynamicObject();
        object->setProperty("kick", static_cast<int>(bar.kick));
        object->setProperty("snare", static_cast<int>(bar.snare));
        object->setProperty("hat", static_cast<int>(bar.hat));
        object->setProperty("bass", static_cast<int>(bar.bass));
        object->setProperty("cutTails", static_cast<int>(bar.cutTails));
        object->setProperty("bassFifthOnBeatThree", bar.bassFifthOnBeatThree);
        bars.add(std::move(entry));
    }

    juce::var sections(new juce::DynamicObject());
    for (std::size_t i = 0; i < style.assignments.size(); ++i)
    {
        const auto range = style.assignments[i];
        sections.getDynamicObject()->setProperty(juce::Identifier(sectionNames[i]),
                                                 juce::Array<juce::var> { range.first + 1, range.last + 1 });
    }

    juce::var result(new juce::DynamicObject());
    auto* object = result.getDynamicObject();
    object->setProperty("version", 1);
    object->setProperty("bars", std::move(bars));
    object->setProperty("sections", std::move(sections));
    return result;
}

inline std::optional<StyleTimeline> decodeStyle(const juce::var& json)
{
    const auto version = json.getProperty("version", {});
    const auto barValue = json.getProperty("bars", {});
    const auto* values = barValue.getArray();
    if (!version.isInt() || static_cast<int>(version) != 1 || values == nullptr
        || values->isEmpty() || values->size() > StyleTimeline::maxBars)
        return {};

    StyleTimeline style;
    for (const auto& value : *values)
    {
        constexpr std::array fields { "kick", "snare", "hat", "bass", "cutTails" };
        std::array<unsigned, fields.size()> masks {};
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            const auto mask = value.getProperty(fields[i], {});
            if (!mask.isInt() || static_cast<int>(mask) < 0 || static_cast<int>(mask) > 255)
                return {};
            masks[i] = static_cast<unsigned>(static_cast<int>(mask));
        }
        const auto fifth = value.getProperty("bassFifthOnBeatThree", {});
        if (!fifth.isBool())
            return {};
        style.bars.push_back({ masks[0], masks[1], masks[2], masks[3], masks[4], static_cast<bool>(fifth) });
    }

    const auto sections = json.getProperty("sections", {});
    for (std::size_t i = 0; i < style.assignments.size(); ++i)
    {
        const auto rangeValue = sections.getProperty(juce::Identifier(sectionNames[i]), {});
        const auto* range = rangeValue.getArray();
        if (range == nullptr || range->size() != 2 || !(*range)[0].isInt() || !(*range)[1].isInt())
            return {};
        const int first = static_cast<int>((*range)[0]);
        const int last = static_cast<int>((*range)[1]);
        if (first < 1 || last < first || last > values->size())
            return {};
        style.assignments[i] = { first - 1, last - 1 };
    }
    return style;
}
