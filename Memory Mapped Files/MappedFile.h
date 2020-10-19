#pragma once
#include <span>
#include <string>

//~~#
class MappedFile {
  using byte = unsigned char;

public:
  enum class CreationDisposition {
    OPEN, //~~#
    CREATE //~~#
  };

  //~~#
  MappedFile(const std::string& filename, CreationDisposition disposition, uint64_t desiredLength = 0);
  ~MappedFile();

  //~~#
  uint64_t size() const { return length; }

  class View; //forward declaration

  //~~#
  View getView(uint64_t offset, size_t length);

  //~~#
  class View : public std::span<byte> {
    friend View MappedFile::getView(uint64_t offset, size_t length);
  public:
    ~View();

  private:
    View(byte*, size_t);
    byte* ptr;
  };

private:
  void* file;
  void* map;
  uint64_t length;

};
