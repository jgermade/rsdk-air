#include <rsdkair/ini.hpp>

#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace rsdkair {

namespace {

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::vector<std::string> split_lines(std::string_view text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string line(text.substr(start, end - start));
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

// "[Name]" -> "Name"; not a section header -> nullopt-like empty with false.
bool section_name(std::string_view line, std::string_view &name)
{
    line = trim(line);
    if (line.size() < 2 || line.front() != '[' || line.back() != ']')
        return false;
    name = trim(line.substr(1, line.size() - 2));
    return true;
}

bool key_of(std::string_view line, std::string_view &key, std::string_view &value)
{
    const std::string_view t = trim(line);
    if (t.empty() || t.front() == ';' || t.front() == '#')
        return false;
    const auto eq = t.find('=');
    if (eq == std::string_view::npos)
        return false;
    key = trim(t.substr(0, eq));
    value = trim(t.substr(eq + 1));
    return true;
}

} // namespace

std::string ini_set(std::string_view text, std::string_view section, std::string_view key, std::string_view value)
{
    std::vector<std::string> lines = split_lines(text);
    const std::string entry = std::string(key) + "=" + std::string(value);

    bool in_section = false;
    std::ptrdiff_t last_in_section = -1;  // last non-empty line of the section
    std::ptrdiff_t section_header = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string_view name;
        if (section_name(lines[i], name)) {
            if (in_section)
                break;
            in_section = iequals(name, section);
            if (in_section) {
                section_header = static_cast<std::ptrdiff_t>(i);
                last_in_section = section_header;
            }
            continue;
        }
        if (!in_section)
            continue;
        std::string_view k, v;
        if (key_of(lines[i], k, v) && iequals(k, key)) {
            lines[i] = entry;
            section_header = -2;  // done
            break;
        }
        if (!trim(lines[i]).empty())
            last_in_section = static_cast<std::ptrdiff_t>(i);
    }

    if (section_header >= 0) {
        lines.insert(lines.begin() + last_in_section + 1, entry);
    } else if (section_header == -1) {
        if (!lines.empty() && !trim(lines.back()).empty())
            lines.emplace_back();
        lines.push_back("[" + std::string(section) + "]");
        lines.push_back(entry);
    }

    std::string out;
    for (const std::string &line : lines)
        out += line + "\n";
    return out;
}

std::string ini_get(std::string_view text, std::string_view section, std::string_view key)
{
    bool in_section = false;
    for (const std::string &line : split_lines(text)) {
        std::string_view name;
        if (section_name(line, name)) {
            in_section = iequals(name, section);
            continue;
        }
        std::string_view k, v;
        if (in_section && key_of(line, k, v) && iequals(k, key))
            return std::string(v);
    }
    return {};
}

bool ini_set_file(const std::filesystem::path &path, std::string_view section, std::string_view key,
                  std::string_view value)
{
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::stringstream buffer;
            buffer << in.rdbuf();
            text = buffer.str();
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << ini_set(text, section, key, value);
    return static_cast<bool>(out);
}

} // namespace rsdkair
