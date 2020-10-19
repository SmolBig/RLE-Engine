#include "MappedFile.h"
#define NOMINMAX
#include <Windows.h>
#include <stdexcept>

//~~#
void throwWindowsError() {
  LPSTR buffer = NULL;
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPSTR)&buffer, 0, NULL);
  std::string msg(buffer);
  LocalFree(buffer);
  throw std::runtime_error(msg);
}

//~~#
MappedFile::MappedFile(const std::string& filename, CreationDisposition disposition, uint64_t desiredLength) {
  DWORD disp = OPEN_EXISTING;
  if(disposition == CreationDisposition::CREATE) {
    disp = CREATE_NEW;
    if(desiredLength == 0) {
      throw std::runtime_error("Forgot to provide desired length when creating a file for mapping.");
    }
  }

  file = CreateFileA(filename.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, disp, 0, NULL);
  if(file == INVALID_HANDLE_VALUE) { throwWindowsError(); }

  LARGE_INTEGER size;
  size.QuadPart = desiredLength;
  map = CreateFileMappingA(file, NULL, PAGE_READWRITE, size.HighPart, size.LowPart, NULL);
  if(map == nullptr) { throwWindowsError(); }

  GetFileSizeEx(file, &size);
  length = size.QuadPart;
}

//~~#
MappedFile::~MappedFile() {
  CloseHandle(map);
  CloseHandle(file);
}

//~~#
MappedFile::View MappedFile::getView(uint64_t offset, size_t viewLength) {
  LARGE_INTEGER liOffset;
  liOffset.QuadPart = offset;
  void* ptr = MapViewOfFile(map, FILE_MAP_WRITE, liOffset.HighPart, liOffset.LowPart, viewLength);
  if(ptr == nullptr) { throwWindowsError(); }
  return View(reinterpret_cast<byte*>(ptr), viewLength);
}

//~~#
MappedFile::View::View(byte* data, size_t length) :
  ptr(data),
  std::span<byte>(data, length)
{
  //nop
}

//~~#
MappedFile::View::~View() {
  FlushViewOfFile(ptr, 0);
  UnmapViewOfFile(ptr);
}
