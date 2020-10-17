#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <limits>

//~~@ use memory mapped files instead of file streams
//~~@ templatize node-accepting functions
//~~@ RLE(X) header value, where X is node type (high nibble offset byte ct, low nibble length byte ct)
//~~_ get some better test files
//~~_ build a test harness
//~~@ modify struct Run to use span instead of position
//~~~ check compiler implementations to ensure that packed is behaving properly and that vector is treating it right

using byte = uint8_t;

template <typename OffsetT, typename LengthT>
struct BaseRLENode {
  using OffsetType = OffsetT;
  static constexpr size_t OffsetMax = std::numeric_limits<OffsetType>::max();
  using LengthType = LengthT;
  static constexpr size_t LengthMax = std::numeric_limits<LengthType>::max();

  OffsetType prefix; //offset from previous node's tail
  LengthType length;
  byte value;
};

using RLENode = BaseRLENode<uint32_t, uint32_t>;

#pragma pack(push, 1)
template <typename OffsetT, typename LengthT>
struct BasePackedRLENode {
  OffsetT prefix;
  LengthT length;
  byte value;
};
#pragma pack(pop)

using PackedRLENode = BasePackedRLENode<RLENode::OffsetType, RLENode::LengthType>;

struct Run {
  size_t position;
  size_t length;
  byte value;
};

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

std::vector<Run> collectRuns(const std::vector<byte>& data) {
  std::vector<Run> runs;
  runs.reserve(100);

  bool reuse = false;
  for(size_t i = 0; i < data.size(); ) {
    if(!reuse) {
      runs.emplace_back();
    }

    auto& run = runs.back();
    run.position = i;
    run.length = 1;
    run.value = data[i];

    while((++i < data.size()) && (data[i] == run.value)) {
      run.length++;
    }

    reuse = (run.length <= sizeof(PackedRLENode));
  }

  if(reuse) {
    runs.pop_back();
  }

  return runs;
}

constexpr size_t lengthFromPair(RLENode::OffsetType offset, RLENode::LengthType length) {
  size_t retval = offset;
  retval <<= bitsizeof<RLENode::LengthType>();
  retval += length;
  return retval;
}

size_t lengthFromLongNode(const RLENode& node) {
  return lengthFromPair(node.prefix, node.length);
}

RLENode makeLongNode(size_t length, uint8_t value) {
  size_t loLength = length & RLENode::LengthMax;
  size_t hiLength = length >> bitsizeof<RLENode::LengthType>();
  return RLENode{ (RLENode::OffsetType)hiLength, (RLENode::LengthType)loLength, value };
}

std::vector<RLENode> parseRuns(const std::vector<Run>& runs) {
  std::vector<RLENode> table;
  table.reserve(100);

  size_t previousNodeTail = 0;
  for(auto& run : runs) {
    //Ensure that offset from previous tail is short enough for OffsetType
    size_t runOffset = run.position - previousNodeTail;
    size_t                gaps = runOffset / RLENode::OffsetMax;
    RLENode::OffsetType offset = runOffset % RLENode::OffsetMax;
    if(gaps) {
      //zero-length node with non-zero value represents "advance tail by (offset * value)"
      while(gaps > std::numeric_limits<byte>::max()) {
        table.push_back({ RLENode::OffsetMax, 0, (byte)std::numeric_limits<byte>::max() });
        gaps -= std::numeric_limits<byte>::max();
      }
      table.push_back({ RLENode::OffsetMax, 0, (byte)gaps });
    }

    size_t length = run.length;
    if(length > RLENode::LengthMax) {
      constexpr size_t LONG_NODE_MAX_LENGTH = lengthFromPair(RLENode::OffsetMax, RLENode::LengthMax);

      while(length > LONG_NODE_MAX_LENGTH) {
        table.push_back({ offset, 0, 0 });
        offset = 0;
        table.push_back({ RLENode::OffsetMax, RLENode::LengthMax, run.value });
        length -= LONG_NODE_MAX_LENGTH;
      }
      if(length > RLENode::LengthMax) {
        table.push_back({ offset, 0, 0 });
        offset = 0;
        table.push_back(makeLongNode(length, run.value));
        length = 0;
      }
    }
    if(length) {
      table.push_back({ offset, (RLENode::LengthType)length, run.value });
    }

    previousNodeTail = run.position + run.length;
  }

  return table;
}

int64_t checkTableBytesSaved(const std::vector<RLENode>& table) {
  size_t tableLength = table.size() * sizeof(PackedRLENode);
  size_t totalRunsLength = 0;
  for(auto& node : table) { totalRunsLength += node.length; }
  return totalRunsLength - tableLength;
}

std::vector<PackedRLENode> packTable(const std::vector<RLENode>& unpackedTable) {
  std::vector<PackedRLENode> packedTable;
  packedTable.reserve(unpackedTable.size());
  for(auto& node : unpackedTable) {
    packedTable.push_back({ node.prefix, node.length, node.value });
  }
  return packedTable;
}

std::vector<RLENode> unpackTable(const std::vector<PackedRLENode>& packedTable) {
  std::vector<RLENode> unpackedTable;
  unpackedTable.reserve(packedTable.size());
  for(auto& node : packedTable) {
    unpackedTable.push_back({ node.prefix, node.length, node.value });
  }
  return unpackedTable;
}

void writeDeflatedFile(const std::vector<byte>& data, const std::vector<RLENode>& table, const std::string& filename) {
  auto file = std::ofstream(filename, std::ios::binary);
  if(!file) { throw std::runtime_error("Could not open output file."); }
  
  size_t tsz = table.size();
  if(tsz > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("Table size too large. (Insanely large. What on earth are you deflating?");
  }
  uint32_t tableNodeCount = (uint32_t)tsz;
  file.write(reinterpret_cast<const char*>(&tableNodeCount), sizeof(tableNodeCount));
  auto packedTable = packTable(table);
  file.write(reinterpret_cast<const char*>(packedTable.data()), (std::streamsize)tableNodeCount * sizeof(std::remove_reference<decltype(table)>::type::value_type));

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
  std::vector<PackedRLENode> packedTable(tableNodeCount);
  src.read(reinterpret_cast<char*>(packedTable.data()), (std::streamsize)tableNodeCount * sizeof(decltype(packedTable)::value_type));
  auto table = unpackTable(packedTable);

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
  std::cout << sizeof(RLENode) << "\n";
  std::cout << sizeof(PackedRLENode) << "\n";
  system("pause");

#ifdef HERPDERP
  auto data = loadFile("testfile.txt");
  auto runs = collectRuns(data);
  auto table = parseRuns(runs);
  
  writeDeflatedFile(data, table, "deflated.rle");
  inflateFile("deflated.rle", "inflated.txt");
#endif
}
