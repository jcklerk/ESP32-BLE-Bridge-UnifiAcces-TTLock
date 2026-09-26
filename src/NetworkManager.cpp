#include "NetworkManager.h"
#include "AppConfig.h"

#if USE_ETHERNET
static volatile bool ethConnected = false;
static void onNetEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setHostname(AppConfig::HOSTNAME);
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      ethConnected = true;
      Serial.printf("Ethernet IP: %s\n", ETH.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
      ethConnected = false;
      break;
    default: break;
  }
}
#endif

void NetworkManager::begin() {
#if USE_ETHERNET
  Network.begin();
  Network.onEvent(onNetEvent);
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE);
#else
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(AppConfig::HOSTNAME);
  // Wi-Fi development mode: configure these at build time.
  #ifndef WIFI_SSID
    #define WIFI_SSID ""
  #endif
  #ifndef WIFI_PASSWORD
    #define WIFI_PASSWORD ""
  #endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif
}

bool NetworkManager::connected() const {
#if USE_ETHERNET
  return ethConnected;
#else
  return WiFi.status() == WL_CONNECTED;
#endif
}

String NetworkManager::ip() const {
#if USE_ETHERNET
  return ETH.localIP().toString();
#else
  return WiFi.localIP().toString();
#endif
}
