/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <wfg/engine/plugin/Catalogue.h>

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>

namespace wfg::plugin
{
    //==============================================================================
    const std::string& Parameter::textFor (float normalised) const noexcept
    {
        const auto clamped = std::clamp (normalised, 0.0f, 1.0f);

        if (discrete && steps > 1 && ! stepText.empty())
        {
            const auto step = std::clamp (static_cast<int> (std::lround (clamped * static_cast<float> (steps - 1))),
                                          0, static_cast<int> (stepText.size()) - 1);
            return stepText[static_cast<std::size_t> (step)];
        }

        const auto at = std::clamp (static_cast<int> (std::lround (clamped * 100.0f)), 0, 100);
        return text[static_cast<std::size_t> (at)];
    }

    //==============================================================================
    bool Catalogue::guessBipolar (float defaultValue, const std::string& atZero, const std::string& atOne)
    {
        /*  THE MIDDLE IS THE REST AND THE ENDS ARE OPPOSITE SIGNS: a default
            at the centre whose text at nought begins with a minus and whose
            text at one does not. A pan reads "L50" / "R50" and is not caught -
            a curated map is where that goes. */
        const auto centred = std::abs (defaultValue - 0.5f) < 0.02f;
        const auto zeroNegative = ! atZero.empty() && (atZero.front() == '-' || atZero.rfind ("\xe2\x88\x92", 0) == 0);
        const auto onePositive = ! atOne.empty() && atOne.front() != '-' && atOne.rfind ("\xe2\x88\x92", 0) != 0;

        return centred && zeroNegative && onePositive;
    }

    Catalogue Catalogue::testGain()
    {
        Catalogue out;
        out.identifier = testGainIdentifier();
        out.name = "Test gain";
        out.latencySamples = 0;

        /*  p0: a linear gain, resting at a half - what spike 07's child applied,
            with a text a person can read. */
        Parameter gain;
        gain.name = "Gain";
        gain.shortName = "Gain";
        gain.unit = "dB";
        gain.defaultValue = 0.5f;

        for (int i = 0; i <= 100; ++i)
        {
            const auto linear = static_cast<double> (i) / 100.0;
            gain.text[static_cast<std::size_t> (i)] = linear <= 0.0
                                                          ? std::string ("-inf dB")
                                                          : juce::String (20.0 * std::log10 (linear), 1).toStdString() + " dB";
        }

        gain.bipolar = false;
        out.params.push_back (std::move (gain));

        /*  p1: the kill switch. Discrete, two steps; at one the child aborts,
            which is how a driver kills a plugin mid-show with no Task Manager. */
        Parameter die;
        die.name = "Die";
        die.shortName = "Die";
        die.discrete = true;
        die.steps = 2;
        die.stepText = { "alive", "dead" };
        die.defaultValue = 0.0f;

        for (int i = 0; i <= 100; ++i)
            die.text[static_cast<std::size_t> (i)] = i < 50 ? "alive" : "dead";

        out.params.push_back (std::move (die));
        return out;
    }

    //==============================================================================
    std::string Catalogue::toJson() const
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("identifier", juce::String (identifier));
        object->setProperty ("name", juce::String (name));
        object->setProperty ("latencySamples", latencySamples);

        juce::Array<juce::var> list;

        for (const auto& parameter : params)
        {
            auto* p = new juce::DynamicObject();
            p->setProperty ("name", juce::String (parameter.name));
            p->setProperty ("shortName", juce::String (parameter.shortName));
            p->setProperty ("unit", juce::String (parameter.unit));
            p->setProperty ("default", static_cast<double> (parameter.defaultValue));
            p->setProperty ("discrete", parameter.discrete);
            p->setProperty ("steps", parameter.steps);
            p->setProperty ("bipolar", parameter.bipolar);

            juce::Array<juce::var> stepTexts;

            for (const auto& step : parameter.stepText)
                stepTexts.add (juce::String (step));

            p->setProperty ("stepText", stepTexts);

            juce::Array<juce::var> texts;

            for (const auto& t : parameter.text)
                texts.add (juce::String (t));

            p->setProperty ("text", texts);
            list.add (juce::var (p));
        }

        object->setProperty ("params", list);

        /*  JSON's numbers are locale-free by definition, and JUCE writes them
            so; nothing here goes through a locale-aware formatter. */
        return juce::JSON::toString (juce::var (object), false).toStdString();
    }

    bool Catalogue::fromJson (const std::string& text, Catalogue& out, std::string& problem)
    {
        const auto parsed = juce::JSON::parse (juce::String (text));
        auto* object = parsed.getDynamicObject();

        if (object == nullptr)
        {
            problem = "not a JSON object";
            return false;
        }

        Catalogue read;
        read.identifier = object->getProperty ("identifier").toString().toStdString();
        read.name = object->getProperty ("name").toString().toStdString();
        read.latencySamples = static_cast<int> (object->getProperty ("latencySamples"));

        if (read.identifier.empty())
        {
            problem = "no identifier";
            return false;
        }

        const auto* list = object->getProperty ("params").getArray();

        if (list != nullptr)
        {
            for (const auto& item : *list)
            {
                auto* p = item.getDynamicObject();

                if (p == nullptr)
                    continue;

                Parameter parameter;
                parameter.name = p->getProperty ("name").toString().toStdString();
                parameter.shortName = p->getProperty ("shortName").toString().toStdString();
                parameter.unit = p->getProperty ("unit").toString().toStdString();
                parameter.defaultValue = static_cast<float> (static_cast<double> (p->getProperty ("default")));
                parameter.discrete = static_cast<bool> (p->getProperty ("discrete"));
                parameter.steps = static_cast<int> (p->getProperty ("steps"));
                parameter.bipolar = static_cast<bool> (p->getProperty ("bipolar"));

                if (const auto* steps = p->getProperty ("stepText").getArray())
                    for (const auto& step : *steps)
                        parameter.stepText.push_back (step.toString().toStdString());

                if (const auto* texts = p->getProperty ("text").getArray())
                {
                    std::size_t at = 0;

                    for (const auto& t : *texts)
                    {
                        if (at >= parameter.text.size())
                            break;

                        parameter.text[at++] = t.toString().toStdString();
                    }
                }

                read.params.push_back (std::move (parameter));
            }
        }

        out = std::move (read);
        return true;
    }

    //==============================================================================
    CatalogueStore::CatalogueStore (std::string folderToUse)
        : root (std::move (folderToUse))
    {
        /*  THE TEST CATALOGUE IS ALWAYS KNOWN, on every machine, so the whole
            surface a real plugin presents - the param nodes, the text nodes,
            the count - is exercised in CI with no plugin installed. */
        held[Catalogue::testGainIdentifier()] = std::make_shared<const Catalogue> (Catalogue::testGain());
    }

    std::string CatalogueStore::fileFor (const std::string& identifier) const
    {
        /*  A HASH RATHER THAN THE IDENTIFIER, which carries a path with every
            character a file name refuses. The identifier itself is inside the
            file, so the name is only a key. */
        const auto hash = juce::SHA256 (identifier.data(), identifier.size()).toHexString().substring (0, 32);

        return juce::File (juce::String (root)).getChildFile (hash + ".json")
                   .getFullPathName().toStdString();
    }

    std::shared_ptr<const Catalogue> CatalogueStore::find (const std::string& identifier) const noexcept
    {
        const std::lock_guard<std::mutex> lock { mutex };
        const auto found = held.find (identifier);
        return found != held.end() ? found->second : nullptr;
    }

    std::uint64_t CatalogueStore::revision() const noexcept
    {
        const std::lock_guard<std::mutex> lock { mutex };
        return revisionCount;
    }

    std::size_t CatalogueStore::size() const noexcept
    {
        const std::lock_guard<std::mutex> lock { mutex };
        return held.size();
    }

    bool CatalogueStore::ensureLoaded (const std::string& identifier)
    {
        if (identifier.empty())
            return false;

        if (find (identifier) != nullptr)
            return true;

        /*  The disk is read outside the lock; only the map is under it. */
        const juce::File file { juce::String (fileFor (identifier)) };

        if (! file.existsAsFile())
            return false;

        Catalogue read;
        std::string problem;

        if (! Catalogue::fromJson (file.loadFileAsString().toStdString(), read, problem))
            return false;

        /*  The file is keyed by the hash of the identifier it was asked for;
            a file that names another is a file somebody edited, and is not
            believed. */
        if (read.identifier != identifier)
            return false;

        const std::lock_guard<std::mutex> lock { mutex };
        held[identifier] = std::make_shared<const Catalogue> (std::move (read));
        ++revisionCount;
        return true;
    }

    bool CatalogueStore::put (const Catalogue& catalogue)
    {
        if (catalogue.identifier.empty())
            return false;

        const auto text = catalogue.toJson();
        auto changed = false;

        {
            const std::lock_guard<std::mutex> lock { mutex };
            const auto found = held.find (catalogue.identifier);
            changed = found == held.end() || found->second->toJson() != text;
            held[catalogue.identifier] = std::make_shared<const Catalogue> (catalogue);

            if (changed)
                ++revisionCount;
        }

        const juce::File file { juce::String (fileFor (catalogue.identifier)) };
        file.getParentDirectory().createDirectory();
        file.replaceWithText (juce::String (text), false, false, "\n");

        return changed;
    }
}
