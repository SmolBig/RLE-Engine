#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <limits>

//~~@ use memory mapped files instead of file streams
//~~@ RLE(X) magic where X is node type (high nibble offset byte ct, low nibble length byte ct)
//~~_ get some better test files
//~~_ build a test harness

using byte = uint8_t;

#pragma pack(push, 1)
struct RLENode {
  using OffsetType = uint8_t;
  using LengthType = uint8_t;

  OffsetType offsetFromPreviousNode;
  LengthType length;
  byte value;
};
#pragma pack(pop)

constexpr size_t OffsetMax = std::numeric_limits<RLENode::OffsetType>::max();
constexpr size_t LengthMax = std::numeric_limits<RLENode::LengthType>::max();

template<class T>
constexpr size_t bitsizeof() {
  return sizeof(T) * 8;
}

struct Run {
  size_t position;
  size_t length;
  byte value;
};

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

    reuse = (run.length <= sizeof(RLENode));
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
  return lengthFromPair(node.offsetFromPreviousNode, node.length);
}

RLENode makeLongNode(size_t length, uint8_t value) {
  size_t loLength = length & LengthMax;
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
    size_t                gaps = runOffset / OffsetMax;
    RLENode::OffsetType offset = runOffset % OffsetMax;
    if(gaps) {
      //zero-length node with non-zero value represents "advance tail by (offset * value)"
      while(gaps > std::numeric_limits<byte>::max()) {
        table.push_back({ OffsetMax, 0, (byte)std::numeric_limits<byte>::max() });
        gaps -= std::numeric_limits<byte>::max();
      }
      table.push_back({ OffsetMax, 0, (byte)gaps });
    }

    size_t length = run.length;
    if(length > LengthMax) {
      constexpr size_t LONG_NODE_MAX_LENGTH = lengthFromPair(OffsetMax, LengthMax);

      while(length > LONG_NODE_MAX_LENGTH) {
        table.push_back({ offset, 0, 0 });
        offset = 0;
        table.push_back({ OffsetMax, LengthMax, run.value });
        length -= LONG_NODE_MAX_LENGTH;
      }
      if(length > LengthMax) {
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

int64_t checkEfficiency(const std::vector<RLENode>& table) {
  size_t tableLength = table.size() * sizeof(RLENode);
  size_t totalRunsLength = 0;
  for(auto& node : table) { totalRunsLength += node.length; }
  return totalRunsLength - tableLength;
}

void writeDeflatedFile(const std::vector<byte>& data, const std::vector<RLENode>& table, const std::string& filename) {
  auto file = std::ofstream(filename, std::ios::binary);
  if(!file) { throw std::runtime_error("Could not open output file."); }
  
  if(table.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("Table size too large. (Insanely large. What on earth are you deflating?");
  }
  uint32_t tableNodeCount = (uint32_t)table.size();
  file.write(reinterpret_cast<const char*>(&tableNodeCount), sizeof(tableNodeCount));
  file.write(reinterpret_cast<const char*>(table.data()), (std::streamsize)tableNodeCount * sizeof(std::remove_reference<decltype(table)>::type::value_type));

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

    size_t bytesToCopy = node.offsetFromPreviousNode;
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
  std::vector<RLENode> table(tableNodeCount);
  src.read(reinterpret_cast<char*>(table.data()), (std::streamsize)tableNodeCount * sizeof(decltype(table)::value_type));

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

    size_t bytesToCopy = node.offsetFromPreviousNode;
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
  auto data = loadFile("testfile.txt");
  auto runs = collectRuns(data);
  auto table = parseRuns(runs);
  
  writeDeflatedFile(data, table, "deflated.rle");
  inflateFile("deflated.rle", "inflated.txt");
}
