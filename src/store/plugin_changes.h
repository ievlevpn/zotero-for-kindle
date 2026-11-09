#pragma once
// A change a plugin asked of the Zotero library (zk.zotero.*): kept by the Library until the sync
// engine has sent it, applied to the local copy at once, cancellable while unsent.

#include <cstdint>
#include <string>

namespace zk {

struct PluginChange {
    enum class Kind { CreateItem, AddToCollection, AddTag, AddNote, Trash, RemoveFromCollection, RemoveTag, DeleteNote };
    // Pending: not sent yet (applied locally). Sent: Zotero has it. Refused: Zotero said no (or its
    // item went). Cancelled: the plugin withdrew it (or its item was withdrawn). Declined: the user
    // said no to a removal (a log entry; never applied).
    enum class State { Pending, Sent, Refused, Cancelled, Declined };

    std::string id, pluginId;  // id: made by Library::queuePluginChange
    Kind kind = Kind::CreateItem;
    // The key of what the change made or changed (zk.zotero's Change.key): the new item or note
    // (made by the Library for CreateItem and AddNote), the item tagged, filed, trashed or taken out
    // of a tag or collection, the note deleted.
    std::string itemKey;
    // CreateItem: {"fields": ItemFields as zk.d.ts has them, "collections": [...], "tags": [...]}
    // (JSON). AddToCollection, RemoveFromCollection: the collection key. AddTag, RemoveTag: the tag.
    // AddNote: the note's HTML. Trash, DeleteNote: "".
    std::string value;
    std::string parentKey;  // AddNote: the item the note goes on
    // The pending change that creates what this one targets (its item, or its note's parent item),
    // "" if none: it is sent after that one, and withdrawn with it.
    std::string dependsOn;
    State state = State::Pending;
    std::string message;  // why it was refused, ...
    int64_t createdMs = 0, sentMs = 0;
};

// As zk.d.ts names them ("createItem", ...; "pending", "sent", ...); stored so in the database.
inline const char* kindName(PluginChange::Kind k) {
    switch (k) {
        case PluginChange::Kind::CreateItem: return "createItem";
        case PluginChange::Kind::AddToCollection: return "addToCollection";
        case PluginChange::Kind::AddTag: return "addTag";
        case PluginChange::Kind::AddNote: return "addNote";
        case PluginChange::Kind::Trash: return "trash";
        case PluginChange::Kind::RemoveFromCollection: return "removeFromCollection";
        case PluginChange::Kind::RemoveTag: return "removeTag";
        case PluginChange::Kind::DeleteNote: return "deleteNote";
    }
    return "";
}

inline const char* stateName(PluginChange::State s) {
    switch (s) {
        case PluginChange::State::Pending: return "pending";
        case PluginChange::State::Sent: return "sent";
        case PluginChange::State::Refused: return "refused";
        case PluginChange::State::Cancelled: return "cancelled";
        case PluginChange::State::Declined: return "declined";
    }
    return "";
}

inline bool kindFromName(const std::string& s, PluginChange::Kind& out) {
    for (int i = 0; i <= static_cast<int>(PluginChange::Kind::DeleteNote); ++i)
        if (s == kindName(static_cast<PluginChange::Kind>(i))) return out = static_cast<PluginChange::Kind>(i), true;
    return false;
}

inline bool stateFromName(const std::string& s, PluginChange::State& out) {
    for (int i = 0; i <= static_cast<int>(PluginChange::State::Declined); ++i)
        if (s == stateName(static_cast<PluginChange::State>(i))) return out = static_cast<PluginChange::State>(i), true;
    return false;
}

}  // namespace zk
