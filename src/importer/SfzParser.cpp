#include "SfzParser.h"

#include <cstring>

namespace lithium::sfz
{

namespace
{
    struct ParseError
    {
        juce::String message;
    };

    using Opcodes = std::map<juce::String, juce::String>;

    bool isNameChar (juce::juce_wchar c) noexcept
    {
        return juce::CharacterFunctions::isLetterOrDigit (c) || c == '_' || c == '$';
    }

    class Parser
    {
    public:
        Parser (Instrument& r, juce::File rootFile) : result (r), root (std::move (rootFile))
        {
            result.file = root;
        }

        void parseText (const juce::String& text, const juce::String& fileName, int depth)
        {
            if (depth > 16)
                throw ParseError { fileName + ": #include nested too deeply" };

            const auto* const start = text.toRawUTF8();
            const auto length = (int) std::strlen (start);
            int pos = 0, line = 1;

            auto peek = [&] (int offset = 0) -> char { return pos + offset < length ? start[pos + offset] : '\0'; };
            auto advance = [&]
            {
                if (start[pos] == '\n')
                    ++line;

                ++pos;
            };

            auto skipToEndOfLine = [&] { while (pos < length && start[pos] != '\n') advance(); };

            auto readWhile = [&] (auto predicate)
            {
                const auto from = pos;

                while (pos < length && predicate (start[pos]))
                    advance();

                return juce::String::fromUTF8 (start + from, pos - from);
            };

            while (pos < length)
            {
                const auto c = peek();

                if (juce::CharacterFunctions::isWhitespace ((juce::juce_wchar) (unsigned char) c))
                {
                    advance();
                }
                else if (c == '/' && peek (1) == '/')
                {
                    skipToEndOfLine();
                }
                else if (c == '/' && peek (1) == '*')
                {
                    while (pos < length && ! (peek() == '*' && peek (1) == '/'))
                        advance();

                    pos = std::min (length, pos + 2);
                }
                else if (c == '#')
                {
                    advance();
                    const auto directive = readWhile ([] (char x) { return isNameChar ((juce::juce_wchar) (unsigned char) x); });

                    if (directive == "define")
                    {
                        readWhile ([] (char x) { return x == ' ' || x == '\t'; });
                        const auto name = readWhile ([] (char x) { return isNameChar ((juce::juce_wchar) (unsigned char) x); });
                        const auto value = readWhile ([] (char x) { return x != '\n' && x != '\r'; });
                        define (name, value.upToFirstOccurrenceOf ("//", false, false).trim());
                    }
                    else if (directive == "include")
                    {
                        readWhile ([] (char x) { return x == ' ' || x == '\t'; });
                        auto rest = readWhile ([] (char x) { return x != '\n' && x != '\r'; }).trim();
                        const auto path = substitute (rest.unquoted().upToFirstOccurrenceOf ("\"", false, false))
                                              .replaceCharacter ('\\', '/');
                        const auto file = root.getParentDirectory().getChildFile (path);

                        if (file.existsAsFile())
                            parseText (file.loadFileAsString(), path, depth + 1);
                        else
                            result.warnings.addIfNotAlreadyThere (fileName + ":" + juce::String (line) + ": missing #include \"" + path + "\"");
                    }
                    else
                    {
                        skipToEndOfLine();
                    }
                }
                else if (c == '<')
                {
                    advance();
                    const auto name = readWhile ([] (char x) { return x != '>' && x != '\n'; }).trim().toLowerCase();

                    if (peek() == '>')
                        advance();

                    header (name, fileName, line);
                }
                else
                {
                    // opcode=value. Values end at whitespace, except sample=,
                    // whose value may contain spaces: it runs to the end of
                    // the line, or to the next "opcode=" on the same line.
                    const auto name = substitute (readWhile ([] (char x) { return isNameChar ((juce::juce_wchar) (unsigned char) x); }));

                    if (name.isEmpty() || peek() != '=')
                    {
                        skipToEndOfLine();
                        continue;
                    }

                    advance();   // '='
                    juce::String value;

                    if (name == "sample" || name == "default_path" || name.startsWith ("label_"))
                    {
                        const auto from = pos;
                        auto end = pos;

                        while (pos < length && peek() != '\n' && peek() != '\r' && peek() != '<'
                               && ! (peek() == '/' && peek (1) == '/'))
                        {
                            if (peek() == ' ' || peek() == '\t')
                            {
                                // Is the next word "name="? Then the value ended here.
                                auto look = pos;

                                while (look < length && (start[look] == ' ' || start[look] == '\t'))
                                    ++look;

                                auto nameEnd = look;

                                while (nameEnd < length && isNameChar ((juce::juce_wchar) (unsigned char) start[nameEnd]))
                                    ++nameEnd;

                                if (nameEnd > look && nameEnd < length && start[nameEnd] == '=')
                                    break;
                            }
                            else
                            {
                                end = pos + 1;
                            }

                            advance();
                        }

                        value = juce::String::fromUTF8 (start + from, end - from);
                    }
                    else
                    {
                        value = readWhile ([] (char x) { return ! juce::CharacterFunctions::isWhitespace ((juce::juce_wchar) (unsigned char) x) && x != '<'; });
                    }

                    opcode (name, substitute (value.trim()), fileName, line);
                }
            }
        }

        void finish()
        {
            flushRegion();
        }

    private:
        enum class Level { none, control, global, master, group, region, other };

        void define (const juce::String& name, const juce::String& value)
        {
            if (name.startsWithChar ('$'))
                defines[name] = value;
        }

        /** Replaces $variables, longest name first where names share a prefix. */
        juce::String substitute (const juce::String& text) const
        {
            if (! text.containsChar ('$') || defines.empty())
                return text;

            juce::String out;
            auto p = text.getCharPointer();

            while (! p.isEmpty())
            {
                if (*p == '$')
                {
                    const juce::String rest (p);
                    const std::pair<const juce::String, juce::String>* best = nullptr;

                    for (const auto& d : defines)
                        if (rest.startsWith (d.first) && (best == nullptr || d.first.length() > best->first.length()))
                            best = &d;

                    if (best != nullptr)
                    {
                        out << best->second;
                        p += best->first.length();
                        continue;
                    }
                }

                out << juce::String::charToString (*p);
                ++p;
            }

            return out;
        }

        void header (const juce::String& name, const juce::String& fileName, int line)
        {
            flushRegion();

            if (name == "control")       level = Level::control;
            else if (name == "global")   { level = Level::global; global.clear(); master.clear(); group.clear(); }
            else if (name == "master")   { level = Level::master; master.clear(); group.clear(); }
            else if (name == "group")    { level = Level::group; group.clear(); }
            else if (name == "region")   { level = Level::region; region.clear(); regionWhere = fileName + ":" + juce::String (line); }
            else                         level = Level::other;   // <curve>, <effect>, <midi>...: not used for import
        }

        void opcode (const juce::String& rawName, const juce::String& value, const juce::String& fileName, int line)
        {
            // Common aliases and misspellings, as SFZ players accept them.
            auto name = rawName;

            if (name == "offby")                    name = "off_by";
            else if (name == "polyphony_group")     name = "group";
            else if (name == "loopmode")            name = "loop_mode";

            switch (level)
            {
                case Level::control:
                    if (name == "default_path")
                        defaultPath = value.replaceCharacter ('\\', '/');
                    else if (name.startsWith ("set_cc"))
                        result.controllerDefaults[name.substring (6).getIntValue()] = value.getDoubleValue();
                    break;

                case Level::global:  set (global, name, value);  break;
                case Level::master:  set (master, name, value);  break;
                case Level::group:   set (group, name, value);   break;
                case Level::region:  set (region, name, value);  break;

                case Level::none:
                    result.warnings.addIfNotAlreadyThere (fileName + ":" + juce::String (line) + ": \"" + name + "\" outside any header ignored");
                    break;

                case Level::other:
                    break;
            }
        }

        static void set (Opcodes& map, const juce::String& name, const juce::String& value)
        {
            // key= is shorthand for lokey, hikey and pitch_keycenter.
            if (name == "key")
            {
                map["lokey"] = map["hikey"] = map["pitch_keycenter"] = value;
                return;
            }

            map[name] = value;
        }

        void flushRegion()
        {
            if (level != Level::region)
                return;

            Region r;
            r.where = regionWhere;

            for (const auto* scope : { &global, &master, &group, &region })
                for (const auto& [k, v] : *scope)
                    r.opcodes[k] = v;

            const auto it = r.opcodes.find ("sample");

            if (it != r.opcodes.end() && it->second.isNotEmpty())
            {
                const auto sample = it->second.replaceCharacter ('\\', '/');

                if (sample.startsWithChar ('*'))
                {
                    r.sample = sample;
                }
                else
                {
                    r.sample = defaultPath + sample;
                    r.sampleFile = root.getParentDirectory().getChildFile (r.sample);
                }

                result.regions.push_back (std::move (r));
            }

            region.clear();
            level = Level::none;
        }

        Instrument& result;
        const juce::File root;
        std::map<juce::String, juce::String> defines;
        juce::String defaultPath;
        Level level = Level::none;
        Opcodes global, master, group, region;
        juce::String regionWhere;
    };
} // namespace

//==============================================================================
juce::String Region::get (const juce::String& opcode, const juce::String& fallback) const
{
    const auto it = opcodes.find (opcode);
    return it != opcodes.end() ? it->second : fallback;
}

double Region::getNumber (const juce::String& opcode, double fallback) const
{
    const auto it = opcodes.find (opcode);
    return it != opcodes.end() && it->second.isNotEmpty() ? it->second.getDoubleValue() : fallback;
}

int Region::getNote (const juce::String& opcode, int fallback) const
{
    const auto it = opcodes.find (opcode);

    if (it == opcodes.end())
        return fallback;

    const auto n = noteNumber (it->second);
    return n >= 0 ? n : fallback;
}

int noteNumber (const juce::String& text)
{
    const auto s = text.trim().toLowerCase();

    if (s.isEmpty())
        return -1;

    if (s.containsOnly ("-0123456789"))
        return juce::jlimit (-1, 127, s.getIntValue());

    // Note names: c4 is middle C (60), as in most SFZ players.
    static const juce::String letters ("c d ef g a b");
    const auto semitone = letters.indexOfChar (s[0]);

    if (semitone < 0 || s[0] == ' ')
        return -1;

    auto rest = s.substring (1);
    int accidental = 0;

    if (rest.startsWithChar ('#'))       { accidental = 1; rest = rest.substring (1); }
    else if (rest.startsWithChar ('b'))  { accidental = -1; rest = rest.substring (1); }

    if (! rest.containsOnly ("-0123456789") || rest.isEmpty())
        return -1;

    const auto note = (rest.getIntValue() + 1) * 12 + semitone + accidental;
    return juce::isPositiveAndBelow (note, 128) ? note : -1;
}

juce::Result parse (const juce::File& sfzFile, Instrument& result)
{
    if (! sfzFile.existsAsFile())
        return juce::Result::fail ("No such file: " + sfzFile.getFullPathName());

    return parse (sfzFile.loadFileAsString(), sfzFile, result);
}

juce::Result parse (const juce::String& text, const juce::File& sfzFile, Instrument& result)
{
    result = {};

    try
    {
        Parser parser (result, sfzFile);
        parser.parseText (text, sfzFile.getFileName(), 0);
        parser.finish();
    }
    catch (const ParseError& e)
    {
        return juce::Result::fail (e.message);
    }

    if (result.regions.empty())
        return juce::Result::fail (sfzFile.getFileName() + " has no regions with samples");

    return juce::Result::ok();
}

} // namespace lithium::sfz
