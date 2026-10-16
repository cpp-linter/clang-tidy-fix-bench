#include <jaql/infra/logger.hpp>

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <sstream>
#include <string>

namespace {

// RAII guard: drops a named logger from the spdlog registry on scope exit.
struct LoggerGuard {
  explicit LoggerGuard(std::string name) : name_(std::move(name)) {}
  ~LoggerGuard() { spdlog::drop(name_); }
  LoggerGuard(const LoggerGuard&) = delete;
  LoggerGuard& operator=(const LoggerGuard&) = delete;

  std::string name_;
};

}  // namespace

// ---- get_logger ---------------------------------------------------------------

TEST(Logger, GetLogger_SameName_ReturnsSamePointer) {
  constexpr std::string_view name = "test_same_ptr";
  LoggerGuard guard{std::string(name)};

  auto a = jaql::infra::get_logger(name);
  auto b = jaql::infra::get_logger(name);

  EXPECT_EQ(a.get(), b.get());
}

// ---- configure_logging: level filtering ---------------------------------------

TEST(Logger, ConfigureLogging_LevelWarn_SuppressesLowerMessages) {
  constexpr std::string_view name = "test_level_filter";
  LoggerGuard guard{std::string(name)};

  std::ostringstream oss;
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
  jaql::infra::configure_logging(spdlog::level::warn, sink);

  auto logger = jaql::infra::get_logger(name);

  EXPECT_FALSE(logger->should_log(spdlog::level::trace));
  EXPECT_FALSE(logger->should_log(spdlog::level::debug));
  EXPECT_FALSE(logger->should_log(spdlog::level::info));
  EXPECT_TRUE(logger->should_log(spdlog::level::warn));
  EXPECT_TRUE(logger->should_log(spdlog::level::err));
  EXPECT_TRUE(logger->should_log(spdlog::level::critical));

  // Restore default for subsequent tests.
  jaql::infra::configure_logging(spdlog::level::info);
}

// ---- configure_logging: subsequent loggers ------------------------------------

TEST(Logger, ConfigureLogging_ChangesApplyToSubsequentLoggers) {
  constexpr std::string_view name_a = "test_subsequent_a";
  constexpr std::string_view name_b = "test_subsequent_b";
  LoggerGuard guard_a{std::string(name_a)};
  LoggerGuard guard_b{std::string(name_b)};

  std::ostringstream oss;
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);

  jaql::infra::configure_logging(spdlog::level::debug, sink);
  auto logger_a = jaql::infra::get_logger(name_a);
  EXPECT_EQ(logger_a->level(), spdlog::level::debug);

  jaql::infra::configure_logging(spdlog::level::err, sink);
  auto logger_b = jaql::infra::get_logger(name_b);
  EXPECT_EQ(logger_b->level(), spdlog::level::err);

  // logger_a was created before the second configure_logging call; its level
  // must remain unchanged at debug.
  EXPECT_EQ(logger_a->level(), spdlog::level::debug);

  // Restore default for subsequent tests.
  jaql::infra::configure_logging(spdlog::level::info);
}
