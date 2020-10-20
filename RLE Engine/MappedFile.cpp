#include "MappedFile.h"
#define NOMINMAX
#include <Windows.h>
#include <stdexcept>

// Simple utility function which throws a std::runtime_error with the error message generated from WinAPI.
void throwWindowsError() {
  LPSTR buffer = NULL;
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPSTR)&buffer, 0, NULL);
  std::string msg(buffer);
  LocalFree(buffer);
  throw std::runtime_error(msg);
}

// Simple RAII wrapper for exception safety in MappedFile constructor
//
// Releases assigned handle on destruction unless commit() has been called, 
//   or handle value is NULL or INVALID_HANDLE_VALUE.
class RAIIHandle {
public:
  RAIIHandle(HANDLE h) { handle = h; }
  void operator=(HANDLE h) { handle = h; }

  operator HANDLE() { return handle; }

  HANDLE commit() {
    releaseOnDestruct = false;
    return handle;
  }

  ~RAIIHandle() {
    if(releaseOnDestruct && (handle != NULL) && (handle != INVALID_HANDLE_VALUE)) {
      CloseHandle(handle); 
    }
  }

private:
  bool releaseOnDestruct = true;
  HANDLE handle = NULL;

};

MappedFile::MappedFile(const std::string& filename, CreationDisposition disposition, uint64_t desiredLength) {
  // Translate from enum to CreateFile argument and ensure that newly created files have non-zero length.
  DWORD disp = OPEN_EXISTING;
  if(disposition == CreationDisposition::CREATE) {
    disp = CREATE_NEW;
    if(desiredLength == 0) {
      throw std::runtime_error("Forgot to provide desired length when creating a file for mapping.");
    }
  }

  // Generate file
  RAIIHandle hFile = CreateFileA(filename.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, disp, 0, NULL);
  if(hFile == INVALID_HANDLE_VALUE) { throwWindowsError(); }

  // Map file
  LARGE_INTEGER size;
  size.QuadPart = desiredLength;
  RAIIHandle hMap = CreateFileMappingA(file, NULL, PAGE_READWRITE, size.HighPart, size.LowPart, NULL);
  if(hMap == nullptr) { throwWindowsError(); }

  // Since mapping with a length exceeding the file length will grow the file, fetch
  //   the filesize at this point to serve as the actual length of the file.
  GetFileSizeEx(file, &size);
  length = size.QuadPart;
  file = hFile.commit();
  map = hMap.commit();
}

MappedFile::~MappedFile() {
  CloseHandle(map);
  CloseHandle(file);
}

MappedFile::View MappedFile::getView(uint64_t offset, size_t viewLength) {
  // For some reason intellisense really hates this function, and commonly reports
  //   errors which do not actually exist.
  LARGE_INTEGER liOffset;
  liOffset.QuadPart = offset;
  void* ptr = MapViewOfFile(map, FILE_MAP_WRITE, liOffset.HighPart, liOffset.LowPart, viewLength);
  if(ptr == nullptr) { throwWindowsError(); }
  return View(reinterpret_cast<byte*>(ptr), viewLength);
}

MappedFile::View::View(byte* data, size_t length) :
  std::span<byte>(data, length), //note that this ctor executes first, regardless of list order
  ptr(data)
{
  //nop
}

MappedFile::View::~View() {
  FlushViewOfFile(ptr, 0);
  UnmapViewOfFile(ptr);
}
