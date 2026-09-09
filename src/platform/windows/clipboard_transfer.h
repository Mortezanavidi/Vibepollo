#pragma once
#include "clipboard_native.h"
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <nlohmann/json.hpp>

namespace clipboard_transfer {
  namespace native = clipboard_native;
  using json = nlohmann::json;
  struct permissions { bool read, write, download, upload; };
  struct transfer {
    std::string token;
    std::vector<native::file> files;
    native::fs::path directory;
    DWORD sequence = 0;
    bool complete = false;
    std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();
    ~transfer() {
      files.clear();
      if (!complete && !directory.empty()) {
        std::error_code ec;
        // Only files created in this transfer's private, randomly named directory.
        for (native::fs::directory_iterator i(directory, ec), end; !ec && i != end; i.increment(ec)) native::fs::remove(i->path(), ec);
        native::fs::remove(directory, ec);
      }
    }
  };
  inline std::mutex mutex;
  inline std::map<std::string, std::unique_ptr<transfer>> downloads, uploads;
  inline std::uint64_t stored_bytes = 0;
  inline void expire(std::map<std::string, std::unique_ptr<transfer>> &map) {
    for (auto it = map.begin(); it != map.end();) {
      if (std::chrono::steady_clock::now() - it->second->touched > std::chrono::minutes(10)) it = map.erase(it);
      else ++it;
    }
  }
  inline transfer &lookup(std::map<std::string, std::unique_ptr<transfer>> &map, const std::string &owner, const json &request) {
    auto it = map.find(owner);
    native::require(it != map.end() && it->second->token == request.at("token").get<std::string>());
    it->second->touched = std::chrono::steady_clock::now();
    return *it->second;
  }
  template<class TokenFactory>
  inline json handle(const std::string &owner, permissions perms, const json &request, TokenFactory new_token) {
    std::lock_guard<std::mutex> lock(mutex);
    expire(downloads); expire(uploads);
    const auto action = request.at("action").get<std::string>();
    if (action == "state") {
      native::require(perms.read);
      auto sequence = GetClipboardSequenceNumber();
      if (request.value("known", std::uint64_t(0)) == sequence) return {{"sequence", sequence}, {"kind", "unchanged"}};
      auto value = native::read();
      json out {{"sequence", value.sequence}, {"kind", value.kind}};
      if (value.kind == "text") out["text"] = value.text;
      if (value.kind == "files") {
        if (!perms.download) return {{"sequence", value.sequence}, {"kind", "unsupported"}};
        native::require(downloads.count(owner) || downloads.size() < 16);
        auto item = std::make_unique<transfer>();
        item->token = new_token(); item->sequence = value.sequence;
        out["files"] = json::array();
        std::uint64_t total = 0;
        for (const auto &path : value.files) {
          native::require(path.is_absolute());
          auto file = native::file::open(path);
          auto name = native::utf8(path.filename().wstring());
          native::require(native::safe_name(name));
          total += file.size; native::require(total <= native::max_batch);
          out["files"].push_back({{"name", name}, {"size", file.size}});
          item->files.push_back(std::move(file));
        }
        out["token"] = item->token;
        downloads[owner] = std::move(item);
      }
      return out;
    }
    if (action == "text") {
      native::require(perms.write);
      native::snapshot value; value.kind = "text"; value.text = request.at("text").get<std::string>();
      auto expected = request.at("sequence").get<std::uint64_t>();
      native::require(expected <= MAXDWORD);
      return {{"sequence", native::publish(value, static_cast<DWORD>(expected))}};
    }
    if (action == "read") {
      native::require(perms.read && perms.download);
      auto &item = lookup(downloads, owner, request);
      native::require(GetClipboardSequenceNumber() == item.sequence);
      auto index = request.at("index").get<std::uint64_t>();
      native::require(index < item.files.size());
      return {{"data", native::hex(item.files[index].read(request.at("offset").get<std::uint64_t>()))}};
    }
    if (action == "begin") {
      native::require(perms.write && perms.upload);
      native::require(uploads.count(owner) || uploads.size() < 16);
      const auto &manifest = request.at("files");
      native::require(manifest.is_array() && !manifest.empty() && manifest.size() <= native::max_files);
      std::uint64_t total = 0;
      std::set<std::wstring> names;
      for (const auto &entry : manifest) {
        auto name = entry.at("name").get<std::string>();
        native::require(native::safe_name(name));
        auto folded = native::wide(name);
        CharUpperBuffW(folded.data(), static_cast<DWORD>(folded.size()));
        native::require(names.insert(folded).second);
        auto size = entry.at("size").get<std::uint64_t>();
        native::require(size <= native::max_batch && total <= native::max_batch - size);
        total += size;
      }
      native::require(stored_bytes + total <= 1024ull * 1024 * 1024);
      auto expected = request.at("sequence").get<std::uint64_t>();
      native::require(expected <= MAXDWORD && GetClipboardSequenceNumber() == expected);
      auto item = std::make_unique<transfer>();
      item->token = new_token(); item->sequence = static_cast<DWORD>(expected);
      item->directory = native::fs::temp_directory_path() / ("Vibepollo-Clipboard-" + item->token);
      native::require(native::fs::space(item->directory.parent_path()).available > total + 16 * 1024 * 1024);
      native::require(native::fs::create_directory(item->directory));
      for (const auto &entry : manifest) {
        auto file = native::file::open(item->directory / native::wide(entry.at("name").get<std::string>()), true);
        file.size = entry.at("size").get<std::uint64_t>(); item->files.push_back(std::move(file));
      }
      stored_bytes += total; // Lifetime budget includes canceled transfers.
      auto result = json {{"token", item->token}};
      uploads[owner] = std::move(item);
      return result;
    }
    if (action == "write") {
      native::require(perms.write && perms.upload);
      auto &item = lookup(uploads, owner, request);
      native::require(!item.complete && GetClipboardSequenceNumber() == item.sequence);
      auto index = request.at("index").get<std::uint64_t>();
      native::require(index < item.files.size());
      item.files[index].append(request.at("offset").get<std::uint64_t>(), native::unhex(request.at("data").get<std::string>()));
      return {{"ok", true}};
    }
    if (action == "finish") {
      native::require(perms.write && perms.upload);
      auto &item = lookup(uploads, owner, request);
      native::require(!item.complete);
      native::snapshot value; value.kind = "files";
      for (auto &file : item.files) {
        native::require(file.written == file.size && FlushFileBuffers(file.handle.get()));
        value.files.push_back(file.path);
      }
      item.files.clear(); // Release write handles before Explorer reads the files.
      auto sequence = native::publish(value, item.sequence);
      item.complete = true;
      return {{"sequence", sequence}};
    }
    throw std::runtime_error("Unsupported clipboard action");
  }
}
