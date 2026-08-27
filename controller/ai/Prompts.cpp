#include "ai/Prompts.hpp"

#include <algorithm>

using json = nlohmann::json;

namespace daw::ai {

namespace {

bool validId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

/// A missing field is not an error: the server may be older than this build, and
/// a playbook with no tags is a playbook, not a broken document.
std::string stringAt(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

} // namespace

PromptPack parsePromptPack(const json& body, std::string* error) {
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return PromptPack{};
    };
    if (error) error->clear();
    if (!body.is_object()) return fail("prompt pack is not a JSON object");

    PromptPack pack;
    pack.version = stringAt(body, "version");
    pack.main = stringAt(body, "main");
    if (pack.main.empty()) return fail("prompt pack has no main prompt");
    if (pack.main.size() > kMaxMainPromptBytes)
        return fail("main prompt is over the size limit");

    const auto books = body.find("playbooks");
    if (books != body.end() && !books->is_null()) {
        if (!books->is_array()) return fail("playbooks is not an array");
        if (books->size() > kMaxPlaybooks)
            return fail("prompt pack has too many playbooks");
        pack.playbooks.reserve(books->size());
        for (const json& entry : *books) {
            if (!entry.is_object()) return fail("a playbook is not a JSON object");
            Playbook book;
            book.id = stringAt(entry, "id");
            book.title = stringAt(entry, "title");
            // Both spellings: the wire uses snake_case like the rest of the API,
            // and the on-disk cache is written from this struct.
            book.useWhen = stringAt(entry, "use_when");
            if (book.useWhen.empty()) book.useWhen = stringAt(entry, "useWhen");
            book.body = stringAt(entry, "body");
            if (!validId(book.id))
                return fail("a playbook id is missing or not [a-z0-9-]");
            if (book.body.empty()) return fail("a playbook has an empty body");
            if (book.body.size() > kMaxPlaybookBytes)
                return fail("a playbook is over the size limit");
            if (findPlaybook(pack, book.id))
                return fail("two playbooks share an id");
            if (book.title.empty()) book.title = book.id;

            const auto tags = entry.find("tags");
            if (tags != entry.end() && tags->is_array()) {
                for (const json& tag : *tags)
                    if (tag.is_string()) book.tags.push_back(tag.get<std::string>());
            }
            pack.playbooks.push_back(std::move(book));
        }
    }
    if (pack.version.empty()) pack.version = "unversioned";
    return pack;
}

json toJson(const PromptPack& pack) {
    json books = json::array();
    for (const Playbook& book : pack.playbooks) {
        books.push_back(json{{"id", book.id},
                             {"title", book.title},
                             {"use_when", book.useWhen},
                             {"tags", book.tags},
                             {"body", book.body}});
    }
    return json{{"version", pack.version},
                {"main", pack.main},
                {"playbooks", std::move(books)}};
}

std::string playbookIndex(const PromptPack& pack) {
    std::string out;
    for (const Playbook& book : pack.playbooks) {
        out += "- ";
        out += book.id;
        if (!book.useWhen.empty()) {
            out += " — ";
            out += book.useWhen;
        }
        out += '\n';
    }
    return out;
}

const Playbook* findPlaybook(const PromptPack& pack, std::string_view id) {
    const auto it = std::find_if(
        pack.playbooks.begin(), pack.playbooks.end(),
        [id](const Playbook& book) { return book.id == id; });
    return it == pack.playbooks.end() ? nullptr : &*it;
}

std::vector<std::string> playbookIds(const PromptPack& pack) {
    std::vector<std::string> ids;
    ids.reserve(pack.playbooks.size());
    for (const Playbook& book : pack.playbooks) ids.push_back(book.id);
    return ids;
}

} // namespace daw::ai
