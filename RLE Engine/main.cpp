#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <limits>

//~~@ use memory mapped files instead of file streams
//~~_ RLE(X) header value, where X is node type (high nibble prefix byte ct, low nibble length byte ct)
//~~_ modify inflate routine to use the header for determining node type
//~~_ get a nice set of various test files going
//~~_ implement system to determine most efficient node type based on runs vector
//~~_ modify deflate routine to select and use most efficient node type
//~~~ check compiler implementations to ensure that pack pragma behaves properly and that vector is treating it right (no filler between elements, etc)

using byte = uint8_t;

template <typename PrefixT, typename LengthT>
struct BaseRLENode {
  using PrefixType = PrefixT;
  static constexpr size_t PrefixMax = std::numeric_limits<PrefixType>::max();
  using LengthType = LengthT;
  static constexpr size_t LengthMax = std::numeric_limits<LengthType>::max();

  PrefixType prefix; //offset from previous node's tail
  LengthType length;
  byte value;
};

#pragma pack(push, 1)
template <typename PrefixT, typename LengthT>
struct BasePackedRLENode {
  PrefixT prefix;
  LengthT length;
  byte value;
};
#pragma pack(pop)

using Run = BaseRLENode<size_t, size_t>;

template<class T>
constexpr size_t bitsizeof() {
  return sizeof(T) * 8;
}

std::vector<byte> loadFile(const std::string& filename) {
  std::vector<byte> data;
  std::ifstream file(filename, std::ios::binary);
  if(!file) { throw std::runtime_error("Could not open input file."); }
  file.seekg(0, std::ios::end);
  data.resize((size_t)file.tellg());
  file.seekg(0, std::ios::beg);
  file.read(reinterpret_cast<char*>(data.data()), data.size());
  return data;
}

template <class PackedNodeType>
std::vector<Run> collectRuns(const std::vector<byte>& data) {
  std::vector<Run> runs;
  runs.reserve(100);

  Run run;
  size_t prevTailPos = 0;
  for(size_t i = 0; i < data.size(); ) {
    auto position = i;
    run.length = 1;
    run.value = data[i];

    while((++i < data.size()) && (data[i] == run.value)) {
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

template <class NodeType, class PackedNodeType>
std::vector<NodeType> unpackTable(const std::vector<PackedNodeType>& packedTable) {
  std::vector<NodeType> unpackedTable;
  unpackedTable.reserve(packedTable.size());
  for(auto& node : packedTable) {
    unpackedTable.push_back({ node.prefix, node.length, node.value });
  }
  return unpackedTable;
}

template <class NodeType, class PackedNodeType>
void writeDeflatedFile(const std::vector<byte>& data, const std::vector<NodeType>& table, const std::string& filename) {
  auto file = std::ofstream(filename, std::ios::binary);
  if(!file) { throw std::runtime_error("Could not open output file."); }
  
  size_t tsz = table.size();
  if(tsz > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("Table size too large. (Insanely large. What on earth are you deflating?");
  }
  uint32_t tableNodeCount = (uint32_t)tsz;
  file.write(reinterpret_cast<const char*>(&tableNodeCount), sizeof(tableNodeCount));
  auto packedTable = packTable<NodeType, PackedNodeType>(table);
  file.write(reinterpret_cast<const char*>(packedTable.data()), (std::streamsize)tableNodeCount * sizeof(std::remove_reference<decltype(packedTable)>::type::value_type));

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
        bytesToCopy *= node.value;
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
  //open both files
  auto src = std::ifstream(srcFilename, std::ios::binary);
  if(!src) { throw std::runtime_error("Could not open source file."); }
  auto dst = std::ofstream(dstFilename, std::ios::binary);
  if(!dst) { throw std::runtime_error("Could not open destination file."); }

  //determine length of source file
  src.seekg(0, std::ios::end);
  size_t srcLength = (size_t)src.tellg();
  src.seekg(0, std::ios::beg);

  //read in RLE table
  uint32_t tableNodeCount;
  src.read(reinterpret_cast<char*>(&tableNodeCount), sizeof(tableNodeCount));
  if(!src.good()) { throw "derp"; }
  std::vector<PackedNodeType> packedTable(tableNodeCount);
  src.read(reinterpret_cast<char*>(packedTable.data()), (std::streamsize)tableNodeCount * sizeof(decltype(packedTable)::value_type));
  auto table = unpackTable<NodeType, PackedNodeType>(packedTable);

  //process nodes
  bool longNode = false;
  for(const auto& node : table) {
    if(longNode) {
      size_t longLength = lengthFromLongNode(node);
      for(size_t i = 0; i < longLength; i++) {
        dst.write(reinterpret_cast<const char*>(&node.value), 1);
      }
      longNode = false;
      continue;
    }

    size_t bytesToCopy = node.prefix;
    if(node.length == 0) {
      if(node.value == 0) {
        longNode = true;
      }
      else {
        bytesToCopy *= node.value;
      }
    }

    std::vector<char> buffer(bytesToCopy);
    src.read(buffer.data(), buffer.size());
    dst.write(buffer.data(), buffer.size());

    for(size_t i = 0; i < node.length; i++) {
      dst.write(reinterpret_cast<const char*>(&node.value), 1);
    }
  }

  //copy remaining bytes
  std::vector<char> buffer(srcLength - (size_t)src.tellg());
  src.read(buffer.data(), buffer.size());
  dst.write(buffer.data(), buffer.size());
}

int main() {
  using RLENode = BaseRLENode<uint8_t, uint8_t>;
  using PackedRLENode = BasePackedRLENode<RLENode::PrefixType, RLENode::LengthType>;

  std::cout << "Node size: " << sizeof(RLENode) << "\n";
  std::cout << "Packed Node size: " << sizeof(PackedRLENode) << "\n\n";

  std::string testfile = "testfile.txt";
  std::string deflated = "deflated.bin";
  std::string inflated = "inflated.bin";

  auto data = loadFile(testfile);
  auto runs = collectRuns<PackedRLENode>(data);
  auto table = parseRuns<RLENode>(runs);
  writeDeflatedFile<RLENode, PackedRLENode>(data, table, deflated);
  inflateFile<RLENode, PackedRLENode>(deflated, inflated);

  auto infData = loadFile(inflated);
  std::cout << "Equality Test: " << (infData == data ? "Pass" : "Fail") << "\n";
  auto defData = loadFile(deflated);
  auto compression = (float)((defData.size() * 10000) / infData.size()) / 100;
  std::cout << "Compressed Length Percentage: " << compression << "\n";
  auto bytesSaved = checkTableBytesSaved<RLENode, PackedRLENode>(table);
  std::cout << "Table bytes saved: " << bytesSaved << "\n";
  std::cout << std::endl;
  system("pause");
}
