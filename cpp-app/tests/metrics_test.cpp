/**
 * @file metrics_test.cpp
 * @brief Unit tests for the hand-rolled Prometheus registry (spec R5.4).
 *
 * The registry has no external dependency, so nothing else validates that what
 * it emits is actually scrapeable. These tests pin the exposition format itself
 * — a malformed line makes a whole scrape fail, and the failure shows up in
 * Prometheus rather than here.
 */

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "common/metrics.hpp"

namespace kvdb {
namespace metrics {
namespace {

/** @brief True when haystack contains needle. */
bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

TEST(CounterTest, StartsAtZeroAndAccumulates) {
  Counter counter;
  EXPECT_EQ(counter.value(), 0u);
  counter.inc();
  counter.inc(4);
  EXPECT_EQ(counter.value(), 5u);
}

TEST(HistogramTest, BucketsAreCumulative) {
  // Prometheus histograms are "le" buckets: an observation counts in its own
  // bucket AND every wider one. Getting this wrong produces quantiles that are
  // silently nonsense rather than an error.
  Histogram histogram({0.01, 0.1, 1.0});
  histogram.observe(0.005); // <= all three
  histogram.observe(0.05);  // <= 0.1 and 1.0
  histogram.observe(2.0);   // <= none

  EXPECT_EQ(histogram.bucket(0), 1u);
  EXPECT_EQ(histogram.bucket(1), 2u);
  EXPECT_EQ(histogram.bucket(2), 2u);
  EXPECT_EQ(histogram.count(), 3u);
  EXPECT_NEAR(histogram.sum_seconds(), 2.055, 0.001);
}

TEST(HistogramTest, ObservationOnABoundIsInclusive) {
  Histogram histogram({0.01});
  histogram.observe(0.01);
  EXPECT_EQ(histogram.bucket(0), 1u) << "an observation exactly on the bound "
                                        "belongs in it (le, not lt)";
}

TEST(RegistryTest, CounterExpositionHasHelpTypeAndValue) {
  Registry registry;
  registry.counter("thing_total", "A thing.").inc(3);

  const std::string out = registry.render();
  EXPECT_TRUE(contains(out, "# HELP thing_total A thing.")) << out;
  EXPECT_TRUE(contains(out, "# TYPE thing_total counter")) << out;
  EXPECT_TRUE(contains(out, "thing_total 3")) << out;
}

TEST(RegistryTest, TheSameNameAndLabelsReturnTheSameSeries) {
  // The hot path calls counter() on every request, so a second call must return
  // the SAME counter rather than resetting it to zero.
  Registry registry;
  registry.counter("hits_total", "Hits.", {{"route", "/kv"}}).inc();
  registry.counter("hits_total", "Hits.", {{"route", "/kv"}}).inc();

  EXPECT_TRUE(contains(registry.render(), "hits_total{route=\"/kv\"} 2"))
      << registry.render();
}

TEST(RegistryTest, HelpAndTypeAppearOncePerNameNotPerSeries) {
  // Repeating HELP/TYPE for a second label set is a scrape ERROR, not a
  // cosmetic problem, so this is the format rule most worth pinning.
  Registry registry;
  registry.counter("hits_total", "Hits.", {{"route", "a"}}).inc();
  registry.counter("hits_total", "Hits.", {{"route", "b"}}).inc();

  const std::string out = registry.render();
  size_t help_count = 0;
  for (size_t i = out.find("# HELP hits_total"); i != std::string::npos;
       i = out.find("# HELP hits_total", i + 1)) {
    ++help_count;
  }
  EXPECT_EQ(help_count, 1u) << "HELP emitted " << help_count << " times:\n"
                            << out;
}

TEST(RegistryTest, HistogramExpositionHasBucketsSumAndCount) {
  Registry registry;
  registry.histogram("dur_seconds", "Duration.", {}, {0.01, 0.1})
      .observe(0.005);

  const std::string out = registry.render();
  EXPECT_TRUE(contains(out, "# TYPE dur_seconds histogram")) << out;
  EXPECT_TRUE(contains(out, "dur_seconds_bucket{le=\"0.01\"} 1")) << out;
  EXPECT_TRUE(contains(out, "dur_seconds_bucket{le=\"0.1\"} 1")) << out;
  // The +Inf bucket is mandatory and must equal the total count.
  EXPECT_TRUE(contains(out, "dur_seconds_bucket{le=\"+Inf\"} 1")) << out;
  EXPECT_TRUE(contains(out, "dur_seconds_count 1")) << out;
  EXPECT_TRUE(contains(out, "dur_seconds_sum ")) << out;
}

TEST(RegistryTest, HistogramLabelsAppearOnBucketsToo) {
  Registry registry;
  registry.histogram("dur_seconds", "Duration.", {{"route", "/kv"}}, {0.01})
      .observe(0.005);

  const std::string out = registry.render();
  // Both the route label and the le label must be present on the bucket line,
  // or the series cannot be aggregated by route.
  EXPECT_TRUE(contains(out, "route=\"/kv\"")) << out;
  EXPECT_TRUE(contains(out, "le=\"0.01\"")) << out;
}

TEST(RegistryTest, GaugeIsEvaluatedAtRenderTime) {
  // The whole reason gauges are callbacks: a value captured at registration
  // would report whatever was true at startup forever.
  Registry registry;
  size_t backing = 1;
  registry.gauge_fn("keys", "Keys.",
                    [&backing]() { return static_cast<double>(backing); });

  EXPECT_TRUE(contains(registry.render(), "keys 1"));
  backing = 42;
  EXPECT_TRUE(contains(registry.render(), "keys 42"))
      << "the gauge was snapshotted at registration instead of read at scrape";
}

TEST(EscapeLabelTest, EscapesWhatWouldBreakAScrape) {
  // Label values include a route derived from a client-supplied path, and the
  // exposition format is line-oriented: a raw newline or quote makes the whole
  // scrape unparseable.
  EXPECT_EQ(escape_label("plain"), "plain");
  EXPECT_EQ(escape_label("a\"b"), "a\\\"b");
  EXPECT_EQ(escape_label("a\\b"), "a\\\\b");
  EXPECT_EQ(escape_label("a\nb"), "a\\nb");
}

TEST(RegistryTest, ConcurrentUseIsSafe) {
  // Counters are incremented from HTTP worker threads, and a route seen for the
  // first time registers a series while others are already observing. Both
  // paths run here; TSan in CI is what turns a missing lock into a failure.
  Registry registry;
  constexpr int kThreads = 8;
  constexpr int kPer = 500;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&registry, t]() {
      for (int i = 0; i < kPer; ++i) {
        registry.counter("shared_total", "Shared.").inc();
        // A per-thread series name, so registration races registration.
        registry
            .counter("per_thread_total", "Per thread.",
                     {{"t", std::to_string(t)}})
            .inc();
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  EXPECT_TRUE(contains(registry.render(),
                       "shared_total " + std::to_string(kThreads * kPer)))
      << registry.render();
}

} // namespace
} // namespace metrics
} // namespace kvdb
