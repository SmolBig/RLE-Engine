#pragma once
#include <limits>
#include <stdexcept>
#include "MappedFile.h"

struct Run {
  uint64_t prefix; //number of preceeding non-run bytes
  uint64_t length;
  std::byte value;
};

template<class T>
constexpr size_t bitsizeof(T = T{}) {
  constexpr size_t BITS_PER_BYTE = 8;
  return sizeof(T) * BITS_PER_BYTE;
}

#pragma pack(push, 1)
template <typename PrefixT, typename LengthT>
struct PackedNode {
  using PrefixType = PrefixT;
  static constexpr size_t PrefixMax = std::numeric_limits<PrefixType>::max();
  using LengthType = LengthT;
  static constexpr size_t LengthMax = std::numeric_limits<LengthType>::max();

  PrefixT prefix;
  LengthT length;
  std::byte value;

  void set(PrefixType newPrefix, LengthType newLength, std::byte newValue) {
    prefix = newPrefix;
    length = newLength;
    value  = newValue;
  }

  PackedNode() = default;

  PackedNode(PrefixType prefix, LengthType length, std::byte value) :
    prefix(prefix),
    length(length),
    value(value)
  {
    //nop
  }

  uint64_t beSkipNode(uint64_t totalPrefix) {
    constexpr uint64_t byteMax = std::numeric_limits<uint8_t>::max();
    constexpr uint64_t maxSkipLength = PrefixMax | (byteMax << bitsizeof<PrefixType>());

    if(totalPrefix < PrefixMax) {
      throw std::runtime_error("Tried to make a skip node when the prefix is not overloaded.");
    }

    if(totalPrefix > maxSkipLength) {
      set(PrefixMax, 0, (std::byte)byteMax);
      return maxSkipLength;
    }

    uint8_t hiBits = (uint8_t)(totalPrefix >> bitsizeof<PrefixType>());
    PrefixType loBits = (PrefixType)(totalPrefix & PrefixMax);
    set(loBits, 0, (std::byte)hiBits);
    return totalPrefix;
  }

  void beSignalNode(PrefixType prefixSize) {
    set(prefixSize, 0, (std::byte)0);
  }

  uint64_t beLongNode(uint64_t longLength, std::byte runValue) {
    constexpr uint64_t maxLongLength = LengthMax | ((uint64_t)PrefixMax << bitsizeof<LengthType>());

    if(longLength < LengthMax) {
      std::runtime_error("Tried to make a long node when the length is not overloaded.");
    }

    if(longLength > maxLongLength) {
      set(PrefixMax, LengthMax, runValue);
      return maxLongLength;
    }

    uint64_t loLength = longLength & LengthMax;
    uint64_t hiLength = longLength >> bitsizeof<LengthType>();
    set((PrefixType)hiLength, (LengthType)loLength, runValue);
    return longLength;
  }

  uint64_t getLongLength() const {
    uint64_t loBits = length;
    uint64_t hiBits = ((uint64_t)prefix) << bitsizeof<LengthType>();
    return loBits | hiBits;
  }

  uint64_t getSkipLength() const {
    uint64_t loBits = prefix;
    uint64_t hiBits = ((uint64_t)value) << bitsizeof<PrefixType>();
    return loBits | hiBits;
  }
};
#pragma pack(pop)

using Node8x8   = PackedNode<uint8_t, uint8_t>;
using Node8x16  = PackedNode<uint8_t, uint16_t>;
using Node16x8  = PackedNode<uint16_t, uint8_t>;
using Node16x16 = PackedNode<uint16_t, uint16_t>;

enum class NodeFormat {
  P8L8   = 0x11,
  P8L16  = 0x12,
  P16L8  = 0x21,
  P16L16 = 0x22,
  INEFFICIENT
};

#pragma pack(push, 1)
struct Header {
  char magic[4] = "RLE";
  uint64_t decompressedLength = 0;
  uint32_t tableNodeCount = 0;

  void setNodeFormat(NodeFormat format) {
    magic[3] = (char)format;
  }

  NodeFormat checkMagic() const {
    static const std::string EXPECT = "RLE";
    if(!std::equal(EXPECT.begin(), EXPECT.end(), std::span(magic).begin())) {
      throw std::runtime_error("Attempted to reinflate a non RLE file.");
    }
    return (NodeFormat)magic[3];
  }
};
#pragma pack(pop)

