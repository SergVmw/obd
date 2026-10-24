#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace h2gjson {

// The smallest JSON writer that will do for a response body built by hand.
//
// The head-unit feed and the identifier store both write JSON into a std::string
// rather than going through a document object: the host test suites compile
// without ArduinoJson, and a body assembled by appending cannot fail halfway
// through the way a fixed-capacity document can. Everything here is total - no
// call can fail and no allocation can be refused silently - so a partially
// written body is only possible if the caller stops writing.
class JsonBuf {
 public:
  explicit JsonBuf(size_t reserveBytes) { out_.reserve(reserveBytes); }
  const std::string& str() const { return out_; }
  std::string take() { return std::move(out_); }

  void raw(const char* s) { out_ += s; }
  void ch(char c) { out_.push_back(c); }

  void key(const char* k, bool commaBefore = true) {
    if (commaBefore) out_.push_back(',');
    out_.push_back('"');
    out_ += k;
    out_ += "\":";
  }

  void strVal(const char* s) {
    out_.push_back('"');
    if (s) {
      for (const char* p = s; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\') {
          out_.push_back('\\');
          out_.push_back(static_cast<char>(c));
        } else if (c >= 0x20) {
          out_.push_back(static_cast<char>(c));
        }
      }
    }
    out_.push_back('"');
  }

  void boolVal(bool v) { out_ += (v ? "true" : "false"); }

  void u32Val(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(v));
    out_ += buf;
  }

  void i32Val(int32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(v));
    out_ += buf;
  }

  void optFloat(bool present, double v) {
    if (!present || !std::isfinite(v)) {
      out_ += "null";
      return;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    out_ += buf;
  }

  void numFloat(double v) { optFloat(true, v); }

 private:
  std::string out_;
};


}  // namespace h2gjson
