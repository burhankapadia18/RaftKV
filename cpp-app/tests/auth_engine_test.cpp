/**
 * @file auth_engine_test.cpp
 * @brief Unit tests for auth::AuthEngine and auth::AuthContext.
 *
 * This is the authentication decision itself, driven against a fake IKVStore so
 * no cluster or gRPC is involved. The assertions that matter most are the
 * negative ones: an unknown user, a disabled user and a wrong password must all
 * come back the same way, and no path may hand out an identity it did not
 * verify.
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "auth/auth_engine.hpp"
#include "auth/base64.hpp"
#include "storage/kv_store.hpp"

namespace kvdb {
namespace auth {
namespace {

/** @brief Minimal in-memory IKVStore; only get() is exercised here. */
class FakeKVStore : public IKVStore {
public:
  void set(const std::string &key, const std::string &value) override {
    data[key] = value;
  }

  [[nodiscard]] std::optional<std::string>
  get(const std::string &key) const override {
    const auto it = data.find(key);
    return it == data.end() ? std::nullopt
                            : std::optional<std::string>(it->second);
  }

  bool remove(const std::string &key) override { return data.erase(key) > 0; }

  [[nodiscard]] bool contains(const std::string &key) const override {
    return data.count(key) > 0;
  }

  [[nodiscard]] StateMap snapshot_state() const override {
    return StateMap(data.begin(), data.end());
  }

  void restore_state(StateMap state) override {
    data.clear();
    data.insert(state.begin(), state.end());
  }

  std::map<std::string, std::string> data;
};

AuthOptions enabled_options(const std::string &admin_password) {
  AuthOptions options;
  options.enabled = true;
  options.admin_password = admin_password;
  return options;
}

/** @brief The header value a client sends for these credentials. */
std::string basic_header(const std::string &name, const std::string &password) {
  return "Basic " + base64_encode(name + ":" + password);
}

/** @brief Install a user in @p store and return the password it was given. */
void put_user(FakeKVStore &store, const std::string &name,
              const std::string &password,
              const std::vector<std::string> &classes,
              const std::vector<std::string> &patterns, bool enabled = true) {
  UserUpsertRequest request;
  request.password = password;
  request.enabled = enabled;
  request.classes = classes;
  request.patterns = patterns;
  const UserRecord record = request.to_record(name);
  store.data[user_storage_key(name)] = record.to_msgpack();
}

// --- AuthContext ----------------------------------------------------------

TEST(AuthContextTest, ClassesAreIndependentNotAHierarchy) {
  AuthContext context;
  context.admin = true;

  // Holding admin does NOT confer read or write. A user-administration account
  // has no business reading application data, and Redis' classes work the same
  // way — an ACL is a set, not a rank.
  EXPECT_TRUE(context.has_class(CommandClass::kAdmin));
  EXPECT_FALSE(context.has_class(CommandClass::kRead));
  EXPECT_FALSE(context.has_class(CommandClass::kWrite));
}

TEST(AuthContextTest, EmptyPatternListDeniesEveryKey) {
  // The safe direction for a default: a record that arrived with no patterns
  // grants nothing rather than everything.
  const AuthContext context;

  EXPECT_FALSE(context.key_allowed(""));
  EXPECT_FALSE(context.key_allowed("anything"));
}

TEST(AuthContextTest, AnyMatchingPatternAllowsTheKey) {
  AuthContext context;
  context.patterns = {"app:*", "shared:config"};

  EXPECT_TRUE(context.key_allowed("app:1"));
  EXPECT_TRUE(context.key_allowed("shared:config"));
  EXPECT_FALSE(context.key_allowed("other:1"));
  EXPECT_FALSE(context.key_allowed("shared:configs"));
}

TEST(AuthContextTest, UnrestrictedIsFullDataAccessButNotAdmin) {
  const AuthContext context = AuthContext::unrestricted();

  EXPECT_TRUE(context.has_class(CommandClass::kRead));
  EXPECT_TRUE(context.has_class(CommandClass::kWrite));
  // With auth off nobody can prove they are an administrator, so the
  // user-management API must stay closed rather than open to all.
  EXPECT_FALSE(context.has_class(CommandClass::kAdmin));
  EXPECT_TRUE(context.key_allowed("any:key"));
}

// --- Disabled engine ------------------------------------------------------

TEST(AuthEngineTest, IsDisabledWithoutAnAdminPassword) {
  FakeKVStore store;
  const AuthEngine engine(store, AuthOptions{});

  EXPECT_FALSE(engine.enabled());

  AuthContext context;
  EXPECT_EQ(engine.authenticate("", context), AuthOutcome::kOk);
  EXPECT_TRUE(context.has_class(CommandClass::kWrite));
  EXPECT_FALSE(context.has_class(CommandClass::kAdmin));
}

TEST(AuthEngineTest, FromEnvLeavesAuthOffWhenThePasswordIsUnset) {
  // AuthOptions::from_env reads the process environment; the assertion that
  // matters is that "absent" and "empty" both mean OFF, and neither means
  // "enabled with an empty password" — which would accept `admin:`.
  ::unsetenv("RAFTKV_ADMIN_PASSWORD");
  EXPECT_FALSE(AuthOptions::from_env().enabled);

  ::setenv("RAFTKV_ADMIN_PASSWORD", "", 1);
  EXPECT_FALSE(AuthOptions::from_env().enabled);

  ::setenv("RAFTKV_ADMIN_PASSWORD", "s3cr3t-admin-pw", 1);
  const AuthOptions options = AuthOptions::from_env();
  EXPECT_TRUE(options.enabled);
  EXPECT_EQ(options.admin_password, "s3cr3t-admin-pw");

  ::unsetenv("RAFTKV_ADMIN_PASSWORD");
}

// --- Bootstrap admin ------------------------------------------------------

TEST(AuthEngineTest, AuthenticatesTheBootstrapAdmin) {
  FakeKVStore store;
  const AuthEngine engine(store, enabled_options("admin-password"));

  ASSERT_TRUE(engine.enabled());

  AuthContext context;
  ASSERT_EQ(
      engine.authenticate(basic_header("admin", "admin-password"), context),
      AuthOutcome::kOk);
  EXPECT_EQ(context.name, "admin");
  EXPECT_TRUE(context.has_class(CommandClass::kRead));
  EXPECT_TRUE(context.has_class(CommandClass::kWrite));
  EXPECT_TRUE(context.has_class(CommandClass::kAdmin));
  EXPECT_TRUE(context.key_allowed("literally:anything"));
}

TEST(AuthEngineTest, RejectsTheAdminWithTheWrongPassword) {
  FakeKVStore store;
  const AuthEngine engine(store, enabled_options("admin-password"));

  AuthContext context;
  EXPECT_EQ(
      engine.authenticate(basic_header("admin", "admin-passworD"), context),
      AuthOutcome::kBadCredentials);
  EXPECT_EQ(engine.authenticate(basic_header("admin", ""), context),
            AuthOutcome::kBadCredentials);
  EXPECT_EQ(
      engine.authenticate(basic_header("admin", "admin-password "), context),
      AuthOutcome::kBadCredentials);
}

TEST(AuthEngineTest, AStoredRecordCannotHijackTheAdminName) {
  // The configured admin is checked BEFORE the store, so a record planted at
  // `__sys:user:admin` — by whatever route — is shadowed rather than honoured.
  // Without that ordering, anyone who could write that key would own the
  // cluster's administrator account.
  FakeKVStore store;
  put_user(store, "admin", "attacker-password", {kClassAdmin}, {"*"});
  const AuthEngine engine(store, enabled_options("real-admin-password"));

  AuthContext context;
  EXPECT_EQ(
      engine.authenticate(basic_header("admin", "attacker-password"), context),
      AuthOutcome::kBadCredentials);
  EXPECT_EQ(engine.authenticate(basic_header("admin", "real-admin-password"),
                                context),
            AuthOutcome::kOk);
}

// --- Stored users ---------------------------------------------------------

TEST(AuthEngineTest, AuthenticatesAStoredUserWithItsAcl) {
  FakeKVStore store;
  put_user(store, "alice", "alice-password", {kClassRead, kClassWrite},
           {"app:*"});
  const AuthEngine engine(store, enabled_options("admin-password"));

  AuthContext context;
  ASSERT_EQ(
      engine.authenticate(basic_header("alice", "alice-password"), context),
      AuthOutcome::kOk);
  EXPECT_EQ(context.name, "alice");
  EXPECT_TRUE(context.has_class(CommandClass::kRead));
  EXPECT_TRUE(context.has_class(CommandClass::kWrite));
  EXPECT_FALSE(context.has_class(CommandClass::kAdmin));
  EXPECT_TRUE(context.key_allowed("app:1"));
  EXPECT_FALSE(context.key_allowed("other:1"));
}

TEST(AuthEngineTest, ReadOnlyUserGetsOnlyTheReadClass) {
  FakeKVStore store;
  put_user(store, "reader", "reader-password", {kClassRead}, {"*"});
  const AuthEngine engine(store, enabled_options("admin-password"));

  AuthContext context;
  ASSERT_EQ(
      engine.authenticate(basic_header("reader", "reader-password"), context),
      AuthOutcome::kOk);
  EXPECT_TRUE(context.has_class(CommandClass::kRead));
  EXPECT_FALSE(context.has_class(CommandClass::kWrite));
  EXPECT_FALSE(context.has_class(CommandClass::kAdmin));
}

TEST(AuthEngineTest, UnknownDisabledAndWrongPasswordAreIndistinguishable) {
  // One outcome for all three, on purpose. Reporting "no such user" separately
  // from "wrong password" turns the endpoint into a user-enumeration oracle: an
  // attacker learns which accounts exist without ever authenticating.
  FakeKVStore store;
  put_user(store, "alice", "alice-password", {kClassRead}, {"*"});
  put_user(store, "dormant", "dormant-password", {kClassRead}, {"*"},
           /*enabled=*/false);
  const AuthEngine engine(store, enabled_options("admin-password"));

  AuthContext context;
  EXPECT_EQ(
      engine.authenticate(basic_header("nobody", "any-password"), context),
      AuthOutcome::kBadCredentials);
  EXPECT_EQ(
      engine.authenticate(basic_header("dormant", "dormant-password"), context),
      AuthOutcome::kBadCredentials);
  EXPECT_EQ(
      engine.authenticate(basic_header("alice", "wrong-password"), context),
      AuthOutcome::kBadCredentials);
}

TEST(AuthEngineTest, RefusesARecordThatDoesNotDecodeOrDoesNotValidate) {
  FakeKVStore store;
  const AuthEngine engine(store, enabled_options("admin-password"));
  AuthContext context;

  // Garbage bytes under a user key authenticate nobody. Apply validates before
  // storing, so this should not arise — but "should not arise" is not a reason
  // to treat it as a valid user.
  store.data[user_storage_key("broken")] = "not msgpack";
  EXPECT_EQ(engine.authenticate(basic_header("broken", "x-password"), context),
            AuthOutcome::kBadCredentials);

  // A record whose embedded name disagrees with its key: refuse rather than
  // choose which field is the identity.
  UserUpsertRequest request;
  request.password = "some-password";
  request.classes = {kClassRead};
  request.patterns = {"*"};
  store.data[user_storage_key("mismatch")] =
      request.to_record("other").to_msgpack();
  EXPECT_EQ(
      engine.authenticate(basic_header("mismatch", "some-password"), context),
      AuthOutcome::kBadCredentials);

  // An unsupported version is refused rather than half-honoured.
  UserRecord future = request.to_record("future");
  future.version = 2;
  store.data[user_storage_key("future")] = future.to_msgpack();
  EXPECT_EQ(
      engine.authenticate(basic_header("future", "some-password"), context),
      AuthOutcome::kBadCredentials);
}

// --- Header parsing -------------------------------------------------------

TEST(AuthEngineTest, MissingOrUnusableCredentialIsNoCredentialsNotBad) {
  // kNoCredentials becomes 401 + WWW-Authenticate, which invites a retry;
  // kBadCredentials becomes 403, which does not. A client that sent nothing (or
  // sent something it can fix) belongs in the first bucket.
  FakeKVStore store;
  const AuthEngine engine(store, enabled_options("admin-password"));
  AuthContext context;

  for (const std::string &header :
       {std::string(""), std::string("Basic"), std::string("Basic "),
        std::string("Bearer sometoken"), std::string("Digest x"),
        std::string("Basic !!!not-base64!!!"),
        std::string("Basic ") + base64_encode("no-colon-here")}) {
    EXPECT_EQ(engine.authenticate(header, context), AuthOutcome::kNoCredentials)
        << "header=[" << header << "]";
  }
}

TEST(AuthEngineTest, SchemeMatchingIsCaseInsensitivePerRfc7235) {
  FakeKVStore store;
  const AuthEngine engine(store, enabled_options("admin-password"));
  AuthContext context;

  const std::string credential = base64_encode("admin:admin-password");
  for (const std::string &scheme :
       {std::string("Basic"), std::string("basic"), std::string("BASIC"),
        std::string("bAsIc")}) {
    EXPECT_EQ(engine.authenticate(scheme + " " + credential, context),
              AuthOutcome::kOk)
        << "scheme=" << scheme;
  }
}

TEST(AuthEngineTest, PasswordMayContainColonsButAUserNameMayNot) {
  // RFC 7617 splits at the FIRST colon. That makes a colon legal in a password
  // and impossible in a user name, which is exactly why username_error forbids
  // one — a name with a colon could never be authenticated unambiguously.
  FakeKVStore store;
  put_user(store, "alice", "pass:with:colons", {kClassRead}, {"*"});
  const AuthEngine engine(store, enabled_options("admin-password"));

  AuthContext context;
  EXPECT_EQ(
      engine.authenticate(basic_header("alice", "pass:with:colons"), context),
      AuthOutcome::kOk);

  // "a:b:c" as a credential means user "a", password "b:c" — never user "a:b".
  EXPECT_EQ(engine.authenticate("Basic " + base64_encode("a:b:pw"), context),
            AuthOutcome::kBadCredentials);
}

TEST(AuthEngineTest, PasswordIsBinarySafeAndCaseSensitive) {
  FakeKVStore store;
  const std::string password("pw\xff\x01 space", 11);
  put_user(store, "alice", password, {kClassRead}, {"*"});
  const AuthEngine engine(store, enabled_options("admin-password"));

  AuthContext context;
  EXPECT_EQ(engine.authenticate(basic_header("alice", password), context),
            AuthOutcome::kOk);
  EXPECT_EQ(
      engine.authenticate(basic_header("alice", "PW\xff\x01 space"), context),
      AuthOutcome::kBadCredentials);
}

TEST(AuthEngineTest, ContextIsNotFilledInOnAFailedAuthentication) {
  // A handler that forgot to check the outcome must not find a usable identity
  // sitting in the out parameter.
  FakeKVStore store;
  const AuthEngine engine(store, enabled_options("admin-password"));

  AuthContext context;
  ASSERT_EQ(
      engine.authenticate(basic_header("nobody", "any-password"), context),
      AuthOutcome::kBadCredentials);

  EXPECT_EQ(context.name, "");
  EXPECT_FALSE(context.has_class(CommandClass::kRead));
  EXPECT_FALSE(context.has_class(CommandClass::kWrite));
  EXPECT_FALSE(context.has_class(CommandClass::kAdmin));
  EXPECT_FALSE(context.key_allowed("anything"));
}

TEST(AuthEngineTest, AUserNameThisServerWouldNeverStoreIsJustARejection) {
  FakeKVStore store;
  const AuthEngine engine(store, enabled_options("admin-password"));
  AuthContext context;

  // These cannot exist as user names, and saying so specifically would leak the
  // naming rule to an unauthenticated caller.
  for (const std::string &name :
       {std::string("has space"), std::string("star*"),
        std::string("__sys:user:admin"), std::string(200, 'x')}) {
    EXPECT_EQ(engine.authenticate(basic_header(name, "any-password"), context),
              AuthOutcome::kBadCredentials)
        << "name=[" << name << "]";
  }
}

} // namespace
} // namespace auth
} // namespace kvdb
