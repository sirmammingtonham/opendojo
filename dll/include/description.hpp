#pragma once

#include <string>
#include <string_view>

namespace opendojo::drill {

// Shared by the full decoder and the bounded library-header reader.
// Raw legacy continuation text is legal only immediately after description:.
struct DescriptionReader {
    enum class Result { Other, Consumed, Invalid };
    static constexpr std::size_t MAX_BYTES = 4096;
    bool continuing = false;
    bool explicit_lines = false;
    bool literal_lines = false;

    Result read(std::string_view raw, std::string& description) {
        auto line = raw;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        const auto colon = line.find(':');
        auto key = line.substr(0, colon);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.remove_suffix(1);
        const bool field = colon != std::string_view::npos;
        if (field && key == "description_format") {
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            if (value != "pipe_lines" || explicit_lines) return Result::Invalid;
            literal_lines = true;
            continuing = false;
            return Result::Consumed;
        }
        if (field && key == "description_line") {
            auto value = line.substr(colon + 1);
            if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            if (value.empty() || value.front() != '|') return Result::Invalid;
            value.remove_prefix(1);
            const auto result = append(value, description, true);
            explicit_lines = true;
            continuing = false;
            return result;
        }
        if (field && key == "description") {
            if (explicit_lines) return Result::Consumed;
            auto value = line.substr(colon + 1);
            if (literal_lines) {
                if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            } else {
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.remove_prefix(1);
            }
            description.clear();
            continuing = true;
            return append(value, description, false);
        }
        if (line.starts_with("---") ||
            (field && (key == "name" || key == "character" || key == "cpu_side" ||
                       key == "recordings" || key == "author_handle" || key == "cloud_id"))) {
            continuing = false;
            return Result::Other;
        }
        return continuing ? append(raw, description, true) : Result::Other;
    }

private:
    static Result append(std::string_view value, std::string& out, bool newline) {
        if (value.find('\0') != std::string_view::npos || value.size() > MAX_BYTES ||
            out.size() + value.size() + newline > MAX_BYTES)
            return Result::Invalid;
        if (newline) out += '\n';
        out.append(value);
        return Result::Consumed;
    }
};

}  // namespace opendojo::drill
