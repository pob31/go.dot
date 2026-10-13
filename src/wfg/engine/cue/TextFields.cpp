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

#include <wfg/engine/cue/TextFields.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/video/Blanks.h>
#include <wfg/engine/video/TextLook.h>

#include <utility>

namespace wfg::cue
{
    TextFields::Set TextFields::set (const std::string& name, const std::string& value)
    {
        if (! video::isFieldName (name))
            return Set::badName;

        const auto found = values.find (name);

        if (found == values.end() && values.size() >= mostNames)
            return Set::full;

        auto cut = video::cutAtCharacter (value, valueBytes);

        if (found != values.end() && found->second == cut)
            return Set::written;

        values[name] = std::move (cut);
        ++changes;
        return Set::written;
    }

    std::string TextFields::valueOf (const std::string& name) const
    {
        const auto found = values.find (name);
        return found != values.end() ? found->second : std::string {};
    }

    void TextFields::replaceAll (std::map<std::string, std::string> leaders)
    {
        if (leaders == values)
            return;

        values = std::move (leaders);
        ++changes;
    }

    void TextFields::clear()
    {
        if (values.empty())
            return;

        values.clear();
        ++changes;
    }

    bool isTextFieldAddress (std::string_view address) noexcept
    {
        return address.size() > textFieldPrefix.size() && address.substr (0, textFieldPrefix.size()) == textFieldPrefix;
    }

    doc::LiveWrite textFieldWriteFor (TextFields& fields)
    {
        return [&fields] (const std::string& address, const std::string& text, const std::vector<osc::Value>& args)
                   -> std::optional<Outcome>
        {
            if (address.rfind (textFieldPrefix, 0) != 0)
                return std::nullopt;

            switch (fields.set (address.substr (textFieldPrefix.size()), text))
            {
                case TextFields::Set::written: return Outcome::ok (args);
                case TextFields::Set::badName: return Outcome::rejected (reason::badAddress);
                case TextFields::Set::full:    return Outcome::rejected (reason::badValue);
            }

            return Outcome::rejected (reason::badValue);
        };
    }
}
