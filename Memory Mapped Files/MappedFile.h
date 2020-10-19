#pragma once
#include <span>
#include <string>

class MappedFile {
public:
  enum class CreationDisposition {
    OPEN,
    CREATE
  };

  MappedFile(const std::string& filename, CreationDisposition disposition, uint64_t desiredLength = 0);
  ~MappedFile();

  uint64_t size() const { return length; }

  class View;
  View getView(uint64_t offset, size_t length);

  class View : public std::span<char> {
  public:
    View(char*, size_t);
    ~View();

  private:
    char* ptr;
  };

private:
  void* file;
  void* map;
  uint64_t length;

};
