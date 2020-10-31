#include "RLE_Inflate.h"
#include "RLE_Deflate.h"
#include <filesystem>
#include <iostream>

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

void primaryTest(const std::string& testfile) {
  std::string deflated = testfile + ".rle";
  std::string inflated = testfile + ".reinflated";

  std::cout << "Purging existing output files.\n";
  std::filesystem::remove(deflated);
  std::filesystem::remove(inflated);

  std::cout << "Deflating...";
  deflateFile(testfile, deflated);
  std::cout << "\nRe-inflating...";
  inflateFile(deflated, inflated);

  std::cout << "\nDone.\n";
  MappedFile testMap(testfile, MappedFile::CreationDisposition::OPEN);
  MappedFile reinfMap(inflated, MappedFile::CreationDisposition::OPEN);
  auto deflatedSize = std::filesystem::file_size(std::filesystem::path(deflated));
  auto compression = (float)((deflatedSize * 10000) / testMap.size()) / 100;
  std::cout << "\nCompressed Length Percentage: " << compression << "\n";
  auto infData = testMap.getView(0, testMap.size());
  auto defData = reinfMap.getView(0, reinfMap.size());
  std::cout << "Testing Equality: " << (std::equal(infData.begin(), infData.end(), defData.begin(), defData.end()) ? "Pass" : "Fail") << "\n";
  std::cout << std::endl;

  system("pause");

}

void efficiencyCalcTest(const std::string& testfile) {
  std::vector<Run> runs;
  {
    MappedFile inMap(testfile, MappedFile::CreationDisposition::OPEN);
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

void deflate(int argc, char** argv) {
  if(argc != 2) { throw std::runtime_error("Usage: deflate [name of file to create deflated copy of]"); }

  std::string sourceFileName(argv[1]);
  std::string deflatedFileName = sourceFileName + ".rle";
  std::cout << "RLE deflating file. Please wait...";
  deflateFile(sourceFileName, deflatedFileName);
  std::cout << "\nFinished.\n\n";
  auto originalSize = std::filesystem::file_size(std::filesystem::path(sourceFileName));
  auto deflatedSize = std::filesystem::file_size(std::filesystem::path(deflatedFileName));
  auto compression = (float)((deflatedSize * 10000) / originalSize) / 100;
  std::cout << "Compression Percentage: " << compression << "\n";
}

void inflate(int argc, char** argv) {
  if(argc != 2) { throw std::runtime_error("Usage: inflate [name of file to create reinflated copy of]"); }

  std::string rleFileName(argv[1]);
  std::string restoredFileName = rleFileName;
  restoredFileName.resize(restoredFileName.size() - (sizeof(".rle") - 1));
  std::cout << "Inflating RLE file. Please wait...";
  inflateFile(rleFileName, restoredFileName);
  std::cout << "\nFinished.\n\n";
}

int main(/*int argc, char** argv*/) {
  primaryTest("testfile.txt");
  return 0;

//#define BUILD_DEFLATE
  /*
  try {
#if defined BUILD_DEFLATE
    deflate(argc, argv);
#else
    inflate(argc, argv);
#endif
  }
  catch(const std::exception& e) {
    std::cout << e.what() << "\n\n";
    system("pause");
  }
  */
}








