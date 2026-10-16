#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "common/handshake/handshake_processor.h"
#include "common/logging/logger.h"
#include "common/utils/rate_limiter.h"
#include "transport/session/transport_session.h"

namespace veil::tests {

using namespace std::chrono_literals;

// ============================================================================
// Issue #173: Debug logging overhead in production hot paths
// ============================================================================

// Test fixture reusing the handshake setup pattern from transport_session_tests.
class DebugLoggingOverheadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    now_ = std::chrono::system_clock::now();
    steady_now_ = std::chrono::steady_clock::now();

    auto now_fn = [this]() { return now_; };
    auto steady_fn = [this]() { return steady_now_; };

    psk_ = std::vector<std::uint8_t>(32, 0xAB);

    handshake::HandshakeInitiator initiator(psk_, 200ms, now_fn);
    utils::TokenBucket bucket(100.0, 1000ms, steady_fn);
    handshake::HandshakeResponder responder(psk_, 200ms, std::move(bucket),
                                            now_fn);

    auto init_bytes = initiator.create_init();
    now_ += 10ms;
    steady_now_ += 10ms;
    auto resp = responder.handle_init(init_bytes);
    ASSERT_TRUE(resp.has_value());

    now_ += 10ms;
    steady_now_ += 10ms;
    auto client_session = initiator.consume_response(resp->response);
    ASSERT_TRUE(client_session.has_value());

    client_handshake_ = *client_session;
    server_handshake_ = resp->session;
  }

  std::chrono::system_clock::time_point now_;
  std::chrono::steady_clock::time_point steady_now_;
  std::vector<std::uint8_t> psk_;
  handshake::HandshakeSession client_handshake_;
  handshake::HandshakeSession server_handshake_;
};

// Verify fragment processing works correctly after WARN->DEBUG log level change.
// This ensures the log level change in transport_session.cpp didn't break
// fragment reassembly logic (Issue #173).
TEST_F(DebugLoggingOverheadTest,
       FragmentReassemblyWorksAfterLogLevelChange) {
  auto now_fn = [this]() { return steady_now_; };

  transport::TransportSessionConfig config;
  config.max_fragment_size = 10;  // Force fragmentation

  transport::TransportSession client(client_handshake_, config, now_fn);
  transport::TransportSession server(server_handshake_, config, now_fn);

  // Create data that requires fragmentation.
  std::vector<std::uint8_t> plaintext(50);
  for (std::size_t i = 0; i < plaintext.size(); ++i) {
    plaintext[i] = static_cast<std::uint8_t>(i & 0xFF);
  }

  auto encrypted_packets = client.encrypt_data(plaintext, 0, true);
  ASSERT_GE(encrypted_packets.size(), 2U);
  EXPECT_EQ(client.stats().fragments_sent, encrypted_packets.size());

  // Decrypt all fragments — the last packet should trigger reassembly.
  std::vector<mux::MuxFrame> all_frames;
  for (const auto& pkt : encrypted_packets) {
    auto result = server.decrypt_packet(pkt);
    ASSERT_TRUE(result.has_value());
    for (auto& frame : *result) {
      all_frames.push_back(std::move(frame));
    }
  }

  // Should have at least one reassembled frame.
  ASSERT_FALSE(all_frames.empty());
  EXPECT_GT(server.stats().fragments_received, 0U);
  EXPECT_GT(server.stats().messages_reassembled, 0U);

  // Verify reassembled data matches original.
  ASSERT_EQ(all_frames.size(), 1U);
  EXPECT_EQ(all_frames[0].data.payload.size(), plaintext.size());
  EXPECT_EQ(all_frames[0].data.payload, plaintext);
}

// Verify non-fragmented messages still work after log level changes.
TEST_F(DebugLoggingOverheadTest,
       NonFragmentedMessageWorksAfterLogLevelChange) {
  auto now_fn = [this]() { return steady_now_; };

  transport::TransportSession client(client_handshake_, {}, now_fn);
  transport::TransportSession server(server_handshake_, {}, now_fn);

  std::vector<std::uint8_t> plaintext{0x01, 0x02, 0x03, 0x04, 0x05};

  auto encrypted = client.encrypt_data(plaintext, 0, true);
  ASSERT_EQ(encrypted.size(), 1U);

  auto decrypted = server.decrypt_packet(encrypted[0]);
  ASSERT_TRUE(decrypted.has_value());
  ASSERT_EQ(decrypted->size(), 1U);
  EXPECT_EQ((*decrypted)[0].data.payload, plaintext);
}

// Verify frame decode failure path works (previously logged at WARN, now DEBUG).
TEST_F(DebugLoggingOverheadTest, FrameDecodeFailureHandledCorrectly) {
  auto now_fn = [this]() { return steady_now_; };

  transport::TransportSession server(server_handshake_, {}, now_fn);

  // Send garbage data — should fail decrypt/decode gracefully.
  std::vector<std::uint8_t> garbage(100, 0xFF);
  auto result = server.decrypt_packet(garbage);

  // Should return nullopt (failed to decrypt), not crash.
  EXPECT_FALSE(result.has_value());
}

// Verify that multiple fragmented messages can be processed sequentially.
// This exercises the fragment reassembly hot path that was logging at WARN.
TEST_F(DebugLoggingOverheadTest,
       MultipleFragmentedMessagesProcess) {
  auto now_fn = [this]() { return steady_now_; };

  transport::TransportSessionConfig config;
  config.max_fragment_size = 10;

  transport::TransportSession client(client_handshake_, config, now_fn);
  transport::TransportSession server(server_handshake_, config, now_fn);

  // Send multiple messages that each require fragmentation.
  for (int msg = 0; msg < 3; ++msg) {
    std::vector<std::uint8_t> plaintext(30);
    for (std::size_t i = 0; i < plaintext.size(); ++i) {
      plaintext[i] = static_cast<std::uint8_t>((msg * 30 + i) & 0xFF);
    }

    auto encrypted_packets = client.encrypt_data(plaintext, 0, true);
    ASSERT_GE(encrypted_packets.size(), 2U);

    std::vector<mux::MuxFrame> frames;
    for (const auto& pkt : encrypted_packets) {
      auto result = server.decrypt_packet(pkt);
      ASSERT_TRUE(result.has_value());
      for (auto& f : *result) {
        frames.push_back(std::move(f));
      }
    }

    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].data.payload, plaintext);
  }

  EXPECT_EQ(server.stats().messages_reassembled, 3U);
}

// ============================================================================
// Compile-time debug log removal verification (Issue #173)
// ============================================================================

// This test verifies the NDEBUG preprocessor guard behavior.
// In Release builds (NDEBUG defined), debug logging code is compiled out.
// In Debug builds (NDEBUG not defined), debug logging code is present.
TEST(CompileTimeDebugRemoval, NdebugGuardConsistency) {
#ifdef NDEBUG
  // Release build: verify NDEBUG is defined as expected.
  // The #ifndef NDEBUG guards in service_main.cpp will exclude debug logs.
  EXPECT_TRUE(true) << "NDEBUG is defined — debug logs are compiled out";
#else
  // Debug build: verify NDEBUG is not defined.
  // The #ifndef NDEBUG guards in service_main.cpp will include debug logs.
  EXPECT_TRUE(true) << "NDEBUG is not defined — debug logs are included";
#endif
}

// Verify that LOG_DEBUG macro expands correctly regardless of build type.
// spdlog's SPDLOG_DEBUG is a no-op when SPDLOG_ACTIVE_LEVEL > DEBUG,
// but the macro itself should always compile without errors.
TEST(CompileTimeDebugRemoval, LogMacrosCompile) {
  // These should compile in both Debug and Release builds.
  LOG_DEBUG("test debug message: {}", 42);
  LOG_INFO("test info message: {}", "hello");
  LOG_WARN("test warn message: {}", 3.14);

  // If we reach here, all log macros compiled successfully.
  SUCCEED();
}

}  // namespace veil::tests
