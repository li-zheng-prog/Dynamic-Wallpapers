#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using WallpaperSnapshot = std::vector<std::pair<std::wstring, std::wstring>>;

// Compare complete paths, including relative segments and alternate separators.
// Empty paths are meaningful (for example a solid-color desktop).
inline std::wstring NormalizeWallpaperPath(std::wstring path) {
    if (path.empty()) return path;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) path = L"\\\\" + path.substr(8);
    else if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4);
    DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (size) {
        std::vector<wchar_t> buffer(size);
        DWORD length = GetFullPathNameW(path.c_str(), size, buffer.data(), nullptr);
        if (length && length < size) path.assign(buffer.data(), length);
    }
    return path;
}

inline bool SameWallpaperPath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(NormalizeWallpaperPath(a).c_str(), NormalizeWallpaperPath(b).c_str()) == 0;
}

// All times are supplied by the caller so asynchronous shell behavior can be
// tested without sleeping or changing the real desktop.
struct WallpaperTracker {
    static constexpr ULONGLONG settleMs = 5000;
    static constexpr ULONGLONG confirmMs = 1000;
    static constexpr unsigned maxAttempts = 3;
    enum class Result { None, Confirmed, TimedOut, External };

    WallpaperSnapshot observed, before, candidate;
    std::wstring target;
    int style = -1;
    ULONGLONG generation = 0, started = 0, candidateSince = 0;
    unsigned attempts = 0;
    bool pending = false, awaitingConfirmation = false, readFailed = false, inconclusive = false;
    std::vector<std::pair<std::wstring, ULONGLONG>> recentTargets;

    static const std::wstring* Find(const WallpaperSnapshot& snapshot, const std::wstring& monitor) {
        for (const auto& item : snapshot)
            if (_wcsicmp(item.first.c_str(), monitor.c_str()) == 0) return &item.second;
        return nullptr;
    }

    static bool SameSnapshot(const WallpaperSnapshot& a, const WallpaperSnapshot& b) {
        if (a.size() != b.size()) return false;
        for (const auto& item : a) {
            const auto* other = Find(b, item.first);
            if (!other || !SameWallpaperPath(item.second, *other)) return false;
        }
        return true;
    }

    static bool AllMatch(const WallpaperSnapshot& snapshot, const std::wstring& image) {
        if (snapshot.empty()) return false;
        for (const auto& item : snapshot)
            if (!SameWallpaperPath(item.second, image)) return false;
        return true;
    }

    bool Blocked() const { return readFailed || inconclusive || !candidate.empty(); }
    bool NeedsPoll() const { return pending || !candidate.empty(); }
    bool SameRequest(const std::wstring& image, int position) const {
        return style == position && SameWallpaperPath(target, image);
    }
    bool CanRequest(const std::wstring& image, int position) const {
        return !Blocked() && (!SameRequest(image, position) || (!pending && attempts < maxAttempts));
    }

    void ResetSelection() {
        // Retain only short-lived evidence of superseded writes; no old request
        // may commit a result or resume automatic work for the new selection.
        ++generation;
        observed.clear(); before.clear(); candidate.clear(); target.clear();
        style = -1; attempts = 0;
        pending = awaitingConfirmation = readFailed = inconclusive = false;
    }

    void ReadFailed() {
        readFailed = true;
        candidate.clear(); // A failed sample cannot count as continuous confirmation.
    }

    ULONGLONG Begin(const std::wstring& image, int position, ULONGLONG now) {
        if (!SameRequest(image, position)) attempts = 0;
        ++attempts;
        ++generation;
        target = image; style = position; started = now;
        before = observed;
        pending = true;
        awaitingConfirmation = false;
        recentTargets.erase(std::remove_if(recentTargets.begin(), recentTargets.end(),
            [now](const auto& entry) { return entry.second <= now; }), recentTargets.end());
        recentTargets.emplace_back(image, now + settleMs);
        return generation;
    }

    void Applied(bool success) {
        awaitingConfirmation = success;
        if (!success) pending = false;
    }

    Result Observe(const WallpaperSnapshot& current, const std::wstring& day,
                   const std::wstring& night, ULONGLONG now) {
        if (current.empty()) { ReadFailed(); return Result::None; }
        readFailed = false;
        inconclusive = false;
        recentTargets.erase(std::remove_if(recentTargets.begin(), recentTargets.end(),
            [now](const auto& entry) { return entry.second <= now; }), recentTargets.end());
        WallpaperSnapshot strangers, accepted = current;
        for (auto& item : accepted) {
            const auto* previous = Find(observed, item.first);
            if (!previous) continue; // A new monitor has no comparison baseline.
            if (SameWallpaperPath(item.second, day) || SameWallpaperPath(item.second, night)) continue;
            // An empty path during our outstanding write is inconclusive. Keep
            // its previous baseline so a persistent solid color is checked later.
            if (pending && now - started < settleMs && item.second.empty()) {
                inconclusive = true;
                item.second = *previous;
                continue;
            }
            if (SameWallpaperPath(item.second, *previous)) continue;
            const auto* original = Find(before, item.first);
            if (pending && now - started < settleMs && original &&
                SameWallpaperPath(item.second, *original)) continue;
            bool ownWrite = false;
            for (const auto& recent : recentTargets)
                if (SameWallpaperPath(item.second, recent.first)) { ownWrite = true; break; }
            if (!ownWrite) strangers.push_back(item);
        }
        if (!strangers.empty()) {
            if (!SameSnapshot(strangers, candidate)) {
                candidate = std::move(strangers);
                candidateSince = now;
            } else if (now - candidateSince >= confirmMs) {
                return Result::External;
            }
            // Never adopt an unconfirmed external wallpaper as our baseline.
        } else {
            candidate.clear();
            observed = std::move(accepted);
        }
        if (awaitingConfirmation && candidate.empty() && AllMatch(current, target)) {
            pending = awaitingConfirmation = false;
            attempts = 0;
            return Result::Confirmed;
        }
        if (pending && now - started >= settleMs) {
            pending = false;
            return Result::TimedOut;
        }
        return Result::None;
    }
};
