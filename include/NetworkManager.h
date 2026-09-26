#pragma once

#include <Arduino.h>

#if USE_ETHERNET
#include <Network.h>
#include <ETH.h>
#else
#include <WiFi.h>
#endif

class NetworkManager {
 public:
  void begin();
  bool connected() const;
  String ip() const;
};
