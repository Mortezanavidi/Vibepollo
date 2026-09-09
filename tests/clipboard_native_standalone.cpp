#include "src/platform/windows/clipboard_native.h"
#include <iostream>

int main() {
  namespace clip = clipboard_native;
  try {
    for (const auto &name : {"..", "../escape", "C:escape", "file:stream", "CON", "nul.txt", "COM1.log", "LPT9", "trailing.", "trailing ", "a\\b"})
      clip::require(!clip::safe_name(name));
    for (const auto &name : {"notes.txt", "photo 1.png", "README"}) clip::require(clip::safe_name(name));
    std::string data;
    for (int i = 0; i < 256; ++i) data.push_back(static_cast<char>(i));
    clip::require(clip::unhex(clip::hex(data)) == data);
    bool rejected = false;
    try { clip::unhex("xx"); } catch (...) { rejected = true; }
    clip::require(rejected);
    auto directory = clip::fs::temp_directory_path() / ("clipboard-test-" + std::to_string(GetCurrentProcessId()));
    clip::require(clip::fs::create_directory(directory));
    auto path = directory / "data.bin";
    {
      auto file = clip::file::open(path, true);
      file.size = data.size();
      rejected = false;
      try { file.append(1, data); } catch (...) { rejected = true; }
      clip::require(rejected);
      file.append(0, data);
      rejected = false;
      try { file.append(data.size(), "x"); } catch (...) { rejected = true; }
      clip::require(rejected);
    }
    {
      auto file = clip::file::open(path);
      clip::require(file.read(0) == data && file.read(data.size()).empty());
    }
    // Real clipboard round trips, on the disposable CI runner only.
    clip::snapshot text; text.kind = "text"; text.text = "Clipboard \xE2\x9C\x93\r\nsecond line";
    auto seq = clip::publish(text, GetClipboardSequenceNumber());
    clip::require(clip::read().text == text.text);
    rejected = false;
    try { clip::publish(text, seq - 1); } catch (...) { rejected = true; }
    clip::require(rejected);
    clip::snapshot files; files.kind = "files"; files.files.push_back(path);
    clip::publish(files, GetClipboardSequenceNumber());
    clip::require(clip::read().files == files.files);
    clip::publish(text, GetClipboardSequenceNumber());
    clip::fs::remove(path); clip::fs::remove(directory);
    std::cout << "Clipboard native tests passed\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
