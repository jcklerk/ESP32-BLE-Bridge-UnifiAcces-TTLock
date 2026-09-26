#include "Util.h"
#include <mbedtls/base64.h>
#include <cstring>

String Util::bytesToHex(const uint8_t* data, size_t len) {
  static const char* h = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += h[(data[i] >> 4) & 0x0F];
    out += h[data[i] & 0x0F];
  }
  return out;
}

bool Util::hexToBytes(const String& input, uint8_t* out, size_t outLen) {
  String hex;
  hex.reserve(input.length());
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (isxdigit((unsigned char)c)) hex += c;
  }
  if (hex.length() != outLen * 2) return false;
  auto nyb = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < outLen; ++i) {
    int hi = nyb(hex[i * 2]);
    int lo = nyb(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

String Util::base64Encode(const uint8_t* data, size_t len) {
  size_t olen = 0;
  mbedtls_base64_encode(nullptr, 0, &olen, data, len);
  std::vector<uint8_t> buf(olen + 1);
  if (mbedtls_base64_encode(buf.data(), buf.size(), &olen, data, len) != 0) return "";
  buf[olen] = 0;
  return String(reinterpret_cast<char*>(buf.data()));
}

bool Util::base64Decode(const String& input, std::vector<uint8_t>& out) {
  size_t olen = 0;
  int rc = mbedtls_base64_decode(nullptr, 0, &olen,
                                 reinterpret_cast<const uint8_t*>(input.c_str()), input.length());
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) return false;
  out.resize(olen);
  rc = mbedtls_base64_decode(out.data(), out.size(), &olen,
                             reinterpret_cast<const uint8_t*>(input.c_str()), input.length());
  if (rc != 0) return false;
  out.resize(olen);
  return true;
}

bool Util::constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

void Util::secureZero(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) *p++ = 0;
}

String Util::jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}
