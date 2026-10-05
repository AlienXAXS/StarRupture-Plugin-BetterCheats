#pragma once
#include <cstddef>

// Generic named-preset storage for a plugin's own tunable settings, backed
// by one INI file next to the plugin's other config (e.g. Plugins\config\
// BetterCheats-Presets.ini). One section per preset, named "<group>:<name>",
// so several independent groups of fields can share a single file --
// BetterDrone has "Speed" and "Range"; BetterCheats has "Movement" and one
// group per weapon type.
//
// Pure data: no UObject/SDK access of any kind, and no per-frame file I/O
// -- Init reads the whole file into memory once, and every other call works
// off that in-memory copy; only Save/Rename/Delete write back to disk, and
// each does a single full-file rewrite (presets files are small; this is
// simpler and safer than patching sections in place).
//
// This store has no notion of "built-in" presets -- it only ever holds what
// a caller explicitly Saves. Refusing to Save/Rename/Delete onto a built-in
// preset's name is the caller's job, since only the caller knows what its
// built-ins are called.
//
// Not internally synchronized: call every function from one thread only.
// Both BetterDrone and BetterCheats drive this purely from UI interaction
// (dropdown/button clicks), so every call here happens on the ImGui render
// thread (the same thread PluginPanelDesc::renderFn runs on) -- never from
// the game tick, and never concurrently with itself.
//
// Shared verbatim between BetterDrone and BetterCheats: this pair of files
// (preset_store.h/.cpp) is meant to be copied as-is into a new plugin, with
// only the enclosing namespace renamed to match and the caller supplying
// its own group names and Field sets. Nothing plugin-specific belongs here.
namespace BetterCheats::PresetStore
{
    // One named float value within a preset. A caller builds an array of
    // these per group -- e.g. Movement's is every tunable row's key/value,
    // a weapon group's is that weapon's own kAttrs keys/values.
    struct Field
    {
        const char* key;
        float value;
    };

    constexpr int kMaxNameLen = 64;

    // Loads filePath into memory if it exists; a missing file is not an
    // error, the store just starts empty (and is created on the first
    // Save). Call once, before any other function here.
    bool Init(const char* filePath);

    // Fills outNames with up to cap preset names in group, alphabetical
    // order. Returns the number written.
    int ListNames(const char* group, char outNames[][kMaxNameLen], int cap);

    // Writes fields as a preset named name in group, creating it or
    // overwriting an existing stored preset of that name, then persists the
    // whole file. Always succeeds unless filePath was never set via Init.
    bool Save(const char* group, const char* name, const Field* fields, int count);

    // Fills out[0..count)'s value from the stored preset name in group,
    // matched by out[i].key. A field the stored preset doesn't have is left
    // untouched, so callers should seed out with current/default values
    // first. Returns false if no such preset exists in group.
    bool Load(const char* group, const char* name, Field* out, int count);

    // Renames a stored preset. Fails and leaves the store unchanged if from
    // doesn't exist in group or to is already taken there.
    bool Rename(const char* group, const char* from, const char* to);

    // Deletes a stored preset. Returns false if it doesn't exist in group.
    bool Delete(const char* group, const char* name);

    // Fills out with baseName, or baseName + " 2", " 3", ... -- the first
    // spelling that isn't already a stored preset name in group. Callers
    // build baseName themselves (e.g. "<matched built-in> Custom", or plain
    // "Custom" when nothing recognizable matched); this only handles making
    // it unique. Never fails; truncates to cap if baseName is already long.
    void SuggestName(const char* group, const char* baseName, char* out, int cap);
}
