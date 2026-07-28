/**
 * @file fuzz_http_parser.cpp
 * @brief libFuzzer target for HttpRequestParser::parse (spec R6.7).
 *
 * The other trust boundary: this is hand-rolled HTTP parsing of bytes straight
 * off a socket, from an unauthenticated peer. Phase 1 found a remote DoS here
 * (std::stoi throwing out of parse() killed the process) and Phase 4's review
 * found a second one (a negative Content-Length wedged the accept loop), both
 * by reasoning rather than by fuzzing — which is the argument for fuzzing it.
 *
 * The contract: parse() is TOTAL. For any input it returns a request or
 * nullopt, and never throws. That is asserted directly below, because an
 * exception escaping parse() is precisely the failure mode that took a node
 * down before.
 */

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>

#include "network/http_request.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const std::string raw(reinterpret_cast<const char *>(data), size);

  try {
    const std::optional<kvdb::HttpRequest> parsed =
        kvdb::HttpRequestParser::parse(raw);
    if (!parsed.has_value()) {
      return 0;
    }

    // content_length must never come back negative: the server casts it to
    // size_t for the body top-up loop, and a negative value there is the wedge
    // Phase 4's review found. Guarding it here means the fuzzer, not a user,
    // finds it if the guard is ever removed.
    if (parsed->content_length < 0) {
      __builtin_trap();
    }

    // query_params() does its own index arithmetic over client-controlled
    // bytes, so it is part of the boundary and gets fuzzed with it.
    const auto params = parsed->query_params();
    for (const auto &param : params) {
      // url_decode is applied to a path/query component in the handler; feed it
      // the fuzzed values too. It must never throw.
      const auto decoded = kvdb::url_decode(param.second, true);
      (void)decoded;
    }
    const auto decoded_path = kvdb::url_decode(parsed->path, false);
    (void)decoded_path;
  } catch (const std::exception &) {
    // parse() is documented as total (R1.8). An exception escaping it is the
    // remote kill switch that already cost this project one outage, so it is a
    // finding, not an expected outcome.
    __builtin_trap();
  }
  return 0;
}
