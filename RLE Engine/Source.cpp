#include <vector>
#include <limits>
#include <stdexcept>
#include "MappedFile.h"

#pragma warning(disable : 4458)

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

enum class NodeFormat {
  P8L8   = 0x11,
  P8L16  = 0x12,
  P16L8  = 0x21,
  P16L16 = 0x22,
};

#pragma pack(push, 1)
struct Header {
  char magic[4] = "RLE";
  uint64_t decompressedLength = 0;
  uint32_t tableNodeCount = 0;

  void setNodeFormat(NodeFormat format) {
    magic[3] = (char)format;
  }

  NodeFormat checkMagic() {
    static const std::string EXPECT = "RLE";
    if(!std::equal(EXPECT.begin(), EXPECT.end(), std::span(magic).begin())) {
      throw std::runtime_error("Attempted to reinflate a non RLE file.");
    }
    return (NodeFormat)magic[3];
  }
};

template <typename PrefixT, typename LengthT>
struct PackedNode {
  using PrefixType = PrefixT;
  static constexpr size_t PrefixMax = std::numeric_limits<PrefixType>::max();
  using LengthType = LengthT;
  static constexpr size_t LengthMax = std::numeric_limits<LengthType>::max();

  PrefixT prefix;
  LengthT length;
  std::byte value;

  void set(PrefixType prefix, LengthType length, std::byte value) {
    this->prefix = prefix;
    this->length = length;
    this->value = value;
  }

  PackedNode() = default;

  PackedNode(PrefixType prefix, LengthType length, std::byte value) {
    set(prefix, length, value);
  }

  uint64_t beSkipNode(uint64_t prefix) {
    constexpr uint8_t byteMax = (uint8_t)std::numeric_limits<uint8_t>::max();
    constexpr uint64_t maxSkipLength = PrefixMax | (byteMax << bitsizeof(PrefixMax));

    if(prefix < PrefixMax) {
      throw std::runtime_error("Tried to make a skip node when the prefix is not overloaded.");
    }

    if(prefix > maxSkipLength) {
      set(PrefixMax, 0, (std::byte)byteMax);
      return maxSkipLength;
    }

    uint8_t hiBits = (uint8_t)(prefix >> bitsizeof<PrefixType>());
    PrefixType loBits = (PrefixType)(prefix & PrefixMax);
    set(loBits, 0, (std::byte)hiBits);
    return prefix;
  }

  void beSignalNode(PrefixType prefix) {
    set(prefix, 0, (std::byte)0);
  }

  uint64_t beLongNode(uint64_t longLength, std::byte value) {
    constexpr uint64_t maxLongLength = LengthMax | (PrefixMax << bitsizeof(LengthMax));

    if(longLength < LengthMax) {
      std::runtime_error("Tried to make a long node when the length is not overloaded.");
    }

    if(longLength > maxLongLength) {
      set(PrefixMax, LengthMax, value);
      return maxLongLength;
    }

    uint64_t loLength = longLength & LengthMax;
    uint64_t hiLength = longLength >> bitsizeof(LengthMax);
    set((PrefixType)hiLength, (LengthType)loLength, value);
    return longLength;
  }

  uint64_t getLongLength() const {
    uint64_t loBits = length;
    uint64_t hiBits = prefix << bitsizeof(length);
    return loBits | hiBits;
  }

  uint64_t getSkipLength() const {
    uint64_t loBits = prefix;
    uint64_t hiBits = value << bitsizeof(prefix);
    return loBits | hiBits;
  }
};
#pragma pack(pop)

template <class NodeType>
void parseRun(const Run& run, std::vector<NodeType>& outVec) {
  //push skip nodes until prefix is within range
  uint64_t prefix = run.prefix;
  while(prefix > NodeType::PrefixMax) {
    outVec.emplace_back();
    prefix -= outVec.back().beSkipNode(prefix);
  }

  //push long nodes until length is within range
  uint64_t length = run.length;
  while(length > NodeType::LengthMax) {
    outVec.emplace_back();
    outVec.back().beSignalNode((uint8_t)prefix);
    outVec.emplace_back();
    length -= outVec.back().beLongNode(length, run.value);
  }

  if(run.length <= sizeof(NodeType)) {
    //do not push degenerate nodes
    return;
  }

  //all values should be in range now, so push a standard node
  outVec.emplace_back((typename NodeType::PrefixType)prefix, (typename NodeType::LengthType)length, run.value);
}

template <class NodeType>
int64_t measureEfficiency(const std::vector<NodeType>& nodes) {
  int64_t efficiency = 0;

  bool longNode = false;
  for(const auto& node : nodes) {
    if(longNode) {
      efficiency += node.getLongLength();
      longNode = false;
      continue;
    }

    longNode = node.length == 0 && (uint8_t)node.value == 0;

    efficiency += node.length;
  }

  efficiency -= std::span(nodes).size_bytes();

  return efficiency;
}

struct RLETable {
  template <class NodeType>
  RLETable(NodeFormat format, const std::vector<NodeType>& nodes) : 
    format(format),
    nodeCount(nodes.size())
  {
    auto begin = (const std::byte*)nodes.data();
    auto end = begin + std::span(nodes).size_bytes();
    std::span<std::byte> span(begin, end);
    nodesAsBytes.insert(nodesAsBytes.begin(), span.begin(), span.end());
  }

  NodeFormat format;
  size_t nodeCount;
  std::vector<std::byte> nodesAsBytes;
};

RLETable selectFormatAndGenerateRLETable(const std::vector<Run>& runs) {
  //~~@ This is horrible. Make a function that checks efficiency
  //mathematically from runs vector prior to generating a table.

  std::vector<PackedNode<uint8_t,  uint8_t>>   nodes8x8;
  std::vector<PackedNode<uint8_t,  uint16_t>>  nodes8x16;
  std::vector<PackedNode<uint16_t, uint8_t>>   nodes16x8;
  std::vector<PackedNode<uint16_t, uint16_t>>  nodes16x16;

  for(auto& run : runs) { parseRun(run, nodes8x8); }
  for(auto& run : runs) { parseRun(run, nodes8x16); }
  for(auto& run : runs) { parseRun(run, nodes16x8); }
  for(auto& run : runs) { parseRun(run, nodes16x16); }
  
  auto efficiency8x8   = measureEfficiency(nodes8x8);
  auto efficiency8x16  = measureEfficiency(nodes8x16);
  auto efficiency16x8  = measureEfficiency(nodes16x8);
  auto efficiency16x16 = measureEfficiency(nodes16x16);

  NodeFormat bestFormat = NodeFormat::P8L8;
  auto bestEfficiency = efficiency8x8;
  if(efficiency8x16 > bestEfficiency) {
    bestFormat = NodeFormat::P8L16;
    bestEfficiency = efficiency8x16;
  }
  if(efficiency16x8 > bestEfficiency) {
    bestFormat = NodeFormat::P16L8;
    bestEfficiency = efficiency16x8;
  }
  if(efficiency16x16 > bestEfficiency) {
    bestFormat = NodeFormat::P16L16;
    bestEfficiency = efficiency16x16;
  }

  switch(bestFormat) {
  case NodeFormat::P8L8:   return RLETable(bestFormat, nodes8x8);
  case NodeFormat::P8L16:  return RLETable(bestFormat, nodes8x16);
  case NodeFormat::P16L8:  return RLETable(bestFormat, nodes16x8);
  case NodeFormat::P16L16: return RLETable(bestFormat, nodes16x16);
  }

  throw std::logic_error("Failed to return from available options.");
}

NodeFormat selectFormatMathemagically(const std::vector<Run>& runs) {
  //~~_
  return NodeFormat::P8L8; //~~! placeholder
}

template <class NodeType>
int64_t measureFormatEfficiency(const std::vector<Run>& runs) {
  //~~_
  return 0; //~~! placeholder
}




