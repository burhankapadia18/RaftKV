#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace kvdb {
namespace metrics {

/**
 * @brief A monotonically increasing counter (R5.4).
 *
 * Updated from HTTP worker threads, so the increment is atomic and relaxed:
 * counters have no ordering relationship with anything else, and a scrape that
 * reads a value one increment stale is indistinguishable from scraping a moment
 * earlier.
 */
class Counter {
public:
  void inc(uint64_t amount = 1) {
    value_.fetch_add(amount, std::memory_order_relaxed);
  }

  [[nodiscard]] uint64_t value() const {
    return value_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<uint64_t> value_{0};
};

/**
 * @brief A fixed-bucket latency histogram.
 *
 * Bucket bounds are supplied at construction and never change, which is what
 * lets the bucket counters be plain atomics with no locking on the hot path.
 *
 * The sum is accumulated in MICROSECONDS as an integer rather than as a double.
 * `std::atomic<double>::fetch_add` only exists in C++20 and this is a C++17
 * codebase; carrying an integer avoids both a lock and a compare-exchange loop,
 * and microsecond resolution is far finer than anything measurable here. It is
 * divided back to seconds at render time, because Prometheus convention is base
 * units.
 */
class Histogram {
public:
  explicit Histogram(std::vector<double> upper_bounds)
      : bounds_(std::move(upper_bounds)), buckets_(bounds_.size()) {}

  /** @brief Record one observation, in seconds. */
  void observe(double seconds) {
    // Cumulative ("le") buckets: an observation counts in its own bucket and
    // every wider one, which is the format Prometheus expects.
    for (size_t i = 0; i < bounds_.size(); ++i) {
      if (seconds <= bounds_[i]) {
        buckets_[i].fetch_add(1, std::memory_order_relaxed);
      }
    }
    count_.fetch_add(1, std::memory_order_relaxed);
    const auto micros = static_cast<uint64_t>(seconds * 1e6);
    sum_micros_.fetch_add(micros, std::memory_order_relaxed);
  }

  [[nodiscard]] const std::vector<double> &bounds() const { return bounds_; }

  [[nodiscard]] uint64_t bucket(size_t index) const {
    return buckets_[index].load(std::memory_order_relaxed);
  }

  [[nodiscard]] uint64_t count() const {
    return count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] double sum_seconds() const {
    return static_cast<double>(sum_micros_.load(std::memory_order_relaxed)) /
           1e6;
  }

private:
  std::vector<double> bounds_;
  // deque-like growth is never needed (fixed at construction), but the elements
  // are atomics and therefore neither copyable nor movable, so the vector is
  // sized once and never resized.
  std::vector<std::atomic<uint64_t>> buckets_;
  std::atomic<uint64_t> count_{0};
  std::atomic<uint64_t> sum_micros_{0};
};

/** @brief Default latency buckets, in seconds. */
inline std::vector<double> default_latency_buckets() {
  return {0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025,
          0.05,   0.1,   0.25,   0.5,   1.0,  5.0};
}

/**
 * @brief Escape a label value for the Prometheus text format.
 *
 * The exposition format is line-oriented, so a raw newline or quote in a label
 * would produce a scrape that fails to parse — and label values here include a
 * route name, which is derived from a client-supplied path.
 */
[[nodiscard]] inline std::string escape_label(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

/**
 * @brief A tiny Prometheus registry with no external dependency (R5.4).
 *
 * Registration takes a mutex; observation does not. That split is deliberate:
 * series are created a handful of times at startup (or on first use of a
 * route), while observations happen on every request from every worker thread.
 * Handing out stable references to heap-allocated metrics is what makes the
 * fast path lock-free — a map that rehashes could otherwise move a Counter
 * under a thread holding a reference to it.
 */
class Registry {
public:
  /** @brief Get or create a counter. Safe to call concurrently. */
  Counter &counter(const std::string &name, const std::string &help,
                   const std::map<std::string, std::string> &labels = {}) {
    const std::string key = series_key(name, labels);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = counters_.find(key);
    if (it != counters_.end()) {
      return *it->second.metric;
    }
    CounterSeries series{name, help, labels, std::make_unique<Counter>()};
    Counter &ref = *series.metric;
    counters_.emplace(key, std::move(series));
    return ref;
  }

  /** @brief Get or create a histogram. Safe to call concurrently. */
  Histogram &histogram(const std::string &name, const std::string &help,
                       const std::map<std::string, std::string> &labels = {},
                       std::vector<double> bounds = default_latency_buckets()) {
    const std::string key = series_key(name, labels);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = histograms_.find(key);
    if (it != histograms_.end()) {
      return *it->second.metric;
    }
    HistogramSeries series{name, help, labels,
                           std::make_unique<Histogram>(std::move(bounds))};
    Histogram &ref = *series.metric;
    histograms_.emplace(key, std::move(series));
    return ref;
  }

  /**
   * @brief Register a gauge evaluated at SCRAPE time.
   *
   * A callback rather than a stored value, for the same reason the Go side uses
   * a custom collector: a gauge refreshed on a timer reports whatever the last
   * tick saw, which is how a dashboard shows a stale store size during the
   * exact minute someone is looking at it. It also keeps observability out of
   * the store itself — nothing had to be added to IKVStore to export a key
   * count.
   */
  void gauge_fn(const std::string &name, const std::string &help,
                std::function<double()> read) {
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_.push_back(GaugeSeries{name, help, std::move(read)});
  }

  /**
   * @brief Render everything in the Prometheus text exposition format.
   *
   * HELP/TYPE are emitted once per metric NAME even when several label sets
   * share it; repeating them per series is a scrape error.
   */
  [[nodiscard]] std::string render() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;

    std::vector<std::string> emitted;
    const auto needs_header = [&emitted](const std::string &name) {
      for (const std::string &seen : emitted) {
        if (seen == name) {
          return false;
        }
      }
      return true;
    };

    for (const auto &entry : counters_) {
      const CounterSeries &series = entry.second;
      if (needs_header(series.name)) {
        out << "# HELP " << series.name << " " << series.help << "\n";
        out << "# TYPE " << series.name << " counter\n";
        emitted.push_back(series.name);
      }
      out << series.name << format_labels(series.labels) << " "
          << series.metric->value() << "\n";
    }

    for (const auto &entry : histograms_) {
      const HistogramSeries &series = entry.second;
      if (needs_header(series.name)) {
        out << "# HELP " << series.name << " " << series.help << "\n";
        out << "# TYPE " << series.name << " histogram\n";
        emitted.push_back(series.name);
      }
      const Histogram &histogram = *series.metric;
      for (size_t i = 0; i < histogram.bounds().size(); ++i) {
        std::map<std::string, std::string> bucket_labels = series.labels;
        bucket_labels["le"] = format_double(histogram.bounds()[i]);
        out << series.name << "_bucket" << format_labels(bucket_labels) << " "
            << histogram.bucket(i) << "\n";
      }
      std::map<std::string, std::string> inf_labels = series.labels;
      inf_labels["le"] = "+Inf";
      out << series.name << "_bucket" << format_labels(inf_labels) << " "
          << histogram.count() << "\n";
      out << series.name << "_sum" << format_labels(series.labels) << " "
          << format_double(histogram.sum_seconds()) << "\n";
      out << series.name << "_count" << format_labels(series.labels) << " "
          << histogram.count() << "\n";
    }

    for (const GaugeSeries &series : gauges_) {
      out << "# HELP " << series.name << " " << series.help << "\n";
      out << "# TYPE " << series.name << " gauge\n";
      out << series.name << " " << format_double(series.read()) << "\n";
    }

    return out.str();
  }

  /** @brief The process-wide registry. */
  static Registry &global() {
    static Registry instance;
    return instance;
  }

private:
  template <typename M> struct Series {
    std::string name;
    std::string help;
    std::map<std::string, std::string> labels;
    std::unique_ptr<M> metric;
  };
  using CounterSeries = Series<Counter>;
  using HistogramSeries = Series<Histogram>;

  struct GaugeSeries {
    std::string name;
    std::string help;
    std::function<double()> read;
  };

  /** @brief Identity of one series: its name plus its label values. */
  [[nodiscard]] static std::string
  series_key(const std::string &name,
             const std::map<std::string, std::string> &labels) {
    std::string key = name;
    for (const auto &label : labels) {
      key += '\x1f'; // unit separator: cannot appear in a metric or label name
      key += label.first;
      key += '=';
      key += label.second;
    }
    return key;
  }

  [[nodiscard]] static std::string
  format_labels(const std::map<std::string, std::string> &labels) {
    if (labels.empty()) {
      return "";
    }
    std::string out = "{";
    bool first = true;
    for (const auto &label : labels) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += label.first;
      out += "=\"";
      out += escape_label(label.second);
      out += "\"";
    }
    out += "}";
    return out;
  }

  /**
   * @brief Render a double without a trailing-zero mess.
   *
   * ostringstream's default formatting turns 0.0005 into "0.0005" but 1.0 into
   * "1", which is what Prometheus wants for an `le` label — the bucket label
   * has to match between scrapes or it becomes a different series.
   */
  [[nodiscard]] static std::string format_double(double value) {
    std::ostringstream out;
    out << value;
    return out.str();
  }

  mutable std::mutex mutex_;
  std::map<std::string, CounterSeries> counters_;
  std::map<std::string, HistogramSeries> histograms_;
  std::vector<GaugeSeries> gauges_;
};

} // namespace metrics
} // namespace kvdb
