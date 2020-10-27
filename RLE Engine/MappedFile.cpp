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

// class RAIIHandle
// Simple RAII wrapper for exception safety in MappedFile constructor
// Releases assigned handle on destruction unless commit() has been called, 
//   or handle value is NULL or INVALID_HANDLE_VALUE.
// Automatic conversion to and from HANDLE, so it can be used as a drop-in
//   solution.
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
  RAIIHandle hMap = CreateFileMappingA(hFile, NULL, PAGE_READWRITE, size.HighPart, size.LowPart, NULL);
  if(hMap == nullptr) { throwWindowsError(); }

  // We've got the goods, so go ahead and validate the temporaries into the members.
  file = hFile.commit();
  map = hMap.commit();

  // Since mapping with a length exceeding the file length will grow the file, fetch
  //   the filesize at this point to serve as the actual length of the file.
  GetFileSizeEx(file, &size);
  length = size.QuadPart;
}

MappedFile::~MappedFile() {
  CloseHandle(map);
  CloseHandle(file);
}

MappedFile::View MappedFile::getView(uint64_t offset, size_t viewLength) {
  // For some reason intellisense really hates this function, and commonly reports
  //   errors which do not actually exist.

  if(viewLength == 0) {
    throw std::runtime_error("MappedFile cannot generate a View object with a length of zero.");
  }

  SYSTEM_INFO sysInfo;
  GetSystemInfo(&sysInfo);
  auto& granularity = sysInfo.dwAllocationGranularity;
  size_t grains  = offset / granularity;
  size_t remains = offset % granularity;

  LARGE_INTEGER liOffset;
  liOffset.QuadPart = grains * granularity;
  void* ptr = MapViewOfFile(map, FILE_MAP_WRITE, liOffset.HighPart, liOffset.LowPart, viewLength);
  if(ptr == nullptr) { throwWindowsError(); }
  return View(reinterpret_cast<std::byte*>(ptr) + remains, viewLength);
}

MappedFile::View::View(std::byte* data, size_t length) :
  std::span<std::byte>(data, length), //note that this ctor executes first, regardless of list order
  ptr(data)
{
  //nop
}

MappedFile::View::~View() {
  FlushViewOfFile(ptr, 0);
  UnmapViewOfFile(ptr);
}
