#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#ifdef MICROVTK_HAS_ZLIB
#include <zlib.h>
#endif

#ifdef MICROVTK_HAS_LZ4
#include <lz4.h>
#endif

namespace microvtk::core {

enum class CompressionType : uint8_t { None, ZLib, LZ4 };

// Abstract Compressor Interface
class Compressor {
public:
  virtual ~Compressor() = default;

  // Compresses data and returns the compressed buffer.
  // VTK typically expects raw compressed bytes here, the header logic is
  // handled by the Writer.
  virtual std::vector<uint8_t> compress(std::span<const uint8_t> input) = 0;
};

// ZLib Implementation
#ifdef MICROVTK_HAS_ZLIB
class ZlibCompressor : public Compressor {
public:
  std::vector<uint8_t> compress(std::span<const uint8_t> input) override {
    uLongf destLen = compressBound(static_cast<uLong>(input.size()));
    std::vector<uint8_t> output(destLen);

    int res = ::compress(output.data(), &destLen, input.data(),
                         static_cast<uLong>(input.size()));
    if (res != Z_OK) {
      // In a real lib we might throw or return empty
      return {};
    }
    output.resize(destLen);
    return output;
  }
};
#endif

// LZ4 Implementation
#ifdef MICROVTK_HAS_LZ4
class Lz4Compressor : public Compressor {
public:
  std::vector<uint8_t> compress(std::span<const uint8_t> input) override {
    int maxDstSize = LZ4_compressBound(static_cast<int>(input.size()));
    std::vector<uint8_t> output(maxDstSize);

    int compressedSize =
        LZ4_compress_default(reinterpret_cast<const char*>(input.data()),
                             reinterpret_cast<char*>(output.data()),
                             static_cast<int>(input.size()), maxDstSize);

    if (compressedSize <= 0) {
      return {};
    }
    output.resize(compressedSize);
    return output;
  }
};
#endif

// Factory
inline std::unique_ptr<Compressor> createCompressor(CompressionType type) {
  switch (type) {
    case CompressionType::ZLib:
#ifdef MICROVTK_HAS_ZLIB
      return std::make_unique<ZlibCompressor>();
#else
      return nullptr;
#endif
    case CompressionType::LZ4:
#ifdef MICROVTK_HAS_LZ4
      return std::make_unique<Lz4Compressor>();
#else
      return nullptr;
#endif
    default:
      return nullptr;
  }
}

}  // namespace microvtk::core
