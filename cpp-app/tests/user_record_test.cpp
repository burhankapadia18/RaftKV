/**
 * @file user_record_test.cpp
 * @brief Unit tests for auth::UserRecord, UserUpsertRequest and the reserved
 *        key space helpers.
 *
 * These bytes arrive over Raft and off disk and are decoded on every replica
 * for an already-committed entry, so the decode limits and the version rule get
 * the same treatment as KVCommand's: refusing bad input is the feature.
 */

#include <gtest/gtest.h>

#include <map>
#include <msgpack.hpp>
#include <set>
#include <string>

#include "auth/user_record.hpp"

namespace kvdb {
namespace auth {
namespace {

/** @brief A complete, valid record to mutate in individual tests. */
UserRecord valid_record(const std::string &name = "alice") {
  UserRecord record;
  record.version = UserRecord::kCurrentVersion;
  record.name = name;
  record.salt_hex = std::string(32, 'a');
  record.pw_sha256_hex = hash_password(record.salt_hex, "s3cr3t-password");
  record.enabled = true;
  record.classes = {kClassRead, kClassWrite};
  record.patterns = {"app:*"};
  return record;
}

UserRecord decode(const std::string &bytes) {
  return UserRecord::from_msgpack(bytes.data(), bytes.size());
}

// --- Reserved key space ---------------------------------------------------

TEST(ReservedKeyTest, RecognisesTheSystemPrefixOnlyAsAPrefix) {
  EXPECT_TRUE(is_reserved_key("__sys:"));
  EXPECT_TRUE(is_reserved_key("__sys:user:alice"));
  EXPECT_TRUE(is_reserved_key("__sys:anything"));
  // A prefix, not a substring: a key that merely CONTAINS the marker is an
  // ordinary key, and refusing it would take a name a client may already use.
  EXPECT_FALSE(is_reserved_key("app:__sys:x"));
  EXPECT_FALSE(is_reserved_key("_sys:x"));
  EXPECT_FALSE(is_reserved_key("__sys"));
  EXPECT_FALSE(is_reserved_key(""));
}

TEST(ReservedKeyTest, UserStorageKeyIsTheNameUnderTheUserPrefix) {
  EXPECT_EQ(user_storage_key("alice"), "__sys:user:alice");
  EXPECT_TRUE(is_reserved_key(user_storage_key("alice")));
}

// --- User names -----------------------------------------------------------

TEST(UsernameTest, AcceptsTheDocumentedCharacterSet) {
  for (const std::string &name :
       {std::string("a"), std::string("alice"), std::string("Alice"),
        std::string("user_1"), std::string("user.name"),
        std::string("user-name"), std::string("A1._-"),
        std::string(128, 'x')}) {
    EXPECT_TRUE(valid_username(name)) << "rejected: " << name;
  }
}

TEST(UsernameTest, RejectsEmptyOverlongAndAnythingOutsideTheSet) {
  EXPECT_FALSE(valid_username(""));
  EXPECT_FALSE(valid_username(std::string(129, 'x')));
  // A colon is the one that MUST be rejected: RFC 7617 splits a Basic
  // credential at the first colon, so "a:b" as a user name is
  // unauthenticatable.
  EXPECT_FALSE(valid_username("a:b"));
  for (const std::string &name :
       {std::string("has space"), std::string("slash/es"), std::string("star*"),
        std::string("pct%"), std::string("at@sign"), std::string("brace{}"),
        std::string("new\nline"), std::string("nul\0byte", 8),
        std::string("__sys:user:x"), std::string("üñî")}) {
    EXPECT_FALSE(valid_username(name)) << "accepted: " << name;
  }
  // The message names the rule rather than just saying "invalid".
  ASSERT_TRUE(username_error("a:b").has_value());
  EXPECT_NE(username_error("a:b")->find("may only contain"), std::string::npos);
}

// --- Password hashing -----------------------------------------------------

TEST(HashPasswordTest, IsSaltedSoIdenticalPasswordsHashDifferently) {
  const std::string a = hash_password(std::string(32, 'a'), "same-password");
  const std::string b = hash_password(std::string(32, 'b'), "same-password");

  EXPECT_EQ(a.size(), 64u);
  EXPECT_NE(a, b) << "the salt is not reaching the digest";
  // Deterministic for the same inputs, or nobody could ever log in twice.
  EXPECT_EQ(a, hash_password(std::string(32, 'a'), "same-password"));
  // And sensitive to the password.
  EXPECT_NE(a, hash_password(std::string(32, 'a'), "same-passworD"));
}

TEST(HashPasswordTest, SaltAndPasswordCannotBeConfusedForEachOther) {
  // The ':' join is what makes this hold: without a separator, ("ab","c") and
  // ("a","bc") would hash identically and two different credentials would
  // authenticate the same user.
  EXPECT_NE(hash_password("ab", "c"), hash_password("a", "bc"));
}

TEST(GenerateSaltTest, ReturnsFreshLowercaseHex) {
  std::set<std::string> seen;
  for (int i = 0; i < 16; ++i) {
    const std::string salt = generate_salt_hex();
    ASSERT_EQ(salt.size(), 32u);
    for (const char c : salt) {
      EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
          << "not lowercase hex: " << salt;
    }
    seen.insert(salt);
  }
  // A predictable salt is no salt. 16 draws of 128 bits colliding is not a
  // flake, it is a broken source.
  EXPECT_EQ(seen.size(), 16u);
}

// --- Encode / decode ------------------------------------------------------

TEST(UserRecordTest, RoundTripsThroughMsgpack) {
  const UserRecord original = valid_record();
  const UserRecord decoded = decode(original.to_msgpack());

  EXPECT_EQ(decoded.version, original.version);
  EXPECT_EQ(decoded.name, original.name);
  EXPECT_EQ(decoded.salt_hex, original.salt_hex);
  EXPECT_EQ(decoded.pw_sha256_hex, original.pw_sha256_hex);
  EXPECT_EQ(decoded.enabled, original.enabled);
  EXPECT_EQ(decoded.classes, original.classes);
  EXPECT_EQ(decoded.patterns, original.patterns);
  EXPECT_FALSE(decoded.validation_error().has_value());
}

TEST(UserRecordTest, RoundTripsADisabledUserWithNoAccess) {
  UserRecord record = valid_record();
  record.enabled = false;
  record.classes.clear();
  record.patterns.clear();

  const UserRecord decoded = decode(record.to_msgpack());

  EXPECT_FALSE(decoded.enabled);
  EXPECT_TRUE(decoded.classes.empty());
  EXPECT_TRUE(decoded.patterns.empty());
  // Holding nothing is a legitimate record, not a malformed one.
  EXPECT_FALSE(decoded.validation_error().has_value());
}

TEST(UserRecordTest, RejectsGarbageAndDecodeBombs) {
  EXPECT_THROW(decode(""), std::exception);
  EXPECT_THROW(decode("not msgpack at all"), std::exception);

  // The same map32 chain that KVCommand's limits exist to refuse. This decode
  // also runs at apply time on a committed entry, on every replica.
  const unsigned char bomb[] = {0xdf, 0xdf, 0xdf, 0xdf, 0xdf, 0xdf,
                                0xdf, 0xdf, 0xdf, 0xdf, 0xdf, 0xdf,
                                0xdf, 0xdf, 0xdf, 0x83, 0x83};
  EXPECT_THROW(UserRecord::from_msgpack(reinterpret_cast<const char *>(bomb),
                                        sizeof(bomb)),
               std::exception);
}

TEST(UserRecordTest, MissingVersionDecodesAsVersionOne) {
  // msgpack-c leaves absent members at their default, and the default is 1 —
  // which is correct, because version 1 is what a record without the field is.
  msgpack::sbuffer buffer;
  const std::map<std::string, std::string> partial = {
      {"name", "alice"},
      {"salt_hex", std::string(32, 'a')},
      {"pw_sha256_hex", std::string(64, 'b')}};
  msgpack::pack(buffer, partial);

  const UserRecord decoded =
      UserRecord::from_msgpack(buffer.data(), buffer.size());

  EXPECT_EQ(decoded.version, 1);
  EXPECT_EQ(decoded.name, "alice");
  // enabled defaults to true, patterns to empty — which denies every key.
  EXPECT_TRUE(decoded.enabled);
  EXPECT_TRUE(decoded.patterns.empty());
}

// --- Validation -----------------------------------------------------------

TEST(UserRecordTest, RejectsAnUnsupportedVersion) {
  // A future record must be REFUSED, not half-understood: enforcing an ACL that
  // is not the one the operator wrote is worse than refusing to enforce one.
  for (const std::int64_t version : {std::int64_t{0}, std::int64_t{2},
                                     std::int64_t{-1}, std::int64_t{9999}}) {
    UserRecord record = valid_record();
    record.version = version;
    ASSERT_TRUE(record.validation_error().has_value()) << "version " << version;
    EXPECT_NE(record.validation_error()->find("version"), std::string::npos);
  }
}

TEST(UserRecordTest, RejectsMalformedSaltsHashesAndClasses) {
  {
    UserRecord record = valid_record();
    record.salt_hex = std::string(31, 'a');
    EXPECT_TRUE(record.validation_error().has_value());
  }
  {
    UserRecord record = valid_record();
    record.pw_sha256_hex = std::string(63, 'a');
    EXPECT_TRUE(record.validation_error().has_value());
  }
  {
    // Uppercase hex would compare unequal to a freshly computed lowercase
    // digest, so a record carrying it could never authenticate — refuse it at
    // the boundary instead of storing an unusable user.
    UserRecord record = valid_record();
    record.pw_sha256_hex = std::string(64, 'A');
    ASSERT_TRUE(record.validation_error().has_value());
    EXPECT_NE(record.validation_error()->find("lowercase hex"),
              std::string::npos);
  }
  {
    UserRecord record = valid_record();
    record.classes = {kClassRead, "superuser"};
    ASSERT_TRUE(record.validation_error().has_value());
    EXPECT_NE(record.validation_error()->find("superuser"), std::string::npos);
  }
  {
    UserRecord record = valid_record("bad name");
    EXPECT_TRUE(record.validation_error().has_value());
  }
}

TEST(UserRecordTest, HasClassIsExactMembership) {
  const UserRecord record = valid_record();

  EXPECT_TRUE(record.has_class(kClassRead));
  EXPECT_TRUE(record.has_class(kClassWrite));
  // admin is NOT implied by holding read and write — the classes are
  // independent, so a data user cannot manage users.
  EXPECT_FALSE(record.has_class(kClassAdmin));
  EXPECT_FALSE(record.has_class("READ"));
  EXPECT_FALSE(record.has_class(""));
}

// --- UserUpsertRequest ----------------------------------------------------

TEST(UserUpsertRequestTest, BuildsARecordWithAFreshSaltAndMatchingHash) {
  UserUpsertRequest request;
  request.password = "correct-horse";
  request.classes = {kClassRead};
  request.patterns = {"app:*"};

  const UserRecord first = request.to_record("alice");
  const UserRecord second = request.to_record("alice");

  EXPECT_FALSE(first.validation_error().has_value());
  EXPECT_EQ(first.name, "alice");
  EXPECT_EQ(first.pw_sha256_hex,
            hash_password(first.salt_hex, "correct-horse"));
  // Two upserts of the same password must not produce the same stored bytes,
  // or the hash itself becomes a password-equality oracle across users.
  EXPECT_NE(first.salt_hex, second.salt_hex);
  EXPECT_NE(first.pw_sha256_hex, second.pw_sha256_hex);
}

TEST(UserUpsertRequestTest, RejectsShortPasswordsAndUnknownClasses) {
  {
    UserUpsertRequest request;
    request.password = "short";
    ASSERT_TRUE(request.validation_error().has_value());
    EXPECT_NE(request.validation_error()->find("8 bytes"), std::string::npos);
  }
  {
    UserUpsertRequest request;
    request.password = "";
    EXPECT_TRUE(request.validation_error().has_value());
  }
  {
    UserUpsertRequest request;
    request.password = "long-enough-password";
    request.classes = {"root"};
    EXPECT_TRUE(request.validation_error().has_value());
  }
  {
    UserUpsertRequest request;
    request.password = "long-enough-password";
    request.classes = {kClassAdmin};
    EXPECT_FALSE(request.validation_error().has_value());
  }
}

TEST(UserUpsertRequestTest, DecodesFromMsgpackAndRefusesBombs) {
  UserUpsertRequest original;
  original.password = "long-enough-password";
  original.enabled = false;
  original.classes = {kClassRead, kClassWrite};
  original.patterns = {"a:*", "b:*"};

  msgpack::sbuffer buffer;
  msgpack::pack(buffer, original);
  const UserUpsertRequest decoded =
      UserUpsertRequest::from_msgpack(buffer.data(), buffer.size());

  EXPECT_EQ(decoded.password, original.password);
  EXPECT_FALSE(decoded.enabled);
  EXPECT_EQ(decoded.classes, original.classes);
  EXPECT_EQ(decoded.patterns, original.patterns);

  EXPECT_THROW(UserUpsertRequest::from_msgpack("", 0), std::exception);
  const unsigned char bomb[] = {0xdf, 0xdf, 0xdf, 0xdf, 0xdf, 0xdf,
                                0xdf, 0xdf, 0xdf, 0xdf, 0xdf, 0xdf,
                                0xdf, 0xdf, 0xdf, 0x83, 0x83};
  EXPECT_THROW(UserUpsertRequest::from_msgpack(
                   reinterpret_cast<const char *>(bomb), sizeof(bomb)),
               std::exception);
}

TEST(UserUpsertRequestTest, OmittedEnabledDefaultsToTrue) {
  // A client that sends only a password gets an enabled user with no classes
  // and no patterns — which can authenticate and do nothing. That is the safe
  // direction; the reverse (all access by default) is not.
  msgpack::sbuffer buffer;
  const std::map<std::string, std::string> partial = {
      {"password", "long-enough-password"}};
  msgpack::pack(buffer, partial);

  const UserUpsertRequest decoded =
      UserUpsertRequest::from_msgpack(buffer.data(), buffer.size());

  EXPECT_TRUE(decoded.enabled);
  EXPECT_TRUE(decoded.classes.empty());
  EXPECT_TRUE(decoded.patterns.empty());
  EXPECT_FALSE(decoded.validation_error().has_value());
}

} // namespace
} // namespace auth
} // namespace kvdb
