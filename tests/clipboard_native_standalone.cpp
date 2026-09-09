#include "src/platform/windows/clipboard_native.h"
#include "src/platform/windows/clipboard_transfer.h"
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
    namespace transfer = clipboard_transfer;
    using json = transfer::json;
    int counter = 0;
    auto random = [&] { return "test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter); };
    transfer::permissions all {true, true, true, true}, none {false, false, false, false};
    auto run = [&](const json &request) { return transfer::handle("test", all, request, random); };
    auto rejects = [&](auto operation) { bool failed = false; try { operation(); } catch (...) { failed = true; } clip::require(failed); };
    rejects([&] { transfer::handle("test", none, json {{"action", "state"}}, random); });
    auto begin = json {{"action", "begin"}, {"sequence", GetClipboardSequenceNumber()},
                       {"files", json::array({{{"name", "test.bin"}, {"size", data.size()}}})}};
    auto bad = begin; bad["files"][0]["name"] = "../escape";
    rejects([&] { run(bad); });
    bad = begin; bad["files"][0]["size"] = -1;
    rejects([&] { run(bad); });
    auto token = run(begin).at("token");
    auto write = json {{"action", "write"}, {"token", token}, {"index", 0}, {"offset", 0}, {"data", clip::hex(data)}};
    bad = write; bad["offset"] = 1;
    rejects([&] { run(bad); });
    rejects([&] { run(json {{"action", "finish"}, {"token", token}}); });
    run(write);
    run(json {{"action", "finish"}, {"token", token}});
    auto state = run(json {{"action", "state"}});
    auto read = json {{"action", "read"}, {"token", state.at("token")}, {"index", 0}, {"offset", 0}};
    clip::require(clip::unhex(run(read).at("data").get<std::string>()) == data);
    rejects([&] { transfer::handle("other-client", all, read, random); });
    rejects([&] { transfer::handle("test", none, read, random); });
    bad = read; bad["index"] = 1;
    rejects([&] { run(bad); });
    clip::publish(text, GetClipboardSequenceNumber());
    rejects([&] { run(read); });
    transfer::downloads.clear();
    transfer::uploads.at("test")->complete = false;
    transfer::uploads.clear();
    clip::fs::remove(path); clip::fs::remove(directory);
    std::cout << "Clipboard native tests passed\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
