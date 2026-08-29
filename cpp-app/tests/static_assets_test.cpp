/**
 * @file static_assets_test.cpp
 * @brief Tests for embedded static-asset lookup and variant selection.
 *
 * Driven with a HAND-WRITTEN table, so these tests need neither Node nor a
 * generated header. The generated table is exercised end-to-end instead
 * (tests/e2e/test_console.py).
 */

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "network/static_assets.hpp"

namespace kvdb {
namespace {

constexpr unsigned char kIndexRaw[] = {'<', 'h', '1', '>'};
constexpr unsigned char kIndexGzip[] = {0x1f, 0x8b, 0x00, 0x01, 0x02};
constexpr unsigned char kAppRaw[] = {'v', 'a', 'r'};

constexpr StaticAsset kTable[] = {
    {"/console/", "text/html; charset=utf-8", kIndexRaw, sizeof(kIndexRaw),
     kIndexGzip, sizeof(kIndexGzip), "\"idx1\"", false},
    {"/console/assets/app-abc123.js", "text/javascript; charset=utf-8", kAppRaw,
     sizeof(kAppRaw), nullptr, 0, "\"app1\"", true},
};

constexpr std::size_t kTableCount = sizeof(kTable) / sizeof(kTable[0]);

TEST(FindStaticAssetTest, MatchesAnExactPath) {
  const StaticAsset *asset =
      find_static_asset(kTable, kTableCount, "/console/");
  ASSERT_NE(asset, nullptr);
  EXPECT_STREQ(asset->etag, "\"idx1\"");
}

TEST(FindStaticAssetTest, ReturnsNullForAnUnknownPath) {
  EXPECT_EQ(find_static_asset(kTable, kTableCount, "/console/nope"), nullptr);
  // No SPA catch-all: the console uses hash routing, so an unknown path under
  // /console/ is a genuine 404 rather than "here is the app".
  EXPECT_EQ(find_static_asset(kTable, kTableCount, "/console/keys"), nullptr);
}

TEST(FindStaticAssetTest, HandlesAnEmptyTable) {
  // This is the KVDB_CONSOLE=OFF shape: the table exists and is empty, so the
  // handler needs no preprocessor branch.
  EXPECT_EQ(find_static_asset(nullptr, 0, "/console/"), nullptr);
}

TEST(AcceptsGzipTest, DetectsTheTokenAnywhereInTheHeader) {
  EXPECT_TRUE(accepts_gzip("gzip"));
  EXPECT_TRUE(accepts_gzip("gzip, deflate, br"));
  EXPECT_TRUE(accepts_gzip("deflate, gzip"));
  EXPECT_TRUE(accepts_gzip("GZIP"));
  EXPECT_TRUE(accepts_gzip("gzip;q=1.0, identity;q=0.5"));
}

TEST(AcceptsGzipTest, RejectsAnAbsentOrUnrelatedHeader) {
  EXPECT_FALSE(accepts_gzip(""));
  EXPECT_FALSE(accepts_gzip("identity"));
  EXPECT_FALSE(accepts_gzip("deflate, br"));
  // Must not match a longer token that merely contains "gzip".
  EXPECT_FALSE(accepts_gzip("notgzipreally"));
}

TEST(PickStaticAssetTest, PrefersGzipWhenBothExistAndTheClientAcceptsIt) {
  const StaticAssetPick pick = pick_static_asset(kTable[0], "gzip, br", "");
  EXPECT_FALSE(pick.not_modified);
  EXPECT_TRUE(pick.gzipped);
  EXPECT_EQ(pick.bytes, kIndexGzip);
  EXPECT_EQ(pick.size, sizeof(kIndexGzip));
}

TEST(PickStaticAssetTest, FallsBackToRawWhenTheClientDoesNotAcceptGzip) {
  // curl sends no Accept-Encoding, and keeping the raw bytes embedded is what
  // keeps `curl http://localhost:8080/console/` readable for debugging.
  const StaticAssetPick pick = pick_static_asset(kTable[0], "", "");
  EXPECT_FALSE(pick.gzipped);
  EXPECT_EQ(pick.bytes, kIndexRaw);
}

TEST(PickStaticAssetTest, FallsBackToRawWhenNoGzipVariantWasEmbedded) {
  const StaticAssetPick pick = pick_static_asset(kTable[1], "gzip", "");
  EXPECT_FALSE(pick.gzipped);
  EXPECT_EQ(pick.bytes, kAppRaw);
}

TEST(PickStaticAssetTest, ReportsNotModifiedOnAMatchingEtag) {
  const StaticAssetPick pick = pick_static_asset(kTable[0], "gzip", "\"idx1\"");
  EXPECT_TRUE(pick.not_modified);
  EXPECT_EQ(pick.size, 0u);
}

TEST(PickStaticAssetTest, MatchesAnEtagInsideAList) {
  // RFC 9110: If-None-Match may carry several validators.
  const StaticAssetPick pick =
      pick_static_asset(kTable[0], "", "\"other\", \"idx1\"");
  EXPECT_TRUE(pick.not_modified);
}

TEST(PickStaticAssetTest, HonoursTheWildcardValidator) {
  EXPECT_TRUE(pick_static_asset(kTable[0], "", "*").not_modified);
}

TEST(PickStaticAssetTest, ServesTheBodyOnAStaleEtag) {
  const StaticAssetPick pick = pick_static_asset(kTable[0], "", "\"old\"");
  EXPECT_FALSE(pick.not_modified);
  EXPECT_EQ(pick.bytes, kIndexRaw);
}

} // namespace
} // namespace kvdb
