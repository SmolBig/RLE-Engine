#define NOMINMAX
#include <Windows.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <iostream>
#include <span>

//https://docs.microsoft.com/en-us/windows/win32/memory/creating-a-file-mapping-object

template<class T>
constexpr size_t bitsizeof(T = T{}) {
  constexpr size_t BITS_PER_BYTE = 8;
  return sizeof(T) * BITS_PER_BYTE;
}

void throwWindowsError() {
  LPSTR errMsg = NULL;
  DWORD sult = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPSTR)&errMsg, 0, NULL);
  throw std::runtime_error(errMsg);
  LocalFree(errMsg);
}

class WinFile {
public:
  WinFile(const std::string& fileName, DWORD access) {
    DWORD creationDisposition = access == GENERIC_READ ? OPEN_EXISTING : CREATE_NEW;
    handle = CreateFileA(fileName.c_str(), access, 0, NULL, creationDisposition, 0, NULL);
    if(handle == INVALID_HANDLE_VALUE) { throwWindowsError(); }
  }

  ~WinFile() {
    CloseHandle(handle);
  }

  operator HANDLE() {
    return handle;
  }

private:
  HANDLE handle;

};

class WinFileMap {
public:
  WinFileMap(HANDLE hFile, DWORD access, size_t desiredLength = 0) {
#ifdef _M_X64
    DWORD sizeHi = (DWORD)(desiredLength >> bitsizeof<DWORD>());
#else
    DWORD sizeHi = 0;
#endif
    DWORD sizeLo = desiredLength & std::numeric_limits<DWORD>::max();

    handle = CreateFileMappingA(hFile, NULL, access, sizeHi, sizeLo, NULL);
    if(handle == INVALID_HANDLE_VALUE) { throwWindowsError(); }
  }

  ~WinFileMap() {
    CloseHandle(handle);
  }

  operator HANDLE() {
    return handle;
  }

private:
  HANDLE handle;

};

class WinFileMapView {
public:
  WinFileMapView(HANDLE hMap, DWORD access) {
    ptr = reinterpret_cast<byte*>(MapViewOfFile(hMap, access, 0, 0, 0));
  }

  byte* data() {
    return ptr;
  }

  const byte* data() const {
    return ptr;
  }

  ~WinFileMapView() {
    UnmapViewOfFile(ptr);
  }

private:
  byte* ptr;

};

class FileMap {
  friend const FileMap openFileAsMap(const std::string&);
  friend FileMap createFileAsMap(const std::string&, size_t length);
public:
  enum class AccessType {
    READONLY,
    READWRITE
  };

  AccessType getAccessType() const {
    return access;
  }

  std::span<byte> getSpan() {
    return std::span<byte>(view.data(), length);
  }

  std::span<const byte> getSpan() const {
    return std::span<const byte>(view.data(), length);
  }

private:
  FileMap(const std::string& filename, AccessType access, size_t desiredLength = 0) :
    file(filename, GENERIC_READ | (access == AccessType::READWRITE ? GENERIC_WRITE : 0)),
    map(file, access == AccessType::READWRITE ? PAGE_READWRITE : PAGE_READONLY, desiredLength),
    view(map, access == AccessType::READWRITE ? FILE_MAP_WRITE : FILE_MAP_READ),
    access(access)
  {
    DWORD sizeHi = 0;
    length = GetFileSize(file, &sizeHi);
#ifdef _M_X64
    length |= (size_t)sizeHi << bitsizeof<DWORD>();
#endif
  }

  WinFile file;
  WinFileMap map;
  WinFileMapView view;
  AccessType access;
  size_t length;

};

const FileMap openFileAsMap(const std::string& filename) {
  return FileMap(filename, FileMap::AccessType::READONLY);
}

FileMap createFileAsMap(const std::string& filename, size_t length) {
  return FileMap(filename, FileMap::AccessType::READWRITE, length);
}

int main() {
  FileMap inmap = openFileAsMap("testfile.txt");
  auto inspan = inmap.getSpan();

  FileMap outmap = createFileAsMap("result.txt", inspan.size());
  auto outspan = outmap.getSpan();

  std::copy(inspan.begin(), inspan.end(), outspan.begin());

}
