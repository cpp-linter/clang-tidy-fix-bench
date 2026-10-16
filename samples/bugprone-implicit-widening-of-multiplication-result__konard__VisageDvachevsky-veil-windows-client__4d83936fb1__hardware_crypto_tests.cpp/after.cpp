#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "common/crypto/crypto_engine.h"
#include "common/crypto/hardware_crypto.h"
#include "common/crypto/hardware_features.h"
#include "common/crypto/random.h"

namespace veil::tests {

// ============================================================================
// Hardware Feature Detection Tests
// ============================================================================

TEST(HardwareFeaturesTests, DetectCpuFeatures) {
  // This test simply verifies that CPU feature detection doesn't crash
  // and returns consistent results
  const auto& features1 = crypto::get_cpu_features();
  const auto& features2 = crypto::get_cpu_features();

  // Same instance should be returned (cached)
  EXPECT_EQ(&features1, &features2);

  // Log detected features for debugging
  const char* features_str = crypto::get_cpu_features_string();
  EXPECT_NE(features_str, nullptr);
  EXPECT_GT(std::strlen(features_str), 0U);
}

TEST(HardwareFeaturesTests, HasHardwareAesConsistent) {
  // Multiple calls should return the same result
  const bool has_aes1 = crypto::has_hardware_aes();
  const bool has_aes2 = crypto::has_hardware_aes();
  EXPECT_EQ(has_aes1, has_aes2);
}

TEST(HardwareFeaturesTests, HasHardwareAesGcmConsistent) {
  const bool has_gcm1 = crypto::has_hardware_aes_gcm();
  const bool has_gcm2 = crypto::has_hardware_aes_gcm();
  EXPECT_EQ(has_gcm1, has_gcm2);

  // If we have AES-GCM, we should also have basic AES
  if (has_gcm1) {
    EXPECT_TRUE(crypto::has_hardware_aes());
  }
}

// ============================================================================
// Hardware-Accelerated Sequence Obfuscation Tests
// ============================================================================

TEST(HardwareCryptoTests, SequenceObfuscationHwRoundTrip) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());

  // Test various sequence values
  const std::vector<std::uint64_t> test_sequences = {
      0,
      1,
      42,
      0x1234567890ABCDEF,
      std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max() - 1,
      0x8000000000000000  // High bit set
  };

  for (const auto original_seq : test_sequences) {
    const auto obfuscated = crypto::obfuscate_sequence_hw(original_seq, key);
    const auto deobfuscated = crypto::deobfuscate_sequence_hw(obfuscated, key);
    EXPECT_EQ(original_seq, deobfuscated)
        << "Failed round-trip for sequence " << original_seq;
  }
}

TEST(HardwareCryptoTests, SequenceObfuscationHwProducesRandomOutput) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());

  // Consecutive sequences should not produce consecutive obfuscated values
  const std::uint64_t seq1 = 1000;
  const std::uint64_t seq2 = 1001;
  const std::uint64_t seq3 = 1002;

  const auto obf1 = crypto::obfuscate_sequence_hw(seq1, key);
  const auto obf2 = crypto::obfuscate_sequence_hw(seq2, key);
  const auto obf3 = crypto::obfuscate_sequence_hw(seq3, key);

  // Obfuscated values should be very different
  EXPECT_NE(obf1, obf2);
  EXPECT_NE(obf2, obf3);
  EXPECT_NE(obf1, obf3);

  // The differences should be large (not just +1)
  EXPECT_GT(std::abs(static_cast<std::int64_t>(obf2 - obf1)), 1000);
  EXPECT_GT(std::abs(static_cast<std::int64_t>(obf3 - obf2)), 1000);
}

TEST(HardwareCryptoTests, SequenceObfuscationHwDiffersByKey) {
  const auto key1_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  const auto key2_vec = crypto::random_bytes(crypto::kAeadKeyLen);

  std::array<std::uint8_t, crypto::kAeadKeyLen> key1{};
  std::array<std::uint8_t, crypto::kAeadKeyLen> key2{};
  std::copy(key1_vec.begin(), key1_vec.end(), key1.begin());
  std::copy(key2_vec.begin(), key2_vec.end(), key2.begin());

  const std::uint64_t sequence = 12345;

  const auto obf1 = crypto::obfuscate_sequence_hw(sequence, key1);
  const auto obf2 = crypto::obfuscate_sequence_hw(sequence, key2);

  // Same sequence with different keys should produce different obfuscated values
  EXPECT_NE(obf1, obf2);
}

TEST(HardwareCryptoTests, SequenceObfuscationHwDeterministic) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());

  const std::uint64_t sequence = 999999;

  // Same sequence and key should always produce the same result
  const auto obf1 = crypto::obfuscate_sequence_hw(sequence, key);
  const auto obf2 = crypto::obfuscate_sequence_hw(sequence, key);

  EXPECT_EQ(obf1, obf2);
}

// ============================================================================
// AES-GCM AEAD Tests
// ============================================================================

TEST(HardwareCryptoTests, AeadEncryptHwRoundTrip) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  const auto nonce_vec = crypto::random_bytes(crypto::kNonceLen);

  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::array<std::uint8_t, crypto::kNonceLen> nonce{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());
  std::copy(nonce_vec.begin(), nonce_vec.end(), nonce.begin());

  const std::vector<std::uint8_t> aad = {'m', 'e', 't', 'a', 'd', 'a', 't', 'a'};
  const std::vector<std::uint8_t> plaintext = {
      'H', 'e', 'l', 'l', 'o', ',', ' ', 'W', 'o', 'r', 'l', 'd', '!'
  };

  // Encrypt
  const auto ciphertext = crypto::aead_encrypt_hw(key, nonce, aad, plaintext);
  ASSERT_FALSE(ciphertext.empty());
  EXPECT_GT(ciphertext.size(), plaintext.size());  // Should include auth tag

  // Decrypt
  const auto decrypted = crypto::aead_decrypt_hw(key, nonce, aad, ciphertext);
  ASSERT_TRUE(decrypted.has_value());
  EXPECT_EQ(decrypted.value(), plaintext);
}

TEST(HardwareCryptoTests, AeadEncryptHwTamperDetection) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  const auto nonce_vec = crypto::random_bytes(crypto::kNonceLen);

  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::array<std::uint8_t, crypto::kNonceLen> nonce{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());
  std::copy(nonce_vec.begin(), nonce_vec.end(), nonce.begin());

  const std::vector<std::uint8_t> aad = {'a', 'a', 'd'};
  const std::vector<std::uint8_t> plaintext = {'s', 'e', 'c', 'r', 'e', 't'};

  // Encrypt
  auto ciphertext = crypto::aead_encrypt_hw(key, nonce, aad, plaintext);
  ASSERT_FALSE(ciphertext.empty());

  // Tamper with ciphertext
  ciphertext[0] ^= 0x01;

  // Decryption should fail
  const auto decrypted = crypto::aead_decrypt_hw(key, nonce, aad, ciphertext);
  EXPECT_FALSE(decrypted.has_value());
}

TEST(HardwareCryptoTests, AeadEncryptHwWrongAad) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  const auto nonce_vec = crypto::random_bytes(crypto::kNonceLen);

  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::array<std::uint8_t, crypto::kNonceLen> nonce{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());
  std::copy(nonce_vec.begin(), nonce_vec.end(), nonce.begin());

  const std::vector<std::uint8_t> aad1 = {'a', 'a', 'd', '1'};
  const std::vector<std::uint8_t> aad2 = {'a', 'a', 'd', '2'};
  const std::vector<std::uint8_t> plaintext = {'d', 'a', 't', 'a'};

  // Encrypt with aad1
  const auto ciphertext = crypto::aead_encrypt_hw(key, nonce, aad1, plaintext);
  ASSERT_FALSE(ciphertext.empty());

  // Decrypt with aad2 should fail
  const auto decrypted = crypto::aead_decrypt_hw(key, nonce, aad2, ciphertext);
  EXPECT_FALSE(decrypted.has_value());
}

TEST(HardwareCryptoTests, AeadEncryptHwToOutputBuffer) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  const auto nonce_vec = crypto::random_bytes(crypto::kNonceLen);

  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::array<std::uint8_t, crypto::kNonceLen> nonce{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());
  std::copy(nonce_vec.begin(), nonce_vec.end(), nonce.begin());

  const std::vector<std::uint8_t> aad = {'a', 'a', 'd'};
  const std::vector<std::uint8_t> plaintext = {'p', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't'};

  // Encrypt to buffer
  std::vector<std::uint8_t> ciphertext_buf(crypto::aead_ciphertext_size(plaintext.size()));
  const auto cipher_len = crypto::aead_encrypt_hw_to(key, nonce, aad, plaintext, ciphertext_buf);
  ASSERT_GT(cipher_len, 0U);
  ciphertext_buf.resize(cipher_len);

  // Decrypt from buffer
  std::vector<std::uint8_t> plaintext_buf(crypto::aead_plaintext_size(ciphertext_buf.size()));
  const auto plain_len = crypto::aead_decrypt_hw_to(key, nonce, aad, ciphertext_buf, plaintext_buf);
  ASSERT_GT(plain_len, 0U);
  plaintext_buf.resize(plain_len);

  EXPECT_EQ(plaintext_buf, plaintext);
}

TEST(HardwareCryptoTests, AeadEncryptHwToInsufficientBuffer) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  const auto nonce_vec = crypto::random_bytes(crypto::kNonceLen);

  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::array<std::uint8_t, crypto::kNonceLen> nonce{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());
  std::copy(nonce_vec.begin(), nonce_vec.end(), nonce.begin());

  const std::vector<std::uint8_t> aad = {};
  const std::vector<std::uint8_t> plaintext(100, 'x');

  // Buffer too small
  std::vector<std::uint8_t> small_buf(10);
  const auto result = crypto::aead_encrypt_hw_to(key, nonce, aad, plaintext, small_buf);
  EXPECT_EQ(result, 0U);  // Should fail
}

// ============================================================================
// Algorithm Selection Tests
// ============================================================================

TEST(HardwareCryptoTests, GetRecommendedAlgorithm) {
  const auto algo = crypto::get_recommended_aead_algorithm();

  // Should return a valid algorithm
  EXPECT_TRUE(algo == crypto::AeadAlgorithm::kChaCha20Poly1305 ||
              algo == crypto::AeadAlgorithm::kAesGcm);

  // If hardware AES-GCM is available, should recommend it
  if (crypto::has_hardware_aes_gcm()) {
    EXPECT_EQ(algo, crypto::AeadAlgorithm::kAesGcm);
  }
}

TEST(HardwareCryptoTests, AlgorithmNameString) {
  EXPECT_STREQ(crypto::aead_algorithm_name(crypto::AeadAlgorithm::kChaCha20Poly1305),
               "ChaCha20-Poly1305");
  EXPECT_STREQ(crypto::aead_algorithm_name(crypto::AeadAlgorithm::kAesGcm),
               "AES-256-GCM");
  EXPECT_STREQ(crypto::aead_algorithm_name(crypto::AeadAlgorithm::kAuto),
               "Auto");
}

TEST(HardwareCryptoTests, EncryptWithAlgorithmChaCha20) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  const auto nonce_vec = crypto::random_bytes(crypto::kNonceLen);

  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::array<std::uint8_t, crypto::kNonceLen> nonce{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());
  std::copy(nonce_vec.begin(), nonce_vec.end(), nonce.begin());

  const std::vector<std::uint8_t> aad = {'a', 'a', 'd'};
  const std::vector<std::uint8_t> plaintext = {'t', 'e', 's', 't'};

  // Encrypt with ChaCha20-Poly1305
  const auto ciphertext = crypto::aead_encrypt_with_algorithm(
      key, nonce, aad, plaintext, crypto::AeadAlgorithm::kChaCha20Poly1305);
  ASSERT_FALSE(ciphertext.empty());

  // Decrypt
  const auto decrypted = crypto::aead_decrypt_with_algorithm(
      key, nonce, aad, ciphertext, crypto::AeadAlgorithm::kChaCha20Poly1305);
  ASSERT_TRUE(decrypted.has_value());
  EXPECT_EQ(decrypted.value(), plaintext);
}

TEST(HardwareCryptoTests, EncryptWithAlgorithmAuto) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  const auto nonce_vec = crypto::random_bytes(crypto::kNonceLen);

  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::array<std::uint8_t, crypto::kNonceLen> nonce{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());
  std::copy(nonce_vec.begin(), nonce_vec.end(), nonce.begin());

  const std::vector<std::uint8_t> aad = {'a', 'u', 't', 'o'};
  const std::vector<std::uint8_t> plaintext = {'a', 'u', 't', 'o', 'm', 'a', 't', 'i', 'c'};

  // Encrypt with auto-selection
  const auto ciphertext = crypto::aead_encrypt_with_algorithm(
      key, nonce, aad, plaintext, crypto::AeadAlgorithm::kAuto);
  ASSERT_FALSE(ciphertext.empty());

  // Decrypt with auto-selection
  const auto decrypted = crypto::aead_decrypt_with_algorithm(
      key, nonce, aad, ciphertext, crypto::AeadAlgorithm::kAuto);
  ASSERT_TRUE(decrypted.has_value());
  EXPECT_EQ(decrypted.value(), plaintext);
}

// ============================================================================
// Compatibility Tests (HW vs SW produce compatible results)
// ============================================================================

TEST(HardwareCryptoTests, HwSwCompatibilitySequenceObfuscation) {
  // Both HW and SW implementations should produce round-trip compatible results
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());

  for (std::uint64_t seq = 0; seq < 100; ++seq) {
    // HW obfuscate -> HW deobfuscate
    const auto hw_obf = crypto::obfuscate_sequence_hw(seq, key);
    const auto hw_deobf = crypto::deobfuscate_sequence_hw(hw_obf, key);
    EXPECT_EQ(seq, hw_deobf) << "HW round-trip failed for seq " << seq;

    // SW obfuscate -> SW deobfuscate (original implementation)
    const auto sw_obf = crypto::obfuscate_sequence(seq, key);
    const auto sw_deobf = crypto::deobfuscate_sequence(sw_obf, key);
    EXPECT_EQ(seq, sw_deobf) << "SW round-trip failed for seq " << seq;
  }
}

TEST(HardwareCryptoTests, LargeDataEncryption) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  const auto nonce_vec = crypto::random_bytes(crypto::kNonceLen);

  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::array<std::uint8_t, crypto::kNonceLen> nonce{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());
  std::copy(nonce_vec.begin(), nonce_vec.end(), nonce.begin());

  // Create large plaintext (1MB)
  const std::size_t size = static_cast<std::size_t>(1024) * 1024;
  std::vector<std::uint8_t> plaintext(size);
  for (std::size_t i = 0; i < size; ++i) {
    plaintext[i] = static_cast<std::uint8_t>(i % 256);
  }

  const std::vector<std::uint8_t> aad = {'l', 'a', 'r', 'g', 'e'};

  // Encrypt
  const auto ciphertext = crypto::aead_encrypt_hw(key, nonce, aad, plaintext);
  ASSERT_FALSE(ciphertext.empty());

  // Decrypt
  const auto decrypted = crypto::aead_decrypt_hw(key, nonce, aad, ciphertext);
  ASSERT_TRUE(decrypted.has_value());
  EXPECT_EQ(decrypted.value(), plaintext);
}

TEST(HardwareCryptoTests, EmptyPlaintextEncryption) {
  const auto key_vec = crypto::random_bytes(crypto::kAeadKeyLen);
  const auto nonce_vec = crypto::random_bytes(crypto::kNonceLen);

  std::array<std::uint8_t, crypto::kAeadKeyLen> key{};
  std::array<std::uint8_t, crypto::kNonceLen> nonce{};
  std::copy(key_vec.begin(), key_vec.end(), key.begin());
  std::copy(nonce_vec.begin(), nonce_vec.end(), nonce.begin());

  const std::vector<std::uint8_t> aad = {'e', 'm', 'p', 't', 'y'};
  const std::vector<std::uint8_t> plaintext = {};  // Empty

  // Encrypt empty plaintext
  const auto ciphertext = crypto::aead_encrypt_hw(key, nonce, aad, plaintext);
  // Should still produce ciphertext (just the auth tag)
  EXPECT_FALSE(ciphertext.empty());

  // Decrypt
  const auto decrypted = crypto::aead_decrypt_hw(key, nonce, aad, ciphertext);
  ASSERT_TRUE(decrypted.has_value());
  EXPECT_TRUE(decrypted.value().empty());
}

}  // namespace veil::tests
