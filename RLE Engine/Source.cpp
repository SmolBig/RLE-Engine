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
    uint64_t hiBits = ((uint64_t)prefix) << bitsizeof(length);
    return loBits | hiBits;
  }

  uint64_t getSkipLength() const {
    uint64_t loBits = prefix;
    uint64_t hiBits = ((uint64_t)value) << bitsizeof(prefix);
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
int64_t measureRunEfficiencyByFormat(const Run& run) {
  uint64_t nodesGenerated = 0;

  // account for skip nodes
  if(run.prefix > NodeType::PrefixMax) {
    constexpr uint64_t skipNodeMax = ((uint64_t)NodeType::PrefixMax << bitsizeof<uint8_t>()) | std::numeric_limits<uint8_t>::max();
    uint64_t maxSkips  = run.prefix / skipNodeMax;
    uint64_t remainder = run.prefix % skipNodeMax;
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
    if(remainder > NodeType::LengthMax) {
      nodesGenerated += 2;
      length -= remainder;
    }
  }

  // account for standard node
  if(length > sizeof(NodeType)) {
    nodesGenerated++;
  }

  return run.length - (nodesGenerated * sizeof(NodeType));
}

template <class NodeType>
int64_t measureFormatEfficiency(const std::vector<Run>& runs) {
  int64_t efficiency = 0;
  for(auto& run : runs) {
    efficiency += measureRunEfficiencyByFormat<NodeType>(run);
  }
  return efficiency;
}

using Node8x8   = PackedNode<uint8_t,  uint8_t>;
using Node8x16  = PackedNode<uint8_t,  uint16_t>;
using Node16x8  = PackedNode<uint16_t, uint8_t>;
using Node16x16 = PackedNode<uint16_t, uint16_t>;

std::pair<NodeFormat, int64_t> selectFormat(const std::vector<Run>& runs) {
  std::unordered_map<NodeFormat, int64_t> efficiencies{
    { NodeFormat::P8L8,   measureFormatEfficiency<Node8x8>(runs) },
    { NodeFormat::P8L16,  measureFormatEfficiency<Node8x16>(runs) },
    { NodeFormat::P16L8,  measureFormatEfficiency<Node16x8>(runs) },
    { NodeFormat::P16L16, measureFormatEfficiency<Node16x16>(runs) }
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
  auto outIter = outView.begin();
  outIter += sizeof(Header);
  outIter += nodes.size_bytes();

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

    auto tailIter = inIter + prefix;
    outIter = std::copy(inIter, tailIter, outIter);
    //tailIter is past the prefix, add the run length to that to get the new srcIter
    inIter = tailIter + node.length;
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

  uint64_t compressedLength = inMap.size() - table.efficiency;
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
  auto headerView = inMap.getView(0, sizeof(Header));

  const Header* header = reinterpret_cast<Header*>(headerView.data());
  auto format = header->checkMagic();
  size_t nodeSize = 0;
  switch(format) {
  case NodeFormat::P8L8:   nodeSize *= sizeof(Node8x8);   break;
  case NodeFormat::P8L16:  nodeSize *= sizeof(Node8x16);  break;
  case NodeFormat::P16L8:  nodeSize *= sizeof(Node16x8);  break;
  case NodeFormat::P16L16: nodeSize *= sizeof(Node16x16); break;
  }
  
  size_t tableByteSize = header->tableNodeCount * nodeSize;
  auto tableView = inMap.getView(sizeof(Header), tableByteSize);
  auto table = extractTableByFormat(tableView.data(), header->tableNodeCount, format);

  size_t bytesSoFar = sizeof(Header) - tableByteSize;
  auto dataView = inMap.getView(bytesSoFar, inMap.size() - bytesSoFar);

  auto outMap = MappedFile(outputFilename, MappedFile::CreationDisposition::CREATE, header->decompressedLength);
  auto outView = outMap.getView(0, outMap.size());

  auto inIter = dataView.begin();
  auto outIter = outView.begin();

  for(auto& node : table) {
    auto inTail = inIter + node.prefix;
    outIter = std::copy(inIter, inTail, outIter);
    inIter = inTail;

    auto outTail = outIter + node.length;
    std::fill(outIter, outTail, node.value);
    outIter = outTail;
  }
  
  outIter = std::copy(inIter, dataView.end(), outIter);
  if(outIter != outView.end()) {
    throw std::runtime_error("Inflated file does not match expected length.");
  }
}

#include <filesystem>
#include <iostream>

int main() {
  std::string testfile = "testfile.txt";
  std::string deflated = testfile + ".rle";
  std::string inflated = "reinflated.txt";

  std::filesystem::remove(deflated);
  std::filesystem::remove(inflated);

  deflateFile(testfile, deflated);
  inflateFile(deflated, inflated);

  MappedFile infMap(inflated, MappedFile::CreationDisposition::OPEN);
  auto infData = infMap.getView(0, infMap.size());
  MappedFile defMap(deflated, MappedFile::CreationDisposition::OPEN);
  auto defData = defMap.getView(0, defMap.size());
  std::cout << "Equality Test: " << (std::equal(infData.begin(), infData.end(), defData.begin(), defData.end()) ? "Pass" : "Fail") << "\n";
  auto compression = (float)((defMap.size() * 10000) / infMap.size()) / 100;
  std::cout << "Compressed Length Percentage: " << compression << "\n";
  std::cout << std::endl;

  system("pause");

}
