#include "preset_store.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

namespace BetterCheats::PresetStore
{
    namespace
    {
        struct StoredField
        {
            std::string key;
            float value;
        };

        struct StoredPreset
        {
            std::string group;
            std::string name;
            std::vector<StoredField> fields;
        };

        std::vector<StoredPreset> g_presets;
        std::string g_filePath;
        bool g_haveFilePath = false;

        StoredPreset* Find(const char* group, const char* name)
        {
            for (auto& p : g_presets)
                if (p.group == group && p.name == name)
                    return &p;
            return nullptr;
        }

        // "[group:name]" -> group, name. Splits on the first ':' -- group
        // identifiers must not themselves contain one.
        bool SplitSection(const std::string& section, std::string& outGroup, std::string& outName)
        {
            const size_t colon = section.find(':');
            if (colon == std::string::npos)
                return false;
            outGroup = section.substr(0, colon);
            outName  = section.substr(colon + 1);
            return !outGroup.empty() && !outName.empty();
        }

        // Reads the whole file into a byte string. Returns empty (not an
        // error) if the file doesn't exist yet.
        std::string ReadWholeFile(const char* path)
        {
            std::string content;
            FILE* f = nullptr;
            if (fopen_s(&f, path, "rb") != 0 || !f)
                return content;

            fseek(f, 0, SEEK_END);
            const long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (size > 0)
            {
                content.resize(static_cast<size_t>(size));
                const size_t read = fread(&content[0], 1, static_cast<size_t>(size), f);
                content.resize(read);
            }
            fclose(f);
            return content;
        }

        // Line-based parser: "[group:name]" headers, "key=value" lines under
        // them, blank lines and ';'/'#' comments ignored. Populates
        // g_presets from scratch.
        void ParseFile(const std::string& content)
        {
            g_presets.clear();

            StoredPreset* current = nullptr;
            size_t pos = 0;
            while (pos < content.size())
            {
                size_t eol = content.find_first_of("\r\n", pos);
                std::string line = (eol == std::string::npos) ? content.substr(pos) : content.substr(pos, eol - pos);
                pos = (eol == std::string::npos) ? content.size() : eol + 1;

                const size_t start = line.find_first_not_of(" \t");
                if (start == std::string::npos)
                    continue;
                line = line.substr(start);
                if (line.empty() || line[0] == ';' || line[0] == '#')
                    continue;

                if (line[0] == '[')
                {
                    const size_t close = line.find(']');
                    if (close == std::string::npos)
                        continue;
                    std::string group, name;
                    if (SplitSection(line.substr(1, close - 1), group, name))
                    {
                        g_presets.push_back({ std::move(group), std::move(name), {} });
                        current = &g_presets.back();
                    }
                    else
                    {
                        current = nullptr;
                    }
                    continue;
                }

                if (!current)
                    continue;

                const size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;

                std::string key = line.substr(0, eq);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                    key.pop_back();
                if (key.empty())
                    continue;

                const std::string valueStr = line.substr(eq + 1);
                current->fields.push_back({ std::move(key), static_cast<float>(atof(valueStr.c_str())) });
            }
        }

        // Regenerates the whole file from g_presets and writes it in one
        // shot. Called only from Save/Rename/Delete, never per-frame.
        bool WriteWholeFile()
        {
            if (!g_haveFilePath)
                return false;

            std::string content;
            for (const auto& p : g_presets)
            {
                content += "[";
                content += p.group;
                content += ":";
                content += p.name;
                content += "]\r\n";

                for (const auto& f : p.fields)
                {
                    char valStr[64];
                    snprintf(valStr, sizeof(valStr), "%.6f", f.value);
                    content += f.key;
                    content += "=";
                    content += valStr;
                    content += "\r\n";
                }
                content += "\r\n";
            }

            FILE* f = nullptr;
            if (fopen_s(&f, g_filePath.c_str(), "wb") != 0 || !f)
                return false;
            fwrite(content.data(), 1, content.size(), f);
            fclose(f);
            return true;
        }
    }

    bool Init(const char* filePath)
    {
        if (!filePath || !filePath[0])
            return false;

        g_filePath    = filePath;
        g_haveFilePath = true;

        ParseFile(ReadWholeFile(filePath));
        return true;
    }

    int ListNames(const char* group, char outNames[][kMaxNameLen], int cap)
    {
        if (!group || !outNames || cap <= 0)
            return 0;

        std::vector<const std::string*> names;
        for (const auto& p : g_presets)
            if (p.group == group)
                names.push_back(&p.name);

        std::sort(names.begin(), names.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });

        int written = 0;
        for (const auto* n : names)
        {
            if (written >= cap)
                break;
            snprintf(outNames[written], kMaxNameLen, "%s", n->c_str());
            ++written;
        }
        return written;
    }

    bool Save(const char* group, const char* name, const Field* fields, int count)
    {
        if (!g_haveFilePath || !group || !name || !name[0] || (!fields && count > 0))
            return false;

        StoredPreset* existing = Find(group, name);
        if (!existing)
        {
            g_presets.push_back({ group, name, {} });
            existing = &g_presets.back();
        }

        existing->fields.clear();
        for (int i = 0; i < count; ++i)
            existing->fields.push_back({ fields[i].key, fields[i].value });

        return WriteWholeFile();
    }

    bool Load(const char* group, const char* name, Field* out, int count)
    {
        if (!group || !name)
            return false;

        const StoredPreset* preset = Find(group, name);
        if (!preset)
            return false;

        for (int i = 0; i < count; ++i)
        {
            if (!out[i].key)
                continue;
            for (const auto& f : preset->fields)
            {
                if (f.key == out[i].key)
                {
                    out[i].value = f.value;
                    break;
                }
            }
        }
        return true;
    }

    bool Rename(const char* group, const char* from, const char* to)
    {
        if (!g_haveFilePath || !group || !from || !to || !to[0])
            return false;
        if (strcmp(from, to) == 0)
            return true; // no-op, not an error

        StoredPreset* existing = Find(group, from);
        if (!existing)
            return false;
        if (Find(group, to))
            return false; // name taken

        existing->name = to;
        return WriteWholeFile();
    }

    bool Delete(const char* group, const char* name)
    {
        if (!g_haveFilePath || !group || !name)
            return false;

        const size_t before = g_presets.size();
        g_presets.erase(
            std::remove_if(g_presets.begin(), g_presets.end(),
                            [&](const StoredPreset& p) { return p.group == group && p.name == name; }),
            g_presets.end());

        if (g_presets.size() == before)
            return false; // nothing matched

        return WriteWholeFile();
    }

    void SuggestName(const char* group, const char* baseName, char* out, int cap)
    {
        if (!out || cap <= 0)
            return;
        out[0] = '\0';
        if (!group || !baseName || !baseName[0])
            return;

        if (!Find(group, baseName))
        {
            snprintf(out, cap, "%s", baseName);
            return;
        }

        // baseName is taken -- try "baseName 2", "baseName 3", ... A bound
        // well past any realistic number of saves rather than looping
        // forever if something upstream keeps producing collisions.
        for (int suffix = 2; suffix < 1000; ++suffix)
        {
            char candidate[kMaxNameLen];
            snprintf(candidate, sizeof(candidate), "%s %d", baseName, suffix);
            if (!Find(group, candidate))
            {
                snprintf(out, cap, "%s", candidate);
                return;
            }
        }

        // Exhausted the bound above -- fall back to the bare base name
        // rather than return nothing.
        snprintf(out, cap, "%s", baseName);
    }
}
