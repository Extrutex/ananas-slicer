#include <catch2/catch_test_macros.hpp>

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

// create_scaled_bitmap() throws when neither <name>.svg nor <name>.png exists in
// resources/images, and the build cannot see that: a removed or renamed image only fails at
// runtime, inside whatever event handler first opens the dialog. This test scans the GUI sources
// for string-literal image names passed to the bitmap loaders and checks each one on disk.

namespace fs = boost::filesystem;

namespace {

fs::path repo_root() { return fs::path(PROFILES_DIR).parent_path().parent_path(); }

std::string read_file(const fs::path& path)
{
    std::ifstream in(path.string(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Skips a string or character literal starting at s[i]; returns the index just past it.
std::size_t skip_literal(const std::string& s, std::size_t i)
{
    const char quote = s[i++];
    while (i < s.size() && s[i] != quote)
        i += s[i] == '\\' ? 2 : 1;
    return i + 1;
}

// Removes // and /* */ comments (keeping newlines so line numbers stay valid). Literals are kept.
std::string strip_comments(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s.compare(i, 2, "//") == 0) {
            const std::size_t end = s.find('\n', i);
            i = end == std::string::npos ? s.size() : end;
        } else if (s.compare(i, 2, "/*") == 0) {
            const std::size_t end = s.find("*/", i + 2);
            const std::size_t stop = end == std::string::npos ? s.size() : end + 2;
            for (std::size_t k = i; k < stop; ++k)
                if (s[k] == '\n')
                    out += '\n';
            i = stop;
        } else if (s[i] == '"' || s[i] == '\'') {
            const std::size_t end = std::min(skip_literal(s, i), s.size());
            out.append(s, i, end - i);
            i = end;
        } else {
            out += s[i++];
        }
    }
    return out;
}

bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(const std::string& s)
{
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

struct Missing
{
    std::string file;
    std::size_t line;
    std::string call;
    std::string name;
};

} // namespace

TEST_CASE("Every literal bitmap name passed to the image loaders exists in resources/images", "[Resources]")
{
    const fs::path images = repo_root() / "resources" / "images";
    const fs::path gui    = repo_root() / "src" / "slic3r";
    REQUIRE(fs::is_directory(images));
    REQUIRE(fs::is_directory(gui));

    // var() resolves a file name against resources/images; the bitmap loaders take a bare name.
    const std::regex call(R"(\b(create_scaled_bitmap|create_menu_bitmap|ScalableBitmap|var)\s*\()");
    const std::regex literal(R"re("((?:[^"\\]|\\.)*)")re");
    const std::regex image_file(R"(\.(png|svg|ico|icns|jpg|gif)$)");

    std::vector<Missing> missing;
    std::size_t          checked = 0;

    for (fs::recursive_directory_iterator it(gui), end; it != end; ++it) {
        const fs::path& path = it->path();
        if (!fs::is_regular_file(path) || (path.extension() != ".cpp" && path.extension() != ".hpp"))
            continue;

        const std::string src = strip_comments(read_file(path));
        for (std::sregex_iterator m(src.begin(), src.end(), call), mend; m != mend; ++m) {
            // Collect the call's argument list up to the matching parenthesis.
            const std::size_t open = static_cast<std::size_t>(m->position(0) + m->length(0));
            std::size_t       j    = open;
            for (int depth = 1; j < src.size() && depth > 0;) {
                if (src[j] == '"' || src[j] == '\'') {
                    j = skip_literal(src, j);
                    continue;
                }
                if (src[j] == '(')
                    ++depth;
                else if (src[j] == ')')
                    --depth;
                ++j;
            }
            const std::string args = src.substr(open, j > open ? j - open - 1 : 0);
            const std::string fn   = (*m)[1].str();

            for (std::sregex_iterator l(args.begin(), args.end(), literal), lend; l != lend; ++l) {
                const std::string name   = (*l)[1].str();
                const std::string before = trim(args.substr(0, static_cast<std::size_t>(l->position(0))));
                const std::string after  = trim(args.substr(static_cast<std::size_t>(l->position(0) + l->length(0))));
                // Composed names ("prefix_" + x), colors ("#RRGGBB") and format strings are not image names.
                if (name.empty() || name[0] == '#' || name.find('%') != std::string::npos ||
                    ends_with(before, "+") || (!after.empty() && after[0] == '+'))
                    continue;

                bool found;
                if (fn == "var") {
                    if (!std::regex_search(name, image_file))
                        continue;
                    found = fs::exists(images / name);
                } else {
                    const std::string base = ends_with(name, ".png") ? name.substr(0, name.size() - 4) : name;
                    found = fs::exists(images / (base + ".svg")) || fs::exists(images / (base + ".png"));
                }
                ++checked;
                if (!found) {
                    const std::size_t line = 1 + static_cast<std::size_t>(
                        std::count(src.begin(), src.begin() + m->position(0), '\n'));
                    missing.push_back({fs::relative(path, repo_root()).generic_string(), line, fn, name});
                }
            }
        }
    }

    // Guard against the scan silently matching nothing (moved sources, broken regex).
    CHECK(checked > 100);
    for (const Missing& m : missing) {
        INFO(m.file << ":" << m.line << " " << m.call << "(\"" << m.name << "\")");
        CHECK(false);
    }
    CHECK(missing.empty());
}
