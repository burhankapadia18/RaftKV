/**
 * @file auth_primitives_test.cpp
 * @brief Unit tests for the auth building blocks: SHA-256, constant-time
 *        comparison, strict base64 and the key-pattern glob.
 *
 * These four are small, pure and load-bearing: every one of them sits directly
 * on the authentication decision, so a bug here is an authentication bypass
 * rather than a wrong answer. SHA-256 is checked against the published NIST
 * vectors — a hash function that is subtly wrong still looks like a hash
 * function, and nothing else in the system would notice.
 */

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "auth/base64.hpp"
#include "auth/glob.hpp"
#include "auth/sha256.hpp"

namespace kvdb {
namespace auth {
namespace {

// --- SHA-256 --------------------------------------------------------------

TEST(Sha256Test, MatchesNistVectors) {
  // FIPS 180-4 / NIST CAVP published values.
  EXPECT_EQ(sha256_hex(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(
      sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijk"
                       "lmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstn"
                       "opqrstu"),
            "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256Test, HandlesTheBlockBoundaryLengths) {
  // The padding branch differs depending on how much room is left in the final
  // block, so 55/56/64 bytes exercise all three paths. Values cross-checked
  // against `printf '...' | sha256sum`.
  EXPECT_EQ(sha256_hex(std::string(55, 'a')),
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  EXPECT_EQ(sha256_hex(std::string(56, 'a')),
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  EXPECT_EQ(sha256_hex(std::string(64, 'a')),
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST(Sha256Test, IsBinarySafe) {
  // Keys and values in this store are binary-transparent, and so is a password:
  // a NUL must not truncate the input the way a C string would.
  const std::string with_nul("a\0b", 3);
  ASSERT_EQ(with_nul.size(), 3u);
  EXPECT_NE(sha256_hex(with_nul), sha256_hex("a"));
  EXPECT_NE(sha256_hex(with_nul), sha256_hex("ab"));
  // High bytes are not sign-extended into a different digest.
  EXPECT_EQ(sha256_hex(std::string("\xff", 1)).size(), 64u);
}

TEST(Sha256Test, StreamsInChunksIdenticallyToOneShot) {
  // Chunk boundaries must not change the digest — the whole point of the
  // internal 64-byte buffer. Split sizes are derived from the input rather than
  // written as literals so the test cannot silently hash a truncated message
  // (it did, at first, and "the implementation is broken" was the wrong
  // conclusion).
  const std::string message(
      "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmno"
      "pjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu");
  const std::string expected = sha256_hex(message);

  static constexpr char kHexDigits[] = "0123456789abcdef";
  for (const size_t split : {size_t{0}, size_t{1}, size_t{63}, size_t{64},
                             size_t{65}, message.size()}) {
    ASSERT_LE(split, message.size());
    Sha256 chunked;
    chunked.update(message.substr(0, split));
    chunked.update(message.substr(split));

    std::string hex;
    for (const uint8_t byte : chunked.digest()) {
      hex += kHexDigits[(byte >> 4) & 0x0f];
      hex += kHexDigits[byte & 0x0f];
    }
    EXPECT_EQ(hex, expected) << "split at " << split;
  }
}

TEST(Sha256Test, RefusesReuseAfterDigestInsteadOfCorruptingItself) {
  // REGRESSION GUARD for a latent out-of-bounds write. digest() fills the
  // internal 64-byte block with padding and absorbs it, so it returns with
  // buffer_len_ at a full block; a further update() would write buffer_[64] and
  // clobber the members next to the array.
  //
  // That write is INVISIBLE TO ASAN — it is intra-object, and a std::array
  // member has no redzone inside its enclosing object — which was verified by
  // compiling a reuse-after-digest probe under -fsanitize=address and watching
  // it run to completion silently. So CI would not have caught a future
  // refactor that hashed several candidates through one instance. The contract
  // is enforced rather than documented for exactly that reason.
  Sha256 hasher;
  hasher.update("abc", 3);
  (void)hasher.digest();

  EXPECT_THROW(hasher.update("more", 4), std::logic_error);
  EXPECT_THROW((void)hasher.digest(), std::logic_error);

  // A fresh instance is of course fine — that is what sha256_hex does per call.
  EXPECT_EQ(sha256_hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// --- constant_time_equal --------------------------------------------------

TEST(ConstantTimeEqualTest, AgreesWithOrdinaryEqualityOnResult) {
  // Timing is not observable from a unit test; what IS testable is that the
  // constant-time version never disagrees with `==` about the answer.
  EXPECT_TRUE(constant_time_equal("", ""));
  EXPECT_TRUE(constant_time_equal("abc", "abc"));
  EXPECT_FALSE(constant_time_equal("abc", "abd"));
  // Differing in the first byte and in the last byte both mean "not equal";
  // the point of the loop is that they cost the same.
  EXPECT_FALSE(constant_time_equal("abcdef", "zbcdef"));
  EXPECT_FALSE(constant_time_equal("abcdef", "abcdez"));
  // A prefix is not a match.
  EXPECT_FALSE(constant_time_equal("abc", "ab"));
  EXPECT_FALSE(constant_time_equal("ab", "abc"));
  // Binary safe: the NUL is compared, not treated as a terminator.
  EXPECT_FALSE(
      constant_time_equal(std::string("a\0b", 3), std::string("a\0c", 3)));
  EXPECT_TRUE(
      constant_time_equal(std::string("a\0b", 3), std::string("a\0b", 3)));
}

// --- base64 ---------------------------------------------------------------

TEST(Base64Test, DecodesRfc4648Vectors) {
  EXPECT_EQ(base64_decode("").value_or("<none>"), "");
  EXPECT_EQ(base64_decode("Zg==").value_or("<none>"), "f");
  EXPECT_EQ(base64_decode("Zm8=").value_or("<none>"), "fo");
  EXPECT_EQ(base64_decode("Zm9v").value_or("<none>"), "foo");
  EXPECT_EQ(base64_decode("Zm9vYg==").value_or("<none>"), "foob");
  EXPECT_EQ(base64_decode("Zm9vYmE=").value_or("<none>"), "fooba");
  EXPECT_EQ(base64_decode("Zm9vYmFy").value_or("<none>"), "foobar");
  // The shape this server actually receives: "user:password".
  EXPECT_EQ(base64_decode("YWxpY2U6czNjcjN0").value_or("<none>"),
            "alice:s3cr3t");
}

TEST(Base64Test, RoundTripsThroughTheEncoder) {
  for (const std::string &sample :
       {std::string(""), std::string("f"), std::string("fo"),
        std::string("foo"), std::string("alice:pw"),
        std::string("\xff\xfe\x00\x01", 4)}) {
    const std::optional<std::string> decoded =
        base64_decode(base64_encode(sample));
    ASSERT_TRUE(decoded.has_value()) << "failed for size " << sample.size();
    EXPECT_EQ(*decoded, sample);
  }
}

TEST(Base64Test, RejectsMalformedInputRatherThanSkipping) {
  // A lenient decoder makes several distinct header values decode to the same
  // credential, which is the bug class this refuses to have.
  EXPECT_FALSE(
      base64_decode("Zm9vYg").has_value());        // length not a multiple of 4
  EXPECT_FALSE(base64_decode("Zg=").has_value());  // ditto, truncated padding
  EXPECT_FALSE(base64_decode("Z!9v").has_value()); // outside the alphabet
  EXPECT_FALSE(base64_decode("Zm 9v").has_value());    // interior space
  EXPECT_FALSE(base64_decode("Zm9v\n").has_value());   // trailing newline
  EXPECT_FALSE(base64_decode("Zg==Zg==").has_value()); // padding mid-string
  EXPECT_FALSE(base64_decode("====").has_value());     // padding only
  EXPECT_FALSE(base64_decode("Z===").has_value());     // three pad characters
  // URL-safe alphabet is a different encoding and is not silently accepted.
  EXPECT_FALSE(base64_decode("a-b_").has_value());
}

TEST(Base64Test, DecodesBinaryIncludingNul) {
  const std::optional<std::string> decoded = base64_decode("AAEC");
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->size(), 3u);
  EXPECT_EQ((*decoded)[0], '\0');
  EXPECT_EQ(static_cast<unsigned char>((*decoded)[2]), 0x02u);
}

// --- glob_match -----------------------------------------------------------

TEST(GlobMatchTest, MatchesLiteralPatternsExactly) {
  EXPECT_TRUE(glob_match("app:user", "app:user"));
  EXPECT_FALSE(glob_match("app:user", "app:users"));
  EXPECT_FALSE(glob_match("app:user", "app:use"));
  EXPECT_FALSE(glob_match("app:user", "APP:USER")); // byte-exact, no folding
  EXPECT_TRUE(glob_match("", ""));
  EXPECT_FALSE(glob_match("", "x"));
}

TEST(GlobMatchTest, StarMatchesAnyRunIncludingEmpty) {
  EXPECT_TRUE(glob_match("*", ""));
  EXPECT_TRUE(glob_match("*", "anything at all"));
  EXPECT_TRUE(glob_match("app:*", "app:"));
  EXPECT_TRUE(glob_match("app:*", "app:user:1"));
  EXPECT_FALSE(glob_match("app:*", "other:user"));
  EXPECT_FALSE(glob_match("app:*", "app")); // the colon is required
  EXPECT_TRUE(glob_match("*:tail", "a:b:tail"));
  EXPECT_TRUE(glob_match("a*b*c", "abc"));
  EXPECT_TRUE(glob_match("a*b*c", "axxbyyc"));
  EXPECT_FALSE(glob_match("a*b*c", "axxbyy"));
  // Consecutive stars collapse; trailing stars may consume nothing.
  EXPECT_TRUE(glob_match("a**", "a"));
  EXPECT_TRUE(glob_match("**a**", "a"));
}

TEST(GlobMatchTest, QuestionMarkMatchesExactlyOneByte) {
  EXPECT_TRUE(glob_match("a?c", "abc"));
  EXPECT_FALSE(glob_match("a?c", "ac"));   // must consume one
  EXPECT_FALSE(glob_match("a?c", "abbc")); // and only one
  EXPECT_TRUE(glob_match("???", "xyz"));
  EXPECT_FALSE(glob_match("???", "xy"));
  // '?' matches a byte, and a NUL is a byte.
  EXPECT_TRUE(glob_match("a?b", std::string("a\0b", 3)));
}

TEST(GlobMatchTest, BacktracksWithoutBlowingUp) {
  // The recursive formulation is exponential on exactly this shape, and the
  // pattern comes from an operator while the key comes from a client — so it
  // would be a remote CPU exhaustion on every authorized request. This must
  // return promptly; if it ever hangs, the iterative walk has been
  // "simplified".
  EXPECT_FALSE(glob_match("a*a*a*a*a*a*a*b", std::string(64, 'a')));
  EXPECT_TRUE(glob_match("a*a*a*a*a*a*a*b", std::string(64, 'a') + "b"));
}

TEST(GlobMatchTest, HasNoEscapeSoAMetacharacterIsNeverLiteral) {
  // DOCUMENTED LIMITATION, pinned: there is no escape syntax, so a key
  // containing '*' cannot be named exactly — a pattern that matches it also
  // matches more. Adding escapes later means flipping this assertion.
  EXPECT_TRUE(glob_match("a*c", "a*c"));  // matches, but as a wildcard
  EXPECT_TRUE(glob_match("a*c", "abbc")); // ...which is why this also matches
}

} // namespace
} // namespace auth
} // namespace kvdb
