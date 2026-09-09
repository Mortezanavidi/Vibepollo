#pragma once
// Shared Windows clipboard primitives for the host and ARM64 client.
#include <WinSock2.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace clipboard_native {
  namespace fs = std::filesystem;
  constexpr std::uint64_t max_batch = 256ull * 1024 * 1024;
  constexpr std::size_t max_files = 64;
  constexpr std::size_t chunk_size = 128 * 1024;
  constexpr std::size_t max_text = 1024 * 1024;
  inline void require(bool ok) { if (!ok) throw std::runtime_error("Clipboard operation failed"); }
  inline std::wstring wide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    require(n > 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
  }
  inline std::string utf8(const std::wstring &s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    require(n > 0);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
  }
  inline bool safe_name(const std::string &s) {
    if (s.empty() || s.size() > 200 || s.back() == '.' || s.back() == ' ') return false;
    for (unsigned char c : s) if (c < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) return false;
    auto stem = s.substr(0, s.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return c >= 'a' && c <= 'z' ? c - 32 : c; });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$") return false;
    if (stem.size() >= 4 && (stem.substr(0, 3) == "COM" || stem.substr(0, 3) == "LPT")) return false;
    try { return !wide(s).empty(); } catch (...) { return false; }
  }
  struct clip_lock {
    HWND window = nullptr;
    explicit clip_lock(bool writing = false) {
      if (writing) { window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr); require(window != nullptr); }
      if (!OpenClipboard(window)) { if (window) DestroyWindow(window); throw std::runtime_error("Clipboard is busy"); }
    }
    ~clip_lock() { CloseClipboard(); if (window) DestroyWindow(window); }
    clip_lock(const clip_lock &) = delete;
    clip_lock &operator=(const clip_lock &) = delete;
  };
  struct snapshot {
    DWORD sequence = 0;
    std::string kind = "empty", text;
    std::vector<fs::path> files;
  };
  inline snapshot read() {
    clip_lock lock;
    snapshot out;
    out.sequence = GetClipboardSequenceNumber();
    if (IsClipboardFormatAvailable(CF_HDROP)) {
      auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
      require(drop != nullptr);
      UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
      require(count > 0 && count <= max_files);
      for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, nullptr, 0);
        require(length > 0 && length < 32767);
        std::wstring name(length + 1, L'\0');
        require(DragQueryFileW(drop, i, name.data(), length + 1) == length);
        name.resize(length);
        out.files.emplace_back(name);
      }
      out.kind = "files";
    } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
      auto data = GetClipboardData(CF_UNICODETEXT);
      require(data != nullptr);
      auto bytes = GlobalSize(data);
      require(bytes >= sizeof(wchar_t) && bytes <= max_text * sizeof(wchar_t));
      auto ptr = static_cast<const wchar_t *>(GlobalLock(data));
      require(ptr != nullptr);
      std::wstring value;
      try {
        auto end = std::find(ptr, ptr + bytes / sizeof(wchar_t), L'\0');
        require(end != ptr + bytes / sizeof(wchar_t));
        value.assign(ptr, end);
      } catch (...) { GlobalUnlock(data); throw; }
      GlobalUnlock(data);
      out.text = utf8(value);
      require(out.text.size() <= max_text);
      out.kind = "text";
    }
    return out;
  }
  inline DWORD publish(const snapshot &value, DWORD expected) {
    std::vector<char> bytes;
    UINT format;
    if (value.kind == "text") {
      require(value.text.size() <= max_text && value.text.find('\0') == std::string::npos);
      auto text = wide(value.text);
      bytes.resize((text.size() + 1) * sizeof(wchar_t));
      memcpy(bytes.data(), text.c_str(), bytes.size());
      format = CF_UNICODETEXT;
    } else {
      require(value.kind == "files" && !value.files.empty() && value.files.size() <= max_files);
      std::wstring names;
      for (const auto &p : value.files) { names += p.wstring(); names.push_back(L'\0'); }
      names.push_back(L'\0');
      bytes.resize(sizeof(DROPFILES) + names.size() * sizeof(wchar_t));
      DROPFILES drop {};
      drop.pFiles = sizeof(DROPFILES); drop.fWide = TRUE;
      memcpy(bytes.data(), &drop, sizeof(drop));
      memcpy(bytes.data() + sizeof(drop), names.data(), names.size() * sizeof(wchar_t));
      format = CF_HDROP;
    }
    auto mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    require(mem != nullptr);
    auto data = GlobalLock(mem);
    if (!data) { GlobalFree(mem); throw std::runtime_error("Clipboard allocation failed"); }
    memcpy(data, bytes.data(), bytes.size()); GlobalUnlock(mem);
    try {
      clip_lock lock(true);
      require(GetClipboardSequenceNumber() == expected);
      require(EmptyClipboard() != FALSE);
      require(SetClipboardData(format, mem) != nullptr);
      mem = nullptr;
    } catch (...) { if (mem) GlobalFree(mem); throw; }
    return GetClipboardSequenceNumber();
  }
  struct file {
    std::shared_ptr<void> handle;
    fs::path path;
    std::uint64_t size = 0, written = 0;
    static file open(const fs::path &path, bool create = false) {
      auto root = path.root_name().wstring();
      require(path.is_absolute() && root.size() == 2 && root[1] == L':');
      require(GetDriveTypeW(path.root_path().c_str()) != DRIVE_REMOTE);
      auto h = CreateFileW(path.c_str(), create ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                          FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      require(h != INVALID_HANDLE_VALUE);
      file out;
      out.handle = std::shared_ptr<void>(h, [](void *v) { CloseHandle(v); });
      out.path = path;
      BY_HANDLE_FILE_INFORMATION info {};
      require(GetFileInformationByHandle(h, &info) != FALSE);
      require(!(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)));
      out.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
      require(out.size <= max_batch);
      return out;
    }
    std::string read(std::uint64_t offset) {
      require(offset <= size);
      LARGE_INTEGER pos {}; pos.QuadPart = static_cast<LONGLONG>(offset);
      require(SetFilePointerEx(handle.get(), pos, nullptr, FILE_BEGIN) != FALSE);
      std::string bytes(static_cast<std::size_t>((std::min)(size - offset, static_cast<std::uint64_t>(chunk_size))), '\0');
      DWORD count = 0;
      require(ReadFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) != FALSE && count == bytes.size());
      return bytes;
    }
    void append(std::uint64_t offset, const std::string &bytes) {
      require(offset == written && !bytes.empty() && bytes.size() <= chunk_size && written <= size && bytes.size() <= size - written);
      DWORD count = 0;
      require(WriteFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) != FALSE && count == bytes.size());
      written += count;
    }
  };
  inline std::string hex(const std::string &bytes) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string out; out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) { out.push_back(alphabet[c >> 4]); out.push_back(alphabet[c & 15]); }
    return out;
  }
  inline std::string unhex(const std::string &text) {
    require(text.size() % 2 == 0 && text.size() <= chunk_size * 2);
    auto digit = [](char c) -> unsigned char { require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')); return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 2) out.push_back(static_cast<char>((digit(text[i]) << 4) | digit(text[i + 1])));
    return out;
  }
}
