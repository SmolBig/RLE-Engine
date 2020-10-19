#include "MappedFile.h"
#include <iostream>

int main() {
  MappedFile map("testfile.txt", MappedFile::CreationDisposition::OPEN);
  auto view = map.getView(0, (size_t)map.size());
  for(auto c : view) {
    std::cout << c;
  }
  std::cout << "\n\n";
  system("pause");
}
