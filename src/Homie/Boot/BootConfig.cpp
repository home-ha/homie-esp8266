#include "BootConfig.hpp"

using namespace HomieInternals;

BootConfig::BootConfig()
: Boot("config")
, _http(80)
, _ssidCount(0)
, _wifiScanAvailable(false)
, _lastWifiScan(0)
, _lastWifiScanEnded(true)
, _flaggedForReboot(false)
, _flaggedForRebootAt(0)
{
}

BootConfig::~BootConfig() {
}

void BootConfig::setup() {
  Boot::setup();

  if (this->_interface->led.enable) {
    digitalWrite(this->_interface->led.pin, this->_interface->led.on);
  }

  const char* deviceId = Helpers.getDeviceId();

  Logger.log(F("Device ID is "));
  Logger.logln(deviceId);

  WiFi.mode(WIFI_AP);

  IPAddress apIp(192, 168, 1, 1);
  char apName[MAX_WIFI_SSID_LENGTH];
  strcpy(apName, this->_interface->brand);
  strcat_P(apName, PSTR("-"));
  strcat(apName, Helpers.getDeviceId());

  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName, deviceId);

  Logger.log(F("AP started as "));
  Logger.logln(apName);

  this->_dns.setTTL(300);
  this->_dns.setErrorReplyCode(DNSReplyCode::ServerFailure);
  this->_dns.start(53, F("homie.config"), apIp);

  this->_http.on("/", HTTP_GET, [this]() {
    Logger.logln(F("Received index request"));
    this->_http.send(200, F("text/plain"), F("See Configuration API usage: https://github.com/marvinroger/homie-esp8266/wiki/6.-Configuration-API"));
  });
  this->_http.on("/heart", HTTP_GET, [this]() {
    Logger.logln(F("Received heart request"));
    this->_http.send(200, FPSTR(PROGMEM_CONFIG_APPLICATION_JSON), F("{\"heart\":\"beat\"}"));
  });
  this->_http.on("/device-info", HTTP_GET, std::bind(&BootConfig::_onDeviceInfoRequest, this));
  this->_http.on("/networks", HTTP_GET, std::bind(&BootConfig::_onNetworksRequest, this));
  this->_http.on("/config", HTTP_PUT, std::bind(&BootConfig::_onConfigRequest, this));
  this->_http.on("/config", HTTP_OPTIONS, [this]() { // CORS
    Logger.logln(F("Received CORS request for /config"));
    this->_http.sendContent(FPSTR(PROGMEM_CONFIG_CORS));
  });
  this->_http.begin();
}

void BootConfig::_generateNetworksJson() {
  DynamicJsonBuffer generatedJsonBuffer = DynamicJsonBuffer(JSON_OBJECT_SIZE(1) + JSON_ARRAY_SIZE(this->_ssidCount) + (this->_ssidCount * JSON_OBJECT_SIZE(3))); // 1 at root, 3 in childrend
  JsonObject& json = generatedJsonBuffer.createObject();

  int jsonLength = 15; // {"networks":[]}
  JsonArray& networks = json.createNestedArray("networks");
  for (int network = 0; network < this->_ssidCount; network++) {
    jsonLength += 36; // {"ssid":"","rssi":,"encryption":""},
    JsonObject& jsonNetwork = generatedJsonBuffer.createObject();
    jsonLength += WiFi.SSID(network).length();
    jsonNetwork["ssid"] = WiFi.SSID(network);
    jsonLength += 4;
    jsonNetwork["rssi"] = WiFi.RSSI(network);
    jsonLength += 4;
    switch (WiFi.encryptionType(network)) {
      case ENC_TYPE_WEP:
        jsonNetwork["encryption"] = "wep";
        break;
      case ENC_TYPE_TKIP:
        jsonNetwork["encryption"] = "wpa";
        break;
      case ENC_TYPE_CCMP:
        jsonNetwork["encryption"] = "wpa2";
        break;
      case ENC_TYPE_NONE:
        jsonNetwork["encryption"] = "none";
        break;
      case ENC_TYPE_AUTO:
        jsonNetwork["encryption"] = "auto";
        break;
    }

    networks.add(jsonNetwork);
  }

  jsonLength++; // \0

  delete[] this->_jsonWifiNetworks;
  this->_jsonWifiNetworks = new char[jsonLength];
  json.printTo(this->_jsonWifiNetworks, jsonLength);
}

void BootConfig::_onDeviceInfoRequest() {
  Logger.logln(F("Received device info request"));

  DynamicJsonBuffer jsonBuffer = DynamicJsonBuffer(JSON_OBJECT_SIZE(4) + JSON_OBJECT_SIZE(2) + JSON_ARRAY_SIZE(this->_interface->registeredNodesCount) + (this->_interface->registeredNodesCount * JSON_OBJECT_SIZE(2)));
  int jsonLength = 82; // {"device_id":"","homie_version":"","firmware":{"name":"","version":""},"nodes":[]}
  JsonObject& json = jsonBuffer.createObject();
  jsonLength += strlen(Helpers.getDeviceId());
  json["device_id"] = Helpers.getDeviceId();
  jsonLength += strlen(VERSION);
  json["homie_version"] = VERSION;
  JsonObject& firmware = json.createNestedObject("firmware");
  jsonLength += strlen(this->_interface->firmware.name);
  firmware["name"] = this->_interface->firmware.name;
  jsonLength += strlen(this->_interface->firmware.version);
  firmware["version"] = this->_interface->firmware.version;

  JsonArray& nodes = json.createNestedArray("nodes");
  for (int i = 0; i < this->_interface->registeredNodesCount; i++) {
    jsonLength += 20; // {"id":"","type":""},
    HomieNode* node = this->_interface->registeredNodes[i];
    JsonObject& jsonNode = jsonBuffer.createObject();
    jsonLength += strlen(node->getId());
    jsonNode["id"] = node->getId();
    jsonLength += strlen(node->getType());
    jsonNode["type"] = node->getType();

    nodes.add(jsonNode);
  }

  jsonLength++; // \0

  std::unique_ptr<char[]> jsonString(new char[jsonLength]);
  json.printTo(jsonString.get(), jsonLength);
  this->_http.send(200, FPSTR(PROGMEM_CONFIG_APPLICATION_JSON), jsonString.get());
}

void BootConfig::_onNetworksRequest() {
  Logger.logln(F("Received networks request"));
  if (this->_wifiScanAvailable) {
    this->_http.send(200, FPSTR(PROGMEM_CONFIG_APPLICATION_JSON), this->_jsonWifiNetworks);
  } else {
    this->_http.send(503, FPSTR(PROGMEM_CONFIG_APPLICATION_JSON), FPSTR(PROGMEM_CONFIG_NETWORKS_FAILURE));
  }
}

void BootConfig::_onConfigRequest() {
  Logger.logln(F("Received config request"));
  if (this->_flaggedForReboot) {
    Logger.logln(F("✖ Device already configured"));
    this->_http.send(403, FPSTR(PROGMEM_CONFIG_APPLICATION_JSON), FPSTR(PROGMEM_CONFIG_JSON_FAILURE));
    return;
  }

  StaticJsonBuffer<MAX_JSON_CONFIG_ARDUINOJSON_BUFFER_SIZE> parseJsonBuffer;
  JsonObject& parsedJson = parseJsonBuffer.parseObject((char*)this->_http.arg("plain").c_str());
  if (!parsedJson.success()) {
    Logger.logln(F("✖ Invalid or too big JSON"));
    this->_http.send(400, FPSTR(PROGMEM_CONFIG_APPLICATION_JSON), FPSTR(PROGMEM_CONFIG_JSON_FAILURE));
    return;
  }

  if (!Helpers.validateConfig(parsedJson)) {
    this->_http.send(400, FPSTR(PROGMEM_CONFIG_APPLICATION_JSON), FPSTR(PROGMEM_CONFIG_JSON_FAILURE));
    return;
  }

  Config.write(this->_http.arg("plain"));

  Logger.logln(F("✔ Configured"));

  this->_http.send(200, FPSTR(PROGMEM_CONFIG_APPLICATION_JSON), "{\"success\":true}");

  this->_flaggedForReboot = true; // We don't reboot immediately, otherwise the response above is not sent
  this->_flaggedForRebootAt = millis();
}

void BootConfig::loop() {
  Boot::loop();

  this->_dns.processNextRequest();
  this->_http.handleClient();

  if (this->_flaggedForReboot) {
    if (millis() - this->_flaggedForRebootAt >= 3000UL) {
      Logger.logln(F("↻ Rebooting in normal mode"));
      ESP.restart();
    }

    return;
  }

  if (!this->_lastWifiScanEnded) {
    int8_t scanResult = WiFi.scanComplete();

    switch (scanResult) {
      case WIFI_SCAN_RUNNING:
        return;
      case WIFI_SCAN_FAILED:
        Logger.logln(F("✖ Wi-Fi scan failed"));
        this->_ssidCount = 0;
        this->_lastWifiScan = 0;
        break;
      default:
        Logger.logln(F("✔ Wi-Fi scan completed"));
        this->_ssidCount = scanResult;
        this->_generateNetworksJson();
        this->_wifiScanAvailable = true;
        break;
    }

    this->_lastWifiScanEnded = true;
  }

  unsigned long now = millis();
  if ((now - this->_lastWifiScan >= CONFIG_SCAN_INTERVAL || this->_lastWifiScan == 0) && this->_lastWifiScanEnded) {
    Logger.logln(F("Triggering Wi-Fi scan"));
    WiFi.scanNetworks(true);
    this->_lastWifiScan = now;
    this->_lastWifiScanEnded = false;
  }
}
