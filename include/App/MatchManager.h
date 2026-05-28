#pragma once

// =============================================================================
// FightSight - Match Manager (Phase 12)
// -----------------------------------------------------------------------------
// Global state machine that gates which ROI groups the CV worker is allowed
// to process at any given moment. The match flows through three phases:
//
//     WAITING   - lobby / character select / loading - few ROIs matter
//     IN_MATCH  - active gameplay - health bars, combo counters, etc.
//     RESULT    - K.O. / round-end screen - winner detection, etc.
//
// The user maps group names (RoiData.group_name) to states via the Match
// Manager dashboard. When a fire on a state-transition ROI happens (an ROI
// with triggers_state_transition set), the global current_state is updated
// atomically; the NEXT CV cycle uses the new state's allowed-groups set.
//
// Filtering is opt-in: when MatchManager.IsFilteringEnabled() is false
// (default), the CV worker processes every ROI regardless of group - so
// existing profiles keep working until the user explicitly enables the
// gate from the dashboard.
// =============================================================================

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace fightsight {

// On-disk schema int values - do NOT renumber.
enum class MatchState : int {
    Waiting = 0,
    InMatch = 1,
    Result  = 2,
};

inline const char* MatchStateName(MatchState s) {
    switch (s) {
        case MatchState::Waiting: return "WAITING";
        case MatchState::InMatch: return "IN_MATCH";
        case MatchState::Result:  return "RESULT";
    }
    return "WAITING";
}

inline MatchState MatchStateFromName(const std::string& s) {
    if (s == "IN_MATCH" || s == "InMatch") return MatchState::InMatch;
    if (s == "RESULT"   || s == "Result")  return MatchState::Result;
    return MatchState::Waiting;
}

// Iteration order for UI tables / dropdowns.
inline constexpr int kMatchStateCount = 3;
inline constexpr MatchState kAllMatchStates[kMatchStateCount] = {
    MatchState::Waiting,
    MatchState::InMatch,
    MatchState::Result,
};

// =============================================================================
// MatchManager
// -----------------------------------------------------------------------------
// Thread-safe shared object owned by main() and referenced by CvWorker.
//   * current_state           - atomic, both threads read/write
//   * active_groups           - guarded by m_mutex
//   * m_filterEnabled         - atomic, master on/off for the gate
//
// The CV worker takes a SnapshotAllowedGroups() at the top of each cycle so
// it doesn't hold the mutex during processing. The UI thread mutates
// active_groups directly through SetGroupActiveIn() (each call grabs the
// mutex briefly).
// =============================================================================
class MatchManager {
public:
    MatchManager() = default;

    // ---- Current state (atomic) -----------------------------------------
    MatchState GetCurrentState() const { return m_currentState.load(); }
    void       SetCurrentState(MatchState s) { m_currentState.store(s); }

    // ---- Master on/off (atomic) -----------------------------------------
    // When false, the CV worker processes every ROI regardless of group -
    // keeping pre-Phase-12 behavior intact for users who haven't filled in
    // the active-groups table yet.
    bool IsFilteringEnabled() const { return m_filterEnabled.load(); }
    void SetFilteringEnabled(bool b) { m_filterEnabled.store(b); }

    // ---- CV-thread fast path --------------------------------------------
    // Returns the group names allowed in the CURRENT state, as an
    // unordered_set for O(1) membership in the cycle loop. Grabs the
    // mutex briefly to copy out the vector.
    std::unordered_set<std::string> SnapshotAllowedGroups() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        const MatchState s = m_currentState.load();
        auto it = m_activeGroups.find(s);
        if (it == m_activeGroups.end()) return {};
        return std::unordered_set<std::string>(
            it->second.begin(), it->second.end());
    }

    // ---- UI-thread accessors --------------------------------------------
    bool IsGroupActiveIn(MatchState s, const std::string& g) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_activeGroups.find(s);
        if (it == m_activeGroups.end()) return false;
        return std::find(it->second.begin(), it->second.end(), g)
               != it->second.end();
    }

    void SetGroupActiveIn(MatchState s, const std::string& g, bool on) {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto& vec = m_activeGroups[s];
        auto it = std::find(vec.begin(), vec.end(), g);
        if (on && it == vec.end()) {
            vec.push_back(g);
        } else if (!on && it != vec.end()) {
            vec.erase(it);
        }
    }

    // ---- Persistence ----------------------------------------------------
    nlohmann::json ToJson() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        nlohmann::json groups = nlohmann::json::object();
        for (const auto& kv : m_activeGroups) {
            groups[MatchStateName(kv.first)] = kv.second;
        }
        return {
            {"filter_enabled", m_filterEnabled.load()},
            {"active_groups",  groups},
        };
    }

    void FromJson(const nlohmann::json& j) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_activeGroups.clear();
        if (!j.is_object()) return;
        m_filterEnabled.store(j.value("filter_enabled", false));
        if (j.contains("active_groups") && j["active_groups"].is_object()) {
            const auto& g = j["active_groups"];
            for (auto it = g.begin(); it != g.end(); ++it) {
                const MatchState s = MatchStateFromName(it.key());
                if (!it.value().is_array()) continue;
                std::vector<std::string> v;
                for (const auto& e : it.value()) {
                    if (e.is_string()) v.push_back(e.get<std::string>());
                }
                m_activeGroups[s] = std::move(v);
            }
        }
    }

private:
    std::atomic<MatchState> m_currentState{MatchState::Waiting};
    std::atomic<bool>       m_filterEnabled{false};
    mutable std::mutex      m_mutex;
    std::map<MatchState, std::vector<std::string>> m_activeGroups;
};

} // namespace fightsight
