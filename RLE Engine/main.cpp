#include <vector>
#include <limits>
#include <stdexcept>
#include <unordered_map>
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
    constexpr uint64_t byteMax = std::numeric_limits<uint8_t>::max();
    constexpr uint64_t maxSkipLength = PrefixMax | (byteMax << bitsizeof<PrefixType>());

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
    constexpr uint64_t maxLongLength = LengthMax | ((uint64_t)PrefixMax << bitsizeof<LengthType>());

    if(longLength < LengthMax) {
      std::runtime_error("Tried to make a long node when the length is not overloaded.");
    }

    if(longLength > maxLongLength) {
      set(PrefixMax, LengthMax, value);
      return maxLongLength;
    }

    uint64_t loLength = longLength & LengthMax;
    uint64_t hiLength = longLength >> bitsizeof<LengthType>();
    set((PrefixType)hiLength, (LengthType)loLength, value);
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

  //all values should be in range now, so push a standard node (if it's not degenerate)
  if(length > sizeof(NodeType)) {
    outVec.emplace_back((typename NodeType::PrefixType)prefix, (typename NodeType::LengthType)length, run.value);
  }
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
  RLETable() = default;

  template <class NodeType>
  RLETable(NodeFormat format, int64_t efficiency, const std::vector<NodeType>& nodes) : 
    format(format),
    efficiency(efficiency),
    nodeCount((uint32_t)nodes.size())
  {
    if(nodes.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("RLE table too large.");
    }
    std::span<const std::byte> span(reinterpret_cast<const std::byte*>(nodes.data()), std::span(nodes).size_bytes());
    nodesAsBytes.insert(nodesAsBytes.begin(), span.begin(), span.end());
  }

  NodeFormat format;
  int64_t efficiency;
  uint32_t nodeCount;
  std::vector<std::byte> nodesAsBytes;
};

template <class NodeType>
int64_t calculateRunEfficiencyByFormat(const Run& run) {
  uint64_t nodesGenerated = 0;
  uint64_t lengthProcessed = 0;

  // account for skip nodes
  if(run.prefix > NodeType::PrefixMax) {
    constexpr uint64_t byteMax = std::numeric_limits<uint8_t>::max();
    constexpr uint64_t maxSkipLength = NodeType::PrefixMax | (byteMax << bitsizeof<NodeType::PrefixType>());
    uint64_t maxSkips  = run.prefix / maxSkipLength;
    uint64_t remainder = run.prefix % maxSkipLength;
    nodesGenerated += maxSkips;
    if(remainder > NodeType::PrefixMax) { nodesGenerated++; }
  }

  // account for signal & long nodes
  auto length = run.length;
  if(length > NodeType::LengthMax) {
    constexpr uint64_t longNodeMax = ((uint64_t)NodeType::LengthMax << bitsizeof<NodeType::PrefixType>()) | std::numeric_limits<NodeType::PrefixType>::max();
    uint64_t maxLongs  = length / longNodeMax;
    uint64_t remainder = length % longNodeMax;
    nodesGenerated += maxLongs * 2;
    length -= maxLongs * longNodeMax;
    lengthProcessed += maxLongs * longNodeMax;
    if(remainder > NodeType::LengthMax) {
      nodesGenerated += 2;
      length -= remainder;
      lengthProcessed += remainder;
    }
  }

  // account for standard node
  if(length > sizeof(NodeType)) {
    nodesGenerated++;
    lengthProcessed += length;
  }

  return lengthProcessed - (nodesGenerated * sizeof(NodeType));
}

template <class NodeType>
int64_t calculateFormatEfficiency(const std::vector<Run>& runs) {
  int64_t efficiency = 0;
  for(auto& run : runs) {
    efficiency += calculateRunEfficiencyByFormat<NodeType>(run);
  }
  return efficiency;
}

using Node8x8   = PackedNode<uint8_t,  uint8_t>;
using Node8x16  = PackedNode<uint8_t,  uint16_t>;
using Node16x8  = PackedNode<uint16_t, uint8_t>;
using Node16x16 = PackedNode<uint16_t, uint16_t>;

std::pair<NodeFormat, int64_t> selectFormat(const std::vector<Run>& runs) {
  std::unordered_map<NodeFormat, int64_t> efficiencies{
    { NodeFormat::P8L8,   calculateFormatEfficiency<Node8x8>(runs)   },
    { NodeFormat::P8L16,  calculateFormatEfficiency<Node8x16>(runs)  },
    { NodeFormat::P16L8,  calculateFormatEfficiency<Node16x8>(runs)  },
    { NodeFormat::P16L16, calculateFormatEfficiency<Node16x16>(runs) }
  };

  int64_t bestEfficiency = 0;
  NodeFormat bestFormat = NodeFormat::INEFFICIENT;
  for(auto p : efficiencies) {
    if(p.second > bestEfficiency) {
      bestEfficiency = p.second;
      bestFormat = p.first;
    }
  }

  return std::make_pair(bestFormat, bestEfficiency);
}

template <class NodeType>
RLETable generateRLETable(NodeFormat format, int64_t efficiency, const std::vector<Run>& runs) {
  std::vector<NodeType> nodes;
  for(auto& run : runs) { parseRun(run, nodes); }
  return RLETable(format, efficiency, nodes);
}

void reject() {
  throw std::runtime_error("Cannot deflate this file efficiently. Reject it. (Rejection not yet implemented.)");
}

template <class NodeType>
void deflateData(MappedFile::View& inView, MappedFile::View& outView) {
  Header* header = reinterpret_cast<Header*>(outView.data());
  const NodeType* nodesPtr = reinterpret_cast<const NodeType*>(outView.data() + sizeof(Header));
  std::span<const NodeType> nodes(nodesPtr, header->tableNodeCount);

  auto inIter = inView.begin();
  auto outIter = outView.begin() + sizeof(Header) + nodes.size_bytes();

  bool longNode = false;
  for(auto& node : nodes) {
    if(longNode) {
      inIter += node.getLongLength();
      longNode = false;
      continue;
    }

    size_t prefix = node.prefix;
    if(node.length == 0) {
      if(node.value == (std::byte)0) {
        longNode = true;
      }
      else {
        prefix = node.getSkipLength();
      }
    }

    auto inTail = inIter + prefix;
    outIter = std::copy(inIter, inTail, outIter);
    //tailIter is past the prefix, add the run length to that to get the new inIter
    inIter = inTail + node.length;
  }

  std::copy(inIter, inView.end(), outIter);
}

std::vector<Run> collectRuns(const std::span<std::byte>& data) {
  std::vector<Run> runs;

  Run run;
  size_t prevTailPos = 0;
  for(size_t i = 0; i < data.size(); ) {
    auto position = i;
    run.length = 1;
    run.value = data[i];

    while((++i < data.size()) && (data[i] == run.value) && (run.length < std::numeric_limits<decltype(run.length)>::max())) {
      run.length++;
    }

    if(run.length > sizeof(Node8x8)) {
      run.prefix = position - prevTailPos;
      runs.push_back(run);
      prevTailPos = position + run.length;
    }
  }

  return runs;
}

void deflateFile(const std::string& inputFilename, const std::string& outputFilename) {
  MappedFile inMap(inputFilename, MappedFile::CreationDisposition::OPEN);
  auto inView = inMap.getView(0, inMap.size());

  std::vector<Run> runs = collectRuns(inView);

  auto selection = selectFormat(runs);
  auto format = selection.first;
  auto efficiency = selection.second;

  RLETable table;
  switch(format) {
  case NodeFormat::P8L8:   table = generateRLETable<Node8x8  >(format, efficiency, runs); break;
  case NodeFormat::P8L16:  table = generateRLETable<Node8x16 >(format, efficiency, runs); break;
  case NodeFormat::P16L8:  table = generateRLETable<Node16x8 >(format, efficiency, runs); break;
  case NodeFormat::P16L16: table = generateRLETable<Node16x16>(format, efficiency, runs); break;
  case NodeFormat::INEFFICIENT:
    reject();
    return;
  };

  uint64_t compressedLength = inMap.size() - table.efficiency + sizeof(Header);
  MappedFile outMap(outputFilename, MappedFile::CreationDisposition::CREATE, compressedLength);
  auto outView = outMap.getView(0, outMap.size());

  Header* header = new(outView.data()) Header;
  header->setNodeFormat(format);
  header->decompressedLength = inMap.size();
  header->tableNodeCount = table.nodeCount;

  auto outIter = outView.begin() + sizeof(Header);
  std::copy(table.nodesAsBytes.begin(), table.nodesAsBytes.end(), outIter);

  switch(format) {
  case NodeFormat::P8L8:   deflateData<Node8x8  >(inView, outView); break;
  case NodeFormat::P8L16:  deflateData<Node8x16 >(inView, outView); break;
  case NodeFormat::P16L8:  deflateData<Node16x8 >(inView, outView); break;
  case NodeFormat::P16L16: deflateData<Node16x16>(inView, outView); break;
  default: throw std::logic_error("Failed switch to format.");
  }
}

template <class NodeType>
std::vector<Run> extractTable(const void* data, size_t nodeCount) {
  std::vector<Run> outVec;
  outVec.reserve(nodeCount);

  std::span<const NodeType> nodes(reinterpret_cast<const NodeType*>(data), nodeCount);

  Run run{};
  for(auto iter = nodes.begin(); iter != nodes.end(); iter++) {
    if(iter->length == 0) {
      if(iter->value == (std::byte)0) { //signal&long node
        run.prefix += iter->prefix;
        iter++;
        run.length = iter->getLongLength();
        run.value = iter->value;

        outVec.push_back(run);
        run.prefix = 0;
        run.length = 0;
        run.value = (std::byte)0;

        continue;
      }
      else { //skip node
        run.prefix += iter->getSkipLength();
        continue;
      }
    }

    //standard
    run.prefix += iter->prefix;
    run.length = iter->length;
    run.value = iter->value;

    outVec.push_back(run);
    run.prefix = 0;
    run.length = 0;
    run.value = (std::byte)0;
  }

  return outVec;
}

std::vector<Run> extractTableByFormat(const void* data, size_t nodeCount, NodeFormat format) {
  std::vector<Run> outVec;
  switch(format) {
  case NodeFormat::P8L8:   return extractTable<Node8x8  >(data, nodeCount);
  case NodeFormat::P8L16:  return extractTable<Node8x16 >(data, nodeCount);
  case NodeFormat::P16L8:  return extractTable<Node16x8 >(data, nodeCount);
  case NodeFormat::P16L16: return extractTable<Node16x16>(data, nodeCount);
  };

  throw std::logic_error("Failed to switch by format type.");
}

void inflateFile(const std::string& inputFilename, const std::string& outputFilename) {
  auto inMap = MappedFile(inputFilename, MappedFile::CreationDisposition::OPEN);
  auto inView = inMap.getView(0, inMap.size());
  auto inIter = inView.begin();

  const Header* header = reinterpret_cast<Header*>(inView.data());
  inIter += sizeof(Header);
  auto format = header->checkMagic();
  size_t nodeSize = 0;
  switch(format) {
  case NodeFormat::P8L8:   nodeSize = sizeof(Node8x8);   break;
  case NodeFormat::P8L16:  nodeSize = sizeof(Node8x16);  break;
  case NodeFormat::P16L8:  nodeSize = sizeof(Node16x8);  break;
  case NodeFormat::P16L16: nodeSize = sizeof(Node16x16); break;
  }
  
  size_t tableByteSize = header->tableNodeCount * nodeSize;
  auto table = extractTableByFormat(inView.data() + sizeof(Header), header->tableNodeCount, format);
  inIter += tableByteSize;

  auto outMap = MappedFile(outputFilename, MappedFile::CreationDisposition::CREATE, header->decompressedLength);
  auto outView = outMap.getView(0, outMap.size());

  auto outIter = outView.begin();

  for(auto& node : table) {
    auto inTail = inIter + node.prefix;
    outIter = std::copy(inIter, inTail, outIter);
    inIter = inTail;

    auto outTail = outIter + node.length;
    std::fill(outIter, outTail, node.value);
    outIter = outTail;
  }
  
  outIter = std::copy(inIter, inView.end(), outIter);
  if(outIter != outView.end()) {
    throw std::runtime_error("Inflated file does not match expected length.");
  }
}

#include <filesystem>
#include <iostream>

void primaryTest() {
  std::string testfile = "testfile.txt";
  std::string deflated = testfile + ".rle";
  std::string inflated = "reinflated.txt";

  std::filesystem::remove(deflated);
  std::filesystem::remove(inflated);

  deflateFile(testfile, deflated);
  inflateFile(deflated, inflated);

  MappedFile testMap(testfile, MappedFile::CreationDisposition::OPEN);
  auto infData = testMap.getView(0, testMap.size());
  MappedFile reinfMap(inflated, MappedFile::CreationDisposition::OPEN);
  auto defData = reinfMap.getView(0, reinfMap.size());
  std::cout << "Equality Test: " << (std::equal(infData.begin(), infData.end(), defData.begin(), defData.end()) ? "Pass" : "Fail") << "\n";
  auto deflatedSize = std::filesystem::file_size(std::filesystem::path(deflated));
  auto compression = (float)((deflatedSize * 10000) / testMap.size()) / 100;
  std::cout << "Compressed Length Percentage: " << compression << "\n";
  std::cout << std::endl;

  system("pause");

}

void efficiencyCalcTest() {
  std::vector<Run> runs;
  {
    MappedFile inMap("testfile.txt", MappedFile::CreationDisposition::OPEN);
    auto inView = inMap.getView(0, inMap.size());
    runs = collectRuns(inView);
  }

  auto c11 = calculateFormatEfficiency<Node8x8>(runs);
  auto c12 = calculateFormatEfficiency<Node8x16>(runs);
  auto c21 = calculateFormatEfficiency<Node16x8>(runs);
  auto c22 = calculateFormatEfficiency<Node16x16>(runs);

  std::vector<Node8x8> nodes11;
  std::vector<Node8x16> nodes12;
  std::vector<Node16x8> nodes21;
  std::vector<Node16x16> nodes22;

  for(auto& run : runs) {
    parseRun(run, nodes11);
    parseRun(run, nodes12);
    parseRun(run, nodes21);
    parseRun(run, nodes22);
  }

  int64_t m11 = measureEfficiency(nodes11);
  int64_t m12 = measureEfficiency(nodes12);
  int64_t m21 = measureEfficiency(nodes21);
  int64_t m22 = measureEfficiency(nodes22);

  if(c11 != m11) { __debugbreak(); }
  if(c12 != m12) { __debugbreak(); }
  if(c21 != m21) { __debugbreak(); }
  if(c22 != m22) { __debugbreak(); }
}

#include <Windows.h>

int main() {
  efficiencyCalcTest();
  primaryTest();
}








