#pragma once
#include <span>
#include <string>

//~~! modify getView to take allocation granularity into account (the span doesn't have to cover the whole view)

/// class MappedFile
/// Opens a file and memory maps it using the Win32 API.
/// Provides an interface for generating views for the mapped file.
/// This class can be used to open or create files which can be read from
///   and written to as if they were sections of main memory.
class MappedFile {
public:
  enum class CreationDisposition {
    OPEN, // Will open an existing file or throw a std::runtime_error if the file is not found.
    CREATE // Will create a new file or throw a std::runtime_error if the file already exists.
    //~~_ add an option for "open or create"?
  };

  // MappedFile constructor
  // When disposition is OPEN, length is ignored.
  // When disposition is CREATE, length must be non-zero.
  // Map length can not be adjusted after creation.
  MappedFile(const std::string& filename, CreationDisposition disposition, uint64_t desiredLength = 0);
  ~MappedFile();

  // Returns length (in bytes) of file/map.
  uint64_t size() const { return length; }

  class View; // forward declaration

  // Returns a View object with the indicated offset and length.
  // Length may not be zero. Use MappedFile::size() to get map length.
  View getView(uint64_t offset, size_t length);

  // class MappedFile::View
  // Objects of this type must be instantiated using the MappedFile::getView() function.
  // The View object inherits from std::span<byte>. The only additional behavior is a
  //   destructor which flushes and releases the underlying WinAPI view resource.
  // Note that views are invalidated when the MappedFile object which created them is
  //   destructed. View behavior beyond that point is undefined, but will probably
  //   (hopefully) result in a segfault.
  class View : public std::span<std::byte> {
    friend View MappedFile::getView(uint64_t offset, size_t length);
  public:
    ~View();

  private:
    View(std::byte*, size_t);
    std::byte* ptr;

  };

private:
  void* file;
  void* map;
  uint64_t length;

};
