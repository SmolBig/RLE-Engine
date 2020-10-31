#pragma once
#include "RLE_Shared.h"
#include <unordered_map>
#include <vector>
#include <future>

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
std::vector<NodeType> parseRunSet(const std::span<const Run>& runs) {
  std::vector<NodeType> nodes;
  nodes.reserve(runs.size());
  for(auto& run : runs) {
    parseRun(run, nodes);
  }
  return nodes;
}

template <class NodeType>
RLETable generateRLETable(NodeFormat format, int64_t efficiency, const std::vector<Run>& runs) {
  size_t threadCount = 4; //~~@
  size_t runsDist = runs.size() / threadCount;

  std::vector<std::span<const Run>> runBlocks;
  runBlocks.reserve(threadCount);
  auto runsIter = runs.begin();
  //note that loop starts at 1 instead of zero, so that one block is not handled by the loop
  for(int i = 1; i < threadCount; i++) {
    auto tail = runsIter + runsDist;
    runBlocks.emplace_back(runsIter, tail);
    runsIter = tail;
  }
  runBlocks.emplace_back(runsIter, runs.end());

  std::vector<std::future<std::vector<NodeType>>> futures;
  auto policy = std::launch::async;
  for(auto& block : runBlocks) {
    futures.push_back(std::async(policy, parseRunSet<NodeType>, block));
  }

  std::vector<NodeType> nodes;
  nodes.reserve(runs.size());
  for(auto& fut : futures) {
    auto block = fut.get();
    nodes.insert(nodes.end(), block.begin(), block.end());
  }
  return RLETable(format, efficiency, nodes);
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

std::vector<Run> collectRuns(const std::span<const std::byte>& data) { //~~@ thread this
  std::vector<Run> runs;
  runs.reserve(data.size() >> 10);

  Run run;
  size_t prevTailPos = 0;
  for(size_t i = 0; i < data.size(); ) {
    auto position = i;
    run.length = 1;
    run.value = data[i];

    while((++i < data.size()) && (data[i] == run.value)) {
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
  case NodeFormat::INEFFICIENT: throw std::runtime_error("Cannot deflate this file efficiently.");
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
