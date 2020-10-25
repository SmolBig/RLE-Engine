#include <iostream>
#include <string>
#include <vector>
#include <limits>
#include <filesystem>
#include "MappedFile.h"

//~~@ replace fstreams with maps
#include <fstream>

//~~@ modify skip node to use shifted bits instead of multiplication
/*~~_ implement system to determine most efficient node type based on runs vector
 * - run parseRuns for each available type
 * - once it works, thread the tests
 * - return the best table and it's NodeType
 * - modify the inflate routine to make use of the header-specified NodeType
 * - apply the modification to the deflation routine
 */
//~~_ get a nice set of various test files going; make sure they test skip/long nodes (and combos)

using byte = uint8_t;

template <typename PrefixT, typename LengthT>
struct BaseRLENode {
  using PrefixType = PrefixT;
  static constexpr size_t PrefixMax = std::numeric_limits<PrefixType>::max();
  using LengthType = LengthT;
  static constexpr size_t LengthMax = std::numeric_limits<LengthType>::max();

  PrefixType prefix; //number of preceeding non-run bytes
  LengthType length;
  byte value;

  //~~@ add functions to make special node types
};

#pragma pack(push, 1)
template <typename PrefixT, typename LengthT>
struct BasePackedRLENode {
  PrefixT prefix;
  LengthT length;
  byte value;
};
#pragma pack(pop)

template<class T>
constexpr size_t bitsizeof(T = T{}) {
  constexpr size_t BITS_PER_BYTE = 8;
  return sizeof(T) * BITS_PER_BYTE;
}

#pragma pack(push, 1)
struct Header {
  char magic[4] = "RLE";
  uint64_t decompressedLength = 0;
  uint32_t tableNodeCount = 0;

  enum class NodeType {
    P8L8   = 0x11,
    P8L16  = 0x12,
    P16L8  = 0x21,
    P16L16 = 0x22,
  };

  NodeType checkMagic() {
    static const std::string EXPECT = "RLE";
    if(!std::equal(EXPECT.begin(), EXPECT.end(), std::span(magic).begin())) {
      throw std::runtime_error("Attempted to decompress a non RLE file.");
    }
    return (NodeType)magic[3];
  }
};
#pragma pack(pop)

using Run = BaseRLENode<uint64_t, uint64_t>;

template <class PackedNodeType, class ContainerType>
std::vector<Run> collectRuns(const ContainerType& data) {
  std::vector<Run> runs;
  runs.reserve(100);

  Run run;
  size_t prevTailPos = 0;
  for(size_t i = 0; i < data.size(); ) {
    auto position = i;
    run.length = 1;
    run.value = data[i];

    while((++i < data.size()) && (data[i] == run.value) && (run.length < std::numeric_limits<decltype(run.length)>::max())) {
      run.length++;
    }

    if(run.length > sizeof(PackedNodeType)) {
      run.prefix = position - prevTailPos;
      runs.push_back(run);
      prevTailPos = position + run.length;
    }
  }

  return runs;
}

template <class NodeType>
constexpr size_t lengthFromLongNode(const NodeType& node) {
  size_t retval = node.prefix;
  retval <<= bitsizeof<NodeType::LengthType>();
  retval += node.length;
  return retval;
}

template <class NodeType>
NodeType makeLongNode(size_t length, uint8_t value) {
  size_t loLength = length & NodeType::LengthMax;
  size_t hiLength = length >> bitsizeof<NodeType::LengthType>();
  return NodeType{ (typename NodeType::PrefixType)hiLength, (typename NodeType::LengthType)loLength, value };
}

template <class NodeType>
std::vector<NodeType> parseRuns(const std::vector<Run>& runs) {
  std::vector<NodeType> table;
  table.reserve(100);

  for(auto& run : runs) {
    //Ensure that prefix is short enough for PrefixType
    size_t                          gaps = run.prefix / NodeType::PrefixMax;
    typename NodeType::PrefixType prefix = run.prefix % NodeType::PrefixMax;
    if(gaps) {
      //zero-length node with non-zero value represents "advance tail by (prefix * value)"
      //~~@ convert from multiplier to high-bits
      while(gaps > std::numeric_limits<byte>::max()) {
        table.push_back({ NodeType::PrefixMax, 0, (byte)std::numeric_limits<byte>::max() });
        gaps -= std::numeric_limits<byte>::max();
      }
      table.push_back({ NodeType::PrefixMax, 0, (byte)gaps });
    }

    //Ensure that length is short enough for LengthType
    size_t length = run.length;
    if(length > NodeType::LengthMax) {
      constexpr size_t LONG_NODE_MAX_LENGTH = lengthFromLongNode(NodeType{ NodeType::PrefixMax, NodeType::LengthMax, 0 });

      //push maxxed-out "long nodes" until length is less than long node max
      while(length > LONG_NODE_MAX_LENGTH) {
        table.push_back({ prefix, 0, 0 });
        prefix = 0;
        table.push_back({ NodeType::PrefixMax, NodeType::LengthMax, run.value });
        length -= LONG_NODE_MAX_LENGTH;
      }
      //if remaining length is still too large for a standard node then push a long node
      if(length > NodeType::LengthMax) {
        table.push_back({ prefix, 0, 0 });
        prefix = 0;
        table.push_back(makeLongNode<NodeType>(length, run.value));
        length = 0; //in this case the long node will cover all remaining length
      }
    }
    //if length remains then push a standard node
    if(length) {
      table.push_back({ prefix, (typename NodeType::LengthType)length, run.value });
    }
  }

  return table;
}

template <class NodeType, class PackedNodeType>
int64_t checkTableBytesSaved(const std::vector<NodeType>& table) {
  size_t tableLength = table.size() * sizeof(PackedNodeType);
  size_t totalRunsLength = 0;
  for(auto& node : table) { totalRunsLength += node.length; }
  return totalRunsLength - tableLength;
}

template <class NodeType, class PackedNodeType>
std::vector<PackedNodeType> packTable(const std::vector<NodeType>& unpackedTable) {
  std::vector<PackedNodeType> packedTable;
  packedTable.reserve(unpackedTable.size());
  for(auto& node : unpackedTable) {
    packedTable.push_back({ node.prefix, node.length, node.value });
  }
  return packedTable;
}

template <class PackedNodeContainer>
std::vector<Run> unpackTable(const PackedNodeContainer& packedTable) {
  std::vector<Run> unpackedTable;
  unpackedTable.reserve(packedTable.size());
  for(auto& node : packedTable) {
    unpackedTable.push_back({ node.prefix, node.length, node.value });
  }
  return unpackedTable;
}

template <class NodeType, class PackedNodeType, class ContainerType>
void writeDeflatedFile(const ContainerType& data, const std::vector<NodeType>& table, const std::string& filename) {
  auto file = std::ofstream(filename, std::ios::binary); //~~@ use a map
  if(!file) { throw std::runtime_error("Could not open output file."); }
  
  size_t tsz = table.size();
  if(tsz > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("Table size too large. (Insanely large. What on earth are you deflating?");
  }
  Header header;
  header.magic[3] = 0x11; //~~_ determine best node type to use and indicate it here
  header.decompressedLength = (uint32_t)data.size();
  header.tableNodeCount = (uint32_t)tsz;
  file.write(reinterpret_cast<const char*>(&header), sizeof(header));
  auto packedTable = packTable<NodeType, PackedNodeType>(table);
  file.write(reinterpret_cast<const char*>(packedTable.data()), (std::streamsize)header.tableNodeCount * sizeof(std::remove_reference<decltype(packedTable)>::type::value_type));

  //begin writing deflated data
  const char* dataPtr = reinterpret_cast<const char*>(data.data());
  const char* const dataEndPtr = dataPtr + data.size();
  bool longNode = false;
  for(auto& node : table) {
    if(longNode) {
      dataPtr += lengthFromLongNode(node);
      longNode = false;
      continue;
    }

    size_t bytesToCopy = node.prefix;
    if(node.length == 0) {
      if(node.value == 0) {
        longNode = true;
      }
      else {
        bytesToCopy *= node.value; //~~@
      }
    }
    file.write(dataPtr, bytesToCopy);
    dataPtr += bytesToCopy;
    dataPtr += node.length;
  }
  file.write(dataPtr, dataEndPtr - dataPtr);
}

template <class NodeType, class PackedNodeType>
void inflateFile(const std::string& srcFilename, const std::string& dstFilename) {
  MappedFile srcMap(srcFilename, MappedFile::CreationDisposition::OPEN);
  if(srcMap.size() > std::numeric_limits<size_t>::max()) {
    //~~_ use rolling view to accomodate this instead of throwing
    throw std::runtime_error("File too large to unpack. (large file support not yet implemented)");
  }
  auto srcView = srcMap.getView(0, srcMap.size());
  auto srcIter = srcView.begin();

  Header* headerPtr = reinterpret_cast<Header*>(&srcView[0]);
  srcIter += sizeof(Header);
  headerPtr->checkMagic(); //~~_ return value is node type, which should be caught and used

  PackedNodeType* firstNodePtr = reinterpret_cast<PackedNodeType*>(&srcView[sizeof(Header)]);
  std::span<PackedNodeType> packedTable(firstNodePtr, headerPtr->tableNodeCount);
  auto table = unpackTable(packedTable);
  srcIter += sizeof(PackedNodeType) * headerPtr->tableNodeCount;

  MappedFile dstMap(dstFilename, MappedFile::CreationDisposition::CREATE, headerPtr->decompressedLength);
  auto dstView = dstMap.getView(0, dstMap.size());
  auto dstIter = dstView.begin();

  bool longNode = false;
  for(const auto& node : table) {
    if(longNode) {
      size_t longLength = lengthFromLongNode(node);
      auto tail = dstIter + longLength;
      std::fill(dstIter, tail, node.value);
      dstIter = tail;
      longNode = false;
      continue;
    }

    size_t bytesToCopy = node.prefix;
    if(node.length == 0) {
      if(node.value == 0) {
        longNode = true;
      }
      else {
        bytesToCopy *= node.value; //~~@ convert from multiplier to high-bits
      }
    }

    auto srcTail = srcIter + bytesToCopy;
    std::copy(srcIter, srcTail, dstIter);
    srcIter = srcTail;
    dstIter += bytesToCopy;

    auto dstTail = dstIter + node.length;
    std::fill(dstIter, dstTail, node.value);
    dstIter = dstTail;
  }

  std::copy(srcIter, srcView.end(), dstIter);
}

int main() {
  using RLENode = BaseRLENode<uint8_t, uint8_t>;
  using PackedRLENode = BasePackedRLENode<RLENode::PrefixType, RLENode::LengthType>;

  std::cout << "Node size: " << sizeof(RLENode) << "\n";
  std::cout << "Packed Node size: " << sizeof(PackedRLENode) << "\n\n";

  std::string testfile = "testfile.txt";
  std::string deflated = "deflated.bin";
  std::string inflated = "inflated.bin";

  std::filesystem::remove(deflated);
  std::filesystem::remove(inflated);

  MappedFile map(testfile, MappedFile::CreationDisposition::OPEN);
  auto data = map.getView(0, map.size());

  auto runs = collectRuns<PackedRLENode>(data);
  auto table = parseRuns<RLENode>(runs);
  writeDeflatedFile<RLENode, PackedRLENode>(data, table, deflated);
  inflateFile<RLENode, PackedRLENode>(deflated, inflated);

  MappedFile infMap(inflated, MappedFile::CreationDisposition::OPEN);
  auto infData = infMap.getView(0, infMap.size());
  std::cout << "Equality Test: " << (std::equal(infData.begin(), infData.end(), data.begin(), data.end()) ? "Pass" : "Fail") << "\n";
  MappedFile defMap(deflated, MappedFile::CreationDisposition::OPEN);
  auto compression = (float)((defMap.size() * 10000) / infMap.size()) / 100;
  std::cout << "Compressed Length Percentage: " << compression << "\n";
  auto bytesSaved = checkTableBytesSaved<RLENode, PackedRLENode>(table);
  std::cout << "Table bytes saved: " << bytesSaved << "\n";
  std::cout << std::endl;

  system("pause");

}
