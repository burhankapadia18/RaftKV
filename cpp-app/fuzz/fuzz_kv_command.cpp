/**
 * @file fuzz_kv_command.cpp
 * @brief libFuzzer target for KVCommand::from_msgpack (spec R6.7).
 *
 * This is the trust boundary for every byte a client sends. The HTTP layer
 * forwards the request body to Raft without parsing it, so from_msgpack is the
 * FIRST code to interpret attacker-controlled bytes — and it also runs against
 * bytes replayed out of the raft log and the WAL, where a crash would take the
 * node down on every restart rather than once.
 *
 * The contract being fuzzed is narrow and total: for ANY input, from_msgpack
 * either returns a KVCommand or throws. It must never read out of bounds, and
 * it must never abort. Combined with -fsanitize=address,undefined that is
 * exactly what libFuzzer checks here.
 */

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include "commands/kv_command.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  try {
    const kvdb::KVCommand cmd = kvdb::KVCommand::from_msgpack(
        reinterpret_cast<const char *>(data), size);

    // Touch the decoded fields so the optimizer cannot discard the parse, and
    // so the validation path is fuzzed too — validation_error() reads op and
    // key, which is where a decoded-but-hostile value would land.
    const auto reason = cmd.validation_error();
    if (reason.has_value() && reason->empty()) {
      // A validation failure with an empty reason would be a contract
      // violation: the whole point is that a rejection says why.
      __builtin_trap();
    }
    if (cmd.is_valid() == reason.has_value()) {
      // is_valid() must be exactly the negation of validation_error(). They are
      // two views of one rule and drifting apart is a real bug.
      __builtin_trap();
    }
  } catch (const std::exception &) {
    // Expected for the overwhelming majority of inputs. A THROW is a pass; only
    // a crash, a sanitizer report or a trap above is a finding.
  }
  return 0;
}
