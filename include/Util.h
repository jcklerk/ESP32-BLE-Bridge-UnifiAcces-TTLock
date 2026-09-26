#pragma once

#include <Arduino.h>
#include <vector>

namespace Util {
String bytesToHex(const uint8_t* data, size_t len);
bool hexToBytes(const String& hex, uint8_t* out, size_t outLen);
String base64Encode(const uint8_t* data, size_t len);
bool base64Decode(const String& input, std::vector<uint8_t>& out);
bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len);
void secureZero(void* ptr, size_t len);
String jsonEscape(const String& input);
}
