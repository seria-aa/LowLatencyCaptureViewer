#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <cwchar>

namespace llcv::diagnostics {
// Aliases are local to one report, not stable cross-report fingerprints. Keep
// media-subtype GUIDs and VID/PID/model information: those are not serials.
class ReportDevicePrivacy {
  public:
    explicit ReportDevicePrivacy(const std::vector<std::wstring>& known) {
        for (const auto& id : known) {
            if (id.empty())
                continue;
            const auto alias = Alias(DeviceKey(id));
            rules_.emplace_back(id, alias);
            const auto start = Fold(id).find(L"\\\\?\\");
            if (start != std::wstring::npos)
                rules_.emplace_back(id.substr(start), alias);
        }
    }
    const auto& Rules() const { return rules_; }
    std::wstring Apply(std::wstring text) {
        // Native DirectShow / PnP symbolic paths: type#hardware#instance#{class}.
        auto folded = Fold(text);
        size_t offset = 0;
        while ((offset = folded.find(L"\\\\?\\", offset)) != std::wstring::npos) {
            const auto a = folded.find(L'#', offset + 4),
                       b = a == std::wstring::npos ? a : folded.find(L'#', a + 1);
            const auto c = b == std::wstring::npos ? b : folded.find(L'#', b + 1);
            const auto boundary = folded.find_first_of(L"\r\n\t \"'<>", offset + 4);
            if (a == std::wstring::npos || b == std::wstring::npos || c == std::wstring::npos ||
                (boundary != std::wstring::npos && boundary < c) || c - offset > 2048 ||
                c + 1 >= text.size() || text[c + 1] != L'{' ||
                !DeviceType(folded.substr(offset + 4, a - offset - 4))) {
                offset += 4;
                continue;
            }
            const auto alias = Alias(folded.substr(offset + 4, c - offset - 4));
            Replace(text, folded, b + 1, c - b - 1, alias);
            offset = b + 1 + alias.size();
        }
        // MMDevice endpoint identity; leave unrelated codec/color GUIDs alone.
        offset = 0;
        while ((offset = folded.find(L"{0.", offset)) != std::wstring::npos) {
            const auto split = folded.find(L"}.{", offset),
                       end = split == std::wstring::npos ? split : folded.find(L'}', split + 3);
            if (split == std::wstring::npos || split - offset > 32 || end == std::wstring::npos ||
                !Guid(folded.substr(split + 3, end - split - 3))) {
                offset += 3;
                continue;
            }
            const auto alias = Alias(folded.substr(offset, end - offset + 1));
            Replace(text, folded, split + 2, end - split - 1, alias);
            offset = split + 2 + alias.size();
        }
        // Device-manager instance IDs sometimes appear without the symbolic-path prefix.
        for (const auto* prefix : {L"USB\\VID_", L"PCI\\VEN_", L"HDAUDIO\\FUNC_"}) {
            offset = 0;
            while ((offset = folded.find(prefix, offset)) != std::wstring::npos) {
                const auto slash = folded.find(L'\\', offset + wcslen(prefix));
                const auto boundary = folded.find_first_of(L"\r\n\t \"'<>#", offset);
                if (slash == std::wstring::npos || slash - offset > 512 ||
                    (boundary != std::wstring::npos && boundary < slash)) {
                    ++offset;
                    continue;
                }
                const auto end = folded.find_first_of(L"\r\n\t \"'<>#", slash + 1);
                const auto length = (end == std::wstring::npos ? folded.size() : end) - slash - 1;
                if (!length || length > 512) {
                    ++offset;
                    continue;
                }
                auto key = folded.substr(offset, slash - offset + 1 + length);
                std::replace(key.begin(), key.end(), L'\\', L'#');
                const auto alias = Alias(key);
                Replace(text, folded, slash + 1, length, alias);
                offset = slash + 1 + alias.size();
            }
        }
        return text;
    }

  private:
    static std::wstring Fold(std::wstring text) {
        for (auto& c : text) {
            if (c == L'/')
                c = L'\\';
            else if (c >= L'a' && c <= L'z')
                c -= L'a' - L'A';
        }
        return text;
    }
    static bool DeviceType(const std::wstring& type) {
        return type == L"USB" || type == L"PCI" || type == L"ROOT" || type == L"SWD" ||
               type == L"DISPLAY" || type == L"HDAUDIO" || type == L"BTHENUM";
    }
    static bool Guid(const std::wstring& value) {
        if (value.size() != 36)
            return false;
        for (size_t i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (value[i] != L'-')
                    return false;
            } else if (!((value[i] >= L'0' && value[i] <= L'9') ||
                         (value[i] >= L'A' && value[i] <= L'F')))
                return false;
        }
        return true;
    }
    static std::wstring DeviceKey(const std::wstring& id) {
        auto key = Fold(id);
        const auto start = key.find(L"\\\\?\\");
        if (start != std::wstring::npos) {
            key = key.substr(start + 4);
            const auto end = key.find(L"#{");
            if (end != std::wstring::npos)
                key.resize(end);
        }
        return key;
    }
    std::wstring Alias(const std::wstring& key) {
        const auto [it, inserted] = aliases_.try_emplace(key);
        if (inserted)
            it->second = L"<DEVICE_" + std::to_wstring(aliases_.size()) + L">";
        return it->second;
    }
    static void Replace(std::wstring& original, std::wstring& folded, size_t start, size_t count,
                        const std::wstring& to) {
        original.replace(start, count, to);
        folded.replace(start, count, Fold(to));
    }
    std::map<std::wstring, std::wstring> aliases_;
    std::vector<std::pair<std::wstring, std::wstring>> rules_;
};
} // namespace llcv::diagnostics
