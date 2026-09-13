/*
 * Copy this file to secrets.h and fill in your values:
 *   cp include/secrets.example.h include/secrets.h
 *
 * iPhone hotspot tips:
 *  - Settings > Personal Hotspot > Maximize Compatibility must be ON (2.4 GHz)
 *  - Rename the hotspot to something simple (no apostrophes)
 */
#pragma once

struct WifiCred {
  const char *ssid;
  const char *pass;
};

static const WifiCred WIFI_NETWORKS[] = {
    {"YOUR_HOME_WIFI", "YOUR_HOME_PASSWORD"},
    {"YOUR_HOTSPOT_NAME", "YOUR_HOTSPOT_PASSWORD"},
};
static const int WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

/* Firebase project */
#define FIREBASE_API_KEY "YOUR_FIREBASE_WEB_API_KEY"
#define FIREBASE_PROJECT_ID "esp32-waterai"

/* Identifies this device in the data */
#define DEVICE_ID "cyd-1"

/* Timezone: US Eastern with DST rules */
#define TZ_INFO "EST5EDT,M3.2.0,M11.1.0"
