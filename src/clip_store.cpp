#include "clip_store.h"

#include <windows.h>
#include <dpapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <utility>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr std::uint32_t kFileMagic = 0x31505443; // CTP1
constexpr std::uint32_t kDataVersion = 1;
constexpr std::uint32_t kMaxTextChars = 1'048'576;

std::uint64_t Now() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

std::filesystem::path DataPath() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData))) {
        return {};
    }
    std::filesystem::path folder(localAppData);
    CoTaskMemFree(localAppData);
    folder /= L"ClipboardTool";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return folder / L"clips.dat";
}

template <typename T>
void Append(std::vector<std::byte>& output, const T& value) {
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    output.insert(output.end(), first, first + sizeof(T));
}

void AppendEntry(std::vector<std::byte>& output, const ClipEntry& entry) {
    Append(output, entry.id);
    Append(output, entry.createdAt);
    const auto length = static_cast<std::uint32_t>(entry.text.size());
    Append(output, length);
    const auto* first = reinterpret_cast<const std::byte*>(entry.text.data());
    output.insert(output.end(), first, first + entry.text.size() * sizeof(wchar_t));
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename T>
    bool Read(T& value) {
        if (offset_ > bytes_.size() || sizeof(T) > bytes_.size() - offset_) {
            return false;
        }
        std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }

    bool ReadEntry(ClipEntry& entry) {
        std::uint32_t length = 0;
        if (!Read(entry.id) || !Read(entry.createdAt) || !Read(length) || length > kMaxTextChars) {
            return false;
        }
        const std::size_t byteCount = static_cast<std::size_t>(length) * sizeof(wchar_t);
        if (offset_ > bytes_.size() || byteCount > bytes_.size() - offset_) {
            return false;
        }
        entry.text.assign(reinterpret_cast<const wchar_t*>(bytes_.data() + offset_), length);
        offset_ += byteCount;
        return true;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    const bool valid = GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
                       size.QuadPart <= std::numeric_limits<DWORD>::max();
    if (!valid) {
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                    read == bytes.size();
    CloseHandle(file);
    return ok;
}

bool WriteFileBytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    const auto temporary = path.wstring() + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

ClipEntry MakeEntry(std::wstring text) {
    static std::uint64_t sequence = 0;
    const auto now = Now();
    return {now + (++sequence), now, std::move(text)};
}

bool IsUsableText(const std::wstring& text) {
    return !text.empty() && text.size() <= kMaxTextChars;
}

} // namespace

bool ClipStore::Load() {
    const auto path = DataPath();
    std::vector<std::byte> encrypted;
    if (path.empty() || !ReadFileBytes(path, encrypted)) {
        return true;
    }

    DATA_BLOB input{static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE*>(encrypted.data())};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return false;
    }

    Reader reader(std::span<const std::byte>(reinterpret_cast<const std::byte*>(output.pbData), output.cbData));
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t historyCount = 0;
    std::uint32_t favoriteCount = 0;
    const bool headerOk = reader.Read(magic) && reader.Read(version) && reader.Read(historyCount) &&
                          reader.Read(favoriteCount) && magic == kFileMagic && version == kDataVersion &&
                          historyCount <= kAbsoluteMaxEntries && favoriteCount <= kAbsoluteMaxEntries;
    if (!headerOk) {
        LocalFree(output.pbData);
        return false;
    }

    std::vector<ClipEntry> history;
    std::vector<ClipEntry> favorites;
    history.reserve(historyCount);
    favorites.reserve(favoriteCount);
    bool ok = true;
    for (std::uint32_t index = 0; index < historyCount && ok; ++index) {
        ClipEntry entry;
        ok = reader.ReadEntry(entry);
        if (ok) history.push_back(std::move(entry));
    }
    for (std::uint32_t index = 0; index < favoriteCount && ok; ++index) {
        ClipEntry entry;
        ok = reader.ReadEntry(entry);
        if (ok) favorites.push_back(std::move(entry));
    }
    LocalFree(output.pbData);
    if (!ok) {
        return false;
    }
    history_ = std::move(history);
    favorites_ = std::move(favorites);
    if (history_.size() > historyLimit_) history_.resize(historyLimit_);
    if (favorites_.size() > favoriteLimit_) favorites_.resize(favoriteLimit_);
    return true;
}

bool ClipStore::Save() const {
    std::vector<std::byte> plain;
    plain.reserve(128);
    Append(plain, kFileMagic);
    Append(plain, kDataVersion);
    Append(plain, static_cast<std::uint32_t>(history_.size()));
    Append(plain, static_cast<std::uint32_t>(favorites_.size()));
    for (const auto& entry : history_) AppendEntry(plain, entry);
    for (const auto& entry : favorites_) AppendEntry(plain, entry);

    DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"ClipboardTool data", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return false;
    }
    const auto path = DataPath();
    const bool ok = !path.empty() && WriteFileBytes(
        path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(output.pbData), output.cbData));
    LocalFree(output.pbData);
    return ok;
}

bool ClipStore::AddHistory(std::wstring text) {
    if (!IsUsableText(text)) return false;
    const auto duplicate = std::find_if(history_.begin(), history_.end(), [&](const ClipEntry& entry) {
        return entry.text == text;
    });
    if (duplicate != history_.end()) history_.erase(duplicate);
    history_.insert(history_.begin(), MakeEntry(std::move(text)));
    if (history_.size() > historyLimit_) history_.resize(historyLimit_);
    return true;
}

bool ClipStore::AddFavoriteFromHistory(std::size_t index) {
    if (index >= history_.size()) return false;
    return AddFavorite(history_[index].text);
}

bool ClipStore::AddFavorite(std::wstring text) {
    if (!IsUsableText(text)) return false;
    const auto duplicate = std::find_if(favorites_.begin(), favorites_.end(), [&](const ClipEntry& entry) {
        return entry.text == text;
    });
    if (duplicate != favorites_.end()) {
        std::rotate(favorites_.begin(), duplicate, duplicate + 1);
        return true;
    }
    favorites_.insert(favorites_.begin(), MakeEntry(std::move(text)));
    if (favorites_.size() > favoriteLimit_) favorites_.resize(favoriteLimit_);
    return true;
}

bool ClipStore::UpdateFavorite(std::size_t index, std::wstring text) {
    if (index >= favorites_.size() || !IsUsableText(text)) return false;
    favorites_[index].text = std::move(text);
    return true;
}

bool ClipStore::DeleteHistory(std::size_t index) {
    if (index >= history_.size()) return false;
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool ClipStore::DeleteFavorite(std::size_t index) {
    if (index >= favorites_.size()) return false;
    favorites_.erase(favorites_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool ClipStore::MoveFavorite(std::size_t from, std::size_t to) {
    if (from >= favorites_.size() || to >= favorites_.size()) return false;
    if (from == to) return true;
    ClipEntry entry = std::move(favorites_[from]);
    favorites_.erase(favorites_.begin() + static_cast<std::ptrdiff_t>(from));
    favorites_.insert(favorites_.begin() + static_cast<std::ptrdiff_t>(to), std::move(entry));
    return true;
}

void ClipStore::ClearHistory() {
    history_.clear();
}

void ClipStore::ClearFavorites() {
    favorites_.clear();
}

void ClipStore::SetLimits(std::size_t historyLimit, std::size_t favoriteLimit) {
    historyLimit_ = std::clamp(historyLimit, std::size_t{1}, kAbsoluteMaxEntries);
    favoriteLimit_ = std::clamp(favoriteLimit, std::size_t{1}, kAbsoluteMaxEntries);
    if (history_.size() > historyLimit_) history_.resize(historyLimit_);
    if (favorites_.size() > favoriteLimit_) favorites_.resize(favoriteLimit_);
}
