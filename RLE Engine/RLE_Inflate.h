#pragma once
#include "RLE_Shared.h"
#include <vector>

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

  //~~@ thread this
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

