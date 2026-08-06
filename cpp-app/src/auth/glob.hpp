#pragma once

#include <cstddef>
#include <string>

/**
 * @file glob.hpp
 * @brief The key-pattern matcher behind a user's allowed key set.
 */

namespace kvdb {
namespace auth {

/**
 * @brief Match @p text against a glob pattern: '*' any run, '?' one byte.
 *
 * Deliberately just those two. Redis' pattern language also has character
 * classes and escapes; a key ACL is a security decision, and every construct
 * added here is another way for an operator to believe a pattern is narrower
 * than it is. Both metacharacters are literal-free: there is no escape, so a
 * pattern cannot match a literal '*' in a key. That is a documented limitation,
 * not an oversight — a key containing '*' is reachable only via a pattern that
 * also matches more.
 *
 * ITERATIVE, NOT RECURSIVE, and that is the security-relevant part. The obvious
 * recursive formulation backtracks exponentially on inputs like "a*a*a*a*b"
 * against a long run of 'a' — a pattern an operator writes and a key an
 * attacker chooses, which makes it a remote CPU exhaustion on every authorized
 * request. The standard two-pointer walk remembers the last '*' and resumes
 * from there, so it is O(len(pattern) x len(text)) worst case with no stack
 * growth.
 *
 * Comparison is byte-exact: keys are binary-transparent in this store, so no
 * case folding and no locale.
 */
[[nodiscard]] inline bool glob_match(const std::string &pattern,
                                     const std::string &text) {
  size_t p = 0;
  size_t t = 0;
  // Where to resume if the current '*' turns out to have consumed too little.
  size_t star = std::string::npos;
  size_t star_text = 0;

  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p;
      star_text = t;
      ++p;
      continue;
    }
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
      continue;
    }
    if (star != std::string::npos) {
      // Give the previous '*' one more byte and retry from just after it.
      p = star + 1;
      t = ++star_text;
      continue;
    }
    return false;
  }

  // Trailing '*'s may match the empty remainder; anything else must not be
  // left.
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

} // namespace auth
} // namespace kvdb
