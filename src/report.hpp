// Report line format and parser. One line per stall, space-delimited
// key=value pairs, frames last so an exe path may contain spaces.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace sw {

struct StallRec {
  int32_t pid = 0;
  std::string name;
  uint64_t start_ns = 0;
  double duration_us = 0;
  double detect_us = 0;
  double handler_us = 0;
  uint64_t slide = 0;
  std::string exe;
  std::vector<uint64_t> frames;
};

inline std::string format_stall_line(const StallRec& r) {
  char head[640];
  snprintf(head, sizeof head,
           "stall pid=%d name=%s start_ns=%llu duration_us=%.1f detect_us=%.1f "
           "handler_us=%.1f slide=0x%llx exe=%s frames=",
           r.pid, r.name.empty() ? "-" : r.name.c_str(), (unsigned long long)r.start_ns,
           r.duration_us, r.detect_us, r.handler_us, (unsigned long long)r.slide,
           r.exe.empty() ? "-" : r.exe.c_str());
  std::string out = head;
  if (r.frames.empty()) {
    out += "-";
  } else {
    char fb[32];
    for (size_t i = 0; i < r.frames.size(); i++) {
      snprintf(fb, sizeof fb, "%s0x%llx", i ? "," : "", (unsigned long long)r.frames[i]);
      out += fb;
    }
  }
  return out;
}

inline bool parse_stall_line(const std::string& line, StallRec& out) {
  if (line.rfind("stall ", 0) != 0) return false;
  auto field = [&](const char* key, size_t* val_pos) -> std::string {
    std::string k = std::string(" ") + key + "=";
    size_t p = line.find(k);
    if (p == std::string::npos) return {};
    p += k.size();
    if (val_pos) *val_pos = p;
    size_t e = line.find(' ', p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
  };
  std::string s;
  s = field("pid", nullptr);
  if (s.empty()) return false;
  out.pid = int32_t(strtol(s.c_str(), nullptr, 10));
  out.name = field("name", nullptr);
  if (out.name.empty()) return false;
  s = field("start_ns", nullptr);
  if (s.empty()) return false;
  out.start_ns = strtoull(s.c_str(), nullptr, 10);
  s = field("duration_us", nullptr);
  if (s.empty()) return false;
  out.duration_us = strtod(s.c_str(), nullptr);
  s = field("detect_us", nullptr);
  if (s.empty()) return false;
  out.detect_us = strtod(s.c_str(), nullptr);
  s = field("handler_us", nullptr);
  if (s.empty()) return false;
  out.handler_us = strtod(s.c_str(), nullptr);
  s = field("slide", nullptr);
  if (s.empty()) return false;
  out.slide = strtoull(s.c_str(), nullptr, 16);
  // exe may contain spaces: it runs until " frames=", which is always last.
  size_t exe_pos = 0;
  if (field("exe", &exe_pos).empty() && line.find(" exe=") == std::string::npos) return false;
  size_t fr = line.find(" frames=", exe_pos);
  if (fr == std::string::npos) return false;
  out.exe = line.substr(exe_pos, fr - exe_pos);
  std::string flist = line.substr(fr + strlen(" frames="));
  out.frames.clear();
  if (!flist.empty() && flist != "-") {
    size_t p = 0;
    while (p < flist.size()) {
      size_t c = flist.find(',', p);
      std::string tok = flist.substr(p, c == std::string::npos ? std::string::npos : c - p);
      if (tok.empty()) return false;
      out.frames.push_back(strtoull(tok.c_str(), nullptr, 16));
      if (c == std::string::npos) break;
      p = c + 1;
    }
  }
  return true;
}

} // namespace sw
