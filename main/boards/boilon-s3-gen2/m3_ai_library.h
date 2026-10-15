#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "media/media_catalog.h"

// P5-2: what 喵喵 can look up and play on the card. No ESP-IDF headers, so
// test_m3_ai_library runs this exact code. The player publishes its catalog
// once it plays and never changes it; the MCP tools on the main task only read
// it and leave a pick for the player task to take.
namespace boilon::ai::card {
// About 4 KB at most per reply: 20 entries, names shortened to 60 bytes.
constexpr size_t kPageSize = 20, kMaxMatches = 20, kMaxNameBytes = 60;
constexpr std::string_view kCut = "…";
constexpr char kNoCard[] =
    "The story player has nothing to play from right now: no card, an empty card or a card error.";

struct Pick {
    size_t directory = 0;  // Place in the catalog's collections.
    size_t number = 0;     // 1-based file; 0 continues from the bookmark.
};
// JSON for the model, or (not ok) why not.
struct Reply {
    bool ok = false;
    std::string text;
};

// Named like the directory list on the screen.
inline std::string DirectoryName(const media::Collection& collection) {
    return collection.is_root ? "未分类" : collection.directory_name;
}
// The file name without its extension.
inline std::string Title(const std::string& file_name) {
    const auto dot = file_name.rfind('.');
    if (dot == std::string::npos || dot == 0 || file_name.size() - dot > 5)
        return file_name;
    return file_name.substr(0, dot);
}
// At most kMaxNameBytes, cut between whole UTF-8 characters; "…" marks a cut.
inline std::string Shorten(std::string_view text) {
    if (text.size() <= kMaxNameBytes)
        return std::string(text);
    size_t end = kMaxNameBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    return std::string(text.substr(0, end)) + std::string(kCut);
}
inline void AppendString(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out += '"';
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (byte < 0x20) {
            out += "\\u00";
            out += kHex[byte >> 4];
            out += kHex[byte & 15];
        } else {
            out += c;
        }
    }
    out += '"';
}
inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
// A byte match that ignores the case of English letters; a UTF-8 needle can
// only match at a character boundary, so Chinese needs nothing more.
inline bool Contains(std::string_view text, std::string_view part) {
    for (size_t i = 0; i + part.size() <= text.size(); ++i) {
        size_t j = 0;
        while (j < part.size() && Lower(text[i + j]) == Lower(part[j]))
            ++j;
        if (j == part.size())
            return true;
    }
    return false;
}
inline bool Same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && Contains(a, b);
}
// Words separated by spaces: ASCII, or the full-width U+3000 (E3 80 80).
inline std::vector<std::string> Words(std::string_view text) {
    std::vector<std::string> words(1);
    for (size_t i = 0; i < text.size();) {
        const size_t space = text[i] == ' ' ? 1 : text.substr(i, 3) == "\xE3\x80\x80" ? 3 : 0;
        if (space && !words.back().empty())
            words.emplace_back();
        if (!space)
            words.back() += text[i];
        i += space ? space : 1;
    }
    if (words.back().empty())
        words.pop_back();
    return words;
}
// The directory the model named: the same name, else the only one that
// contains the name or that the name contains. A name these replies shortened
// still works: its "…" is dropped.
inline std::optional<size_t> FindDirectory(const media::MediaCatalog& catalog,
                                           std::string_view name) {
    if (name.size() > kCut.size() && name.substr(name.size() - kCut.size()) == kCut)
        name.remove_suffix(kCut.size());
    const auto& collections = catalog.Collections();
    for (size_t i = 0; i < collections.size(); ++i)
        if (Same(DirectoryName(collections[i]), name))
            return i;
    std::optional<size_t> found;
    for (size_t i = 0; i < collections.size() && !name.empty(); ++i) {
        const auto directory = DirectoryName(collections[i]);
        if (!Contains(directory, name) && !Contains(name, directory))
            continue;
        if (found)
            return std::nullopt;
        found = i;
    }
    return found;
}
inline Reply NoDirectory(const media::MediaCatalog& catalog, std::string_view name) {
    const auto& collections = catalog.Collections();
    std::string text = "No single directory matches \"" + Shorten(name) + "\". Directories:";
    for (size_t i = 0; i < collections.size() && i < kPageSize; ++i)
        text += " \"" + Shorten(DirectoryName(collections[i])) + "\"";
    if (collections.size() > kPageSize)
        text += " and " + std::to_string(collections.size() - kPageSize) + " more";
    return {false, text + "."};
}
inline Reply NoPage(int page, size_t pages) {
    return {false, "Page " + std::to_string(page) + " does not exist; there are " +
                       std::to_string(pages) + " pages."};
}
inline void AppendFile(std::string& out, const media::Collection& collection, size_t number,
                       bool with_directory) {
    out += '{';
    if (with_directory) {
        out += "\"directory\":";
        AppendString(out, Shorten(DirectoryName(collection)));
        out += ',';
    }
    out += "\"number\":" + std::to_string(number) + ",\"title\":";
    AppendString(out, Shorten(Title(collection.tracks[number - 1].file_name)));
    out += '}';
}

// self.story.list: the directories, one directory's files, or a search. Every
// word of the keyword must appear in the file's title or its directory's name.
inline Reply List(const media::MediaCatalog& catalog, std::optional<std::pair<size_t, size_t>> now,
                  std::string_view directory, std::string_view keyword, int page) {
    const auto& collections = catalog.Collections();
    std::optional<size_t> only;
    if (!directory.empty()) {
        only = FindDirectory(catalog, directory);
        if (!only)
            return NoDirectory(catalog, directory);
    }
    std::string out = "{";
    if (const auto words = Words(keyword); !words.empty()) {
        out += "\"keyword\":";
        AppendString(out, Shorten(keyword));
        out += ",\"matches\":[";
        size_t found = 0;
        bool more = false;
        for (size_t d = 0; d < collections.size() && !more; ++d) {
            if (only && d != *only)
                continue;
            const auto name = DirectoryName(collections[d]);
            for (size_t f = 0; f < collections[d].tracks.size() && !more; ++f) {
                const auto title = Title(collections[d].tracks[f].file_name);
                if (!std::all_of(words.begin(), words.end(), [&](const std::string& word) {
                        return Contains(title, word) || Contains(name, word);
                    }))
                    continue;
                more = found == kMaxMatches;
                if (more)
                    break;
                if (found++)
                    out += ',';
                AppendFile(out, collections[d], f + 1, true);
            }
        }
        out += std::string("],\"more\":") + (more ? "true" : "false") + "}";
        return {true, out};
    }
    const size_t total = only ? collections[*only].tracks.size() : collections.size();
    const size_t pages = std::max<size_t>(1, (total + kPageSize - 1) / kPageSize);
    if (page < 1 || static_cast<size_t>(page) > pages)
        return NoPage(page, pages);
    const size_t first = (page - 1) * kPageSize, last = std::min(total, first + kPageSize);
    if (only) {
        out += "\"directory\":";
        AppendString(out, Shorten(DirectoryName(collections[*only])));
        out += ",\"files\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
               ",\"pages\":" + std::to_string(pages) + ",\"items\":[";
        for (size_t f = first; f < last; ++f) {
            if (f > first)
                out += ',';
            AppendFile(out, collections[*only], f + 1, false);
        }
        return {true, out + "]}"};
    }
    out += "\"directories\":[";
    for (size_t d = first; d < last; ++d) {
        out += d > first ? ",{\"name\":" : "{\"name\":";
        AppendString(out, Shorten(DirectoryName(collections[d])));
        out += ",\"files\":" + std::to_string(collections[d].tracks.size()) + "}";
    }
    out += "],\"page\":" + std::to_string(page) + ",\"pages\":" + std::to_string(pages);
    if (now && now->first < collections.size() && now->second >= 1 &&
        now->second <= collections[now->first].tracks.size()) {
        out += ",\"now\":";
        AppendFile(out, collections[now->first], now->second, true);
    }
    return {true, out + "}"};
}

// self.story.play: checks the request and says what will play.
inline Reply Check(const media::MediaCatalog& catalog, std::string_view directory, int number,
                   Pick& pick) {
    const auto index = FindDirectory(catalog, directory);
    if (!index)
        return NoDirectory(catalog, directory);
    const auto& collection = catalog.Collections()[*index];
    const size_t files = collection.tracks.size();
    if (number < 0 || static_cast<size_t>(number) > files)
        return {false, "\"" + Shorten(DirectoryName(collection)) + "\" has " +
                           std::to_string(files) + " files: number must be 1 to " +
                           std::to_string(files) + ", or 0 to continue where it was last heard."};
    pick = {*index, static_cast<size_t>(number)};
    std::string out = "{\"playing\":";
    if (number) {
        AppendFile(out, collection, pick.number, true);
        out += ",\"from\":\"its beginning\"";
    } else {
        out += "{\"directory\":";
        AppendString(out, Shorten(DirectoryName(collection)));
        out += "},\"from\":\"where it was last heard\"";
    }
    return {true, out + ",\"starts\":\"when you finish this answer\"}"};
}

// Shared by the player task (publishes, reports what is on, takes picks) and
// the main task (the two tools).
class Card {
public:
    static Card& Shared() {
        static Card card;
        return card;
    }
    // The playing catalog, or nullptr: no card, an empty card or a card error.
    // A published catalog stays alive and unchanged.
    void Publish(const media::MediaCatalog* catalog) {
        catalog_.store(catalog, std::memory_order_release);
    }
    // The selected directory and 1-based file, from the player's tick.
    void SetNow(std::optional<std::pair<size_t, size_t>> now) {
        const bool fits = now && now->first < 0xffff && now->second <= 0xffff;
        now_.store(fits ? static_cast<uint32_t>((now->first + 1) << 16 | now->second) : 0,
                   std::memory_order_relaxed);
    }
    Reply List(std::string_view directory, std::string_view keyword, int page) const {
        const auto* catalog = catalog_.load(std::memory_order_acquire);
        if (!catalog)
            return {false, kNoCard};
        const uint32_t now = now_.load(std::memory_order_relaxed);
        std::optional<std::pair<size_t, size_t>> selected;
        if (now)
            selected.emplace(static_cast<size_t>((now >> 16) - 1),
                             static_cast<size_t>(now & 0xffff));
        return card::List(*catalog, selected, directory, keyword, page);
    }
    // A later pick replaces one the player has not taken yet.
    Reply Play(std::string_view directory, int number) {
        const auto* catalog = catalog_.load(std::memory_order_acquire);
        if (!catalog)
            return {false, kNoCard};
        Pick pick;
        auto reply = Check(*catalog, directory, number, pick);
        if (reply.ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = pick;
        }
        return reply;
    }
    std::optional<Pick> Take() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(pending_, std::nullopt);
    }

private:
    std::atomic<const media::MediaCatalog*> catalog_{nullptr};
    std::atomic<uint32_t> now_{0};
    std::mutex mutex_;
    std::optional<Pick> pending_;
};
}  // namespace boilon::ai::card
