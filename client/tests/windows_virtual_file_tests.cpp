#include "platform/windows/VirtualFileDrag.h"

#include <iostream>

int main() {
  std::wstring error;
  if (!dropspace::windows::selfTestVirtualFileProvider(&error)) {
    std::wcerr << error << L'\n';
    return 1;
  }
  std::cout << "windows_virtual_file_tests: passed\n";
  return 0;
}

