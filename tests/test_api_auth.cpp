// Phase 5-A: API key auth — parsing, constant-time check semantics, and the
// disabled-by-default contract (AGENT_API_KEYS empty => everything passes).

#include "agent/api_auth.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace agent;

namespace {

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

TEST(ApiAuth, DisabledWhenNoKeys) {
    ApiAuth auth;
    EXPECT_FALSE(auth.Enabled());
    EXPECT_TRUE(auth.Check(""));           // anything passes when disabled
    EXPECT_TRUE(auth.Check("whatever"));
}

TEST(ApiAuth, AcceptsConfiguredKey) {
    ApiAuth auth({"test-key"});
    EXPECT_TRUE(auth.Enabled());
    EXPECT_TRUE(auth.Check("test-key"));
}

TEST(ApiAuth, RejectsWrongAndEmptyAndPrefix) {
    ApiAuth auth({"test-key"});
    EXPECT_FALSE(auth.Check("wrong-key"));
    EXPECT_FALSE(auth.Check(""));
    EXPECT_FALSE(auth.Check("test"));      // prefix of the real key
    EXPECT_FALSE(auth.Check("test-key-extra"));
}

TEST(ApiAuth, MultipleKeysAnyMatch) {
    ApiAuth auth({"k1", "k2"});
    EXPECT_TRUE(auth.Check("k1"));
    EXPECT_TRUE(auth.Check("k2"));
    EXPECT_FALSE(auth.Check("k3"));
}

TEST(ApiAuth, FromEnvParsesCommaSeparatedWithTrim) {
    SetEnv("AGENT_API_KEYS", " alpha , ,beta ,");
    ApiAuth auth = ApiAuth::FromEnv();
    EXPECT_TRUE(auth.Enabled());
    EXPECT_TRUE(auth.Check("alpha"));
    EXPECT_TRUE(auth.Check("beta"));
    EXPECT_FALSE(auth.Check(" alpha"));    // entries were trimmed
    EXPECT_FALSE(auth.Check(""));
}

TEST(ApiAuth, FromEnvEmptyDisables) {
    SetEnv("AGENT_API_KEYS", "");
    ApiAuth auth = ApiAuth::FromEnv();
    EXPECT_FALSE(auth.Enabled());
    EXPECT_TRUE(auth.Check(""));
    SetEnv("AGENT_API_KEYS", "  , , ");
    EXPECT_FALSE(ApiAuth::FromEnv().Enabled());
}
