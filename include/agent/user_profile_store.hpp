#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;  // forward declaration — header does not include sqlite3.h

namespace agent {

// Cross-session user profile (Phase 2.2). Aggregated from the user's
// feedback history and slot history by PreferenceExtractor, injected into
// the planner prompt as a "用户画像" section so returning users get fewer
// clarifying questions and better-personalized ranking features.
struct UserProfile {
    std::string user_id;
    std::vector<std::string> preferred_cities;
    std::vector<std::string> preferred_categories;
    double price_sensitivity = 0.5;   // 0..1: share of liked items with a deep discount
    std::vector<std::string> dietary_tags;  // historical taboo terms
    double avg_budget = 0.0;          // mean of budget slots seen (0 = unknown)
    std::string updated_at;           // ISO-8601 local time
};

// Raw signals the PreferenceExtractor aggregates from. Kept separate from
// the profile so the extraction rules stay a pure function (unit-testable).
struct FeedbackSignal {
    std::string item_id;
    std::string feedback_type;  // like / dislike
};
struct SlotRecord {
    double budget = 0.0;        // 0 = not set
    std::string taboo;
};

// Read/write interface for user profiles. Plain sync calls (single
// connection + mutex), same shape as ObservabilityStore.
class UserProfileStore {
public:
    virtual ~UserProfileStore() = default;

    // nullopt when the user has no profile yet (or the store is unusable).
    virtual std::optional<UserProfile> Get(const std::string& user_id) = 0;
    virtual bool Upsert(const UserProfile& p) = 0;

    // Signals for PreferenceExtractor: item-level feedback and slot history
    // (budget/taboo from each session's latest context) of one user.
    virtual std::vector<FeedbackSignal> LoadFeedbackSignals(const std::string& user_id) = 0;
    virtual std::vector<SlotRecord> LoadSlotHistory(const std::string& user_id) = 0;
};

// SQLite-backed store living in the SAME database file as sessions/feedback
// (default data/sessions.db) but on its own connection, so the
// SessionMemoryStore interface stays untouched. WAL + busy_timeout make the
// two connections coexist (same pattern as the observability ATTACH).
class SqliteUserProfileStore : public UserProfileStore {
public:
    // Opens db_path (creating it and the user_profiles table if needed).
    // Throws on failure — callers who want degrade-on-failure should catch.
    explicit SqliteUserProfileStore(const std::string& db_path);
    ~SqliteUserProfileStore() override;

    SqliteUserProfileStore(const SqliteUserProfileStore&) = delete;
    SqliteUserProfileStore& operator=(const SqliteUserProfileStore&) = delete;

    std::optional<UserProfile> Get(const std::string& user_id) override;
    bool Upsert(const UserProfile& p) override;
    std::vector<FeedbackSignal> LoadFeedbackSignals(const std::string& user_id) override;
    std::vector<SlotRecord> LoadSlotHistory(const std::string& user_id) override;

private:
    void InitSchema();

    std::mutex mutex_;
    sqlite3* db_ = nullptr;
};

} // namespace agent
