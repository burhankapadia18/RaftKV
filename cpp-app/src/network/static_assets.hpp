#pragma once

#include <cstddef>
#include <string_view>

namespace kvdb {

/**
 * @brief One asset compiled into the binary.
 *
 * Both a raw and a gzip variant are embedded, and the gzip one may be absent
 * (@c gzip_bytes == nullptr) for assets compression does not help. Embedding
 * both costs roughly 250 KB of .rodata on a multi-megabyte binary and buys two
 * things: no decompressor has to be linked into the engine to serve a client
 * that does not advertise gzip, and `curl .../console/` stays readable.
 *
 * Every member is a pointer or a POD, so a table of these is `constexpr` and
 * lives entirely in .rodata -- nothing is copied, allocated or initialised at
 * start-up. The console costs the database nothing until a browser asks.
 */
struct StaticAsset {
  /** @brief Request path this asset answers, matched exactly. */
  const char *path;
  const char *content_type;

  const unsigned char *bytes;
  std::size_t size;

  /** @brief Pre-compressed variant, or nullptr when none was embedded. */
  const unsigned char *gzip_bytes;
  std::size_t gzip_size;

  /** @brief Strong validator, quoted, derived from the content at build time.
   */
  const char *etag;

  /**
   * @brief True for content-hashed assets, which may be cached forever.
   *
   * index.html is false: its URL never changes, so it must be revalidated (and
   * answers 304 via @c etag). Hashed assets under /console/assets/ are true.
   */
  bool immutable_cache;
};

/** @brief What to send for one asset, given the request's conditional headers.
 */
struct StaticAssetPick {
  bool not_modified = false;
  const unsigned char *bytes = nullptr;
  std::size_t size = 0;
  bool gzipped = false;
};

/**
 * @brief Look up @p path in @p table.
 * @return The asset, or nullptr when there is no exact match.
 *
 * A linear scan, deliberately: the table holds fewer than twenty entries, so a
 * hash map would cost more in code and start-up than it saves in comparisons.
 *
 * There is NO fallback to index.html. The console uses hash routing (#/keys),
 * so every real asset has an exact path and an unknown one is a genuine 404 --
 * answering "here is the app" to every typo makes a 404 unobservable.
 *
 * Safe with @p table == nullptr and @p count == 0, which is the shape the
 * generated table takes when the console was not built in.
 */
[[nodiscard]] inline const StaticAsset *
find_static_asset(const StaticAsset *table, std::size_t count,
                  std::string_view path) {
  for (std::size_t i = 0; i < count; ++i) {
    if (path == table[i].path) {
      return &table[i];
    }
  }
  return nullptr;
}

/**
 * @brief True when @p accept_encoding lists gzip as a whole token.
 *
 * Token-aware rather than a substring search: "notgzipreally" is not an offer
 * of gzip, and serving compressed bytes to a client that cannot decompress
 * them produces garbage rather than an error.
 *
 * Case-insensitive, and q-values are ignored -- a client that writes
 * "gzip;q=0" is vanishingly rare and gets correct-but-compressed output, which
 * is a far smaller problem than parsing quality values by hand here.
 */
[[nodiscard]] inline bool accepts_gzip(std::string_view accept_encoding) {
  static constexpr std::string_view kToken = "gzip";

  std::size_t position = 0;
  while (position < accept_encoding.size()) {
    std::size_t end = accept_encoding.find(',', position);
    if (end == std::string_view::npos) {
      end = accept_encoding.size();
    }
    std::string_view token = accept_encoding.substr(position, end - position);

    // Trim spaces, then drop any ";q=..." parameters.
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
      token.remove_prefix(1);
    }
    const std::size_t semicolon = token.find(';');
    if (semicolon != std::string_view::npos) {
      token = token.substr(0, semicolon);
    }
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
      token.remove_suffix(1);
    }

    if (token.size() == kToken.size()) {
      bool equal = true;
      for (std::size_t i = 0; i < token.size(); ++i) {
        const char lowered = (token[i] >= 'A' && token[i] <= 'Z')
                                 ? static_cast<char>(token[i] - 'A' + 'a')
                                 : token[i];
        if (lowered != kToken[i]) {
          equal = false;
          break;
        }
      }
      if (equal) {
        return true;
      }
    }

    position = end + 1;
  }
  return false;
}

/**
 * @brief True when @p if_none_match satisfies @p etag.
 *
 * RFC 9110 allows a list of validators and the wildcard "*". A substring search
 * is enough for the list case because the etags here are quoted build-time
 * hashes, so one cannot appear inside another by accident.
 */
[[nodiscard]] inline bool etag_matches(std::string_view if_none_match,
                                       std::string_view etag) {
  if (if_none_match.empty()) {
    return false;
  }
  if (if_none_match == "*") {
    return true;
  }
  return if_none_match.find(etag) != std::string_view::npos;
}

/**
 * @brief Decide what to send for @p asset.
 *
 * The conditional check runs FIRST: a 304 needs no body, so there is no point
 * choosing a variant for one.
 */
[[nodiscard]] inline StaticAssetPick
pick_static_asset(const StaticAsset &asset, std::string_view accept_encoding,
                  std::string_view if_none_match) {
  StaticAssetPick pick;

  if (etag_matches(if_none_match, asset.etag)) {
    pick.not_modified = true;
    return pick;
  }

  if (asset.gzip_bytes != nullptr && accepts_gzip(accept_encoding)) {
    pick.bytes = asset.gzip_bytes;
    pick.size = asset.gzip_size;
    pick.gzipped = true;
    return pick;
  }

  pick.bytes = asset.bytes;
  pick.size = asset.size;
  return pick;
}

} // namespace kvdb
