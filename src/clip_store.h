#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ClipEntry {
    std::uint64_t id{};
    std::uint64_t createdAt{};
    std::wstring text;
};

class ClipStore {
public:
    bool Load();
    bool Save() const;

    bool AddHistory(std::wstring text);
    bool AddFavoriteFromHistory(std::size_t index);
    bool AddFavorite(std::wstring text);
    bool UpdateFavorite(std::size_t index, std::wstring text);
    bool DeleteHistory(std::size_t index);
    bool DeleteFavorite(std::size_t index);
    bool MoveFavorite(std::size_t from, std::size_t to);
    void ClearHistory();
    void ClearFavorites();
    void SetLimits(std::size_t historyLimit, std::size_t favoriteLimit);

    const std::vector<ClipEntry>& History() const { return history_; }
    const std::vector<ClipEntry>& Favorites() const { return favorites_; }
    std::size_t HistoryLimit() const { return historyLimit_; }
    std::size_t FavoriteLimit() const { return favoriteLimit_; }

    static constexpr std::size_t kDefaultHistoryLimit = 50;
    static constexpr std::size_t kDefaultFavoriteLimit = 100;
    static constexpr std::size_t kAbsoluteMaxEntries = 1000;

private:
    std::vector<ClipEntry> history_;
    std::vector<ClipEntry> favorites_;
    std::size_t historyLimit_{kDefaultHistoryLimit};
    std::size_t favoriteLimit_{kDefaultFavoriteLimit};
};
