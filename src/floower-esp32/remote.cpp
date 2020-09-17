#include "remote.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#define LOG_TAG ""
#else
#include "esp_log.h"
static const char* LOG_TAG = "Remote";
#endif

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

// Floower custom service
#define FLOOWER_SERVICE_UUID "28e17913-66c1-475f-a76e-86b5242f4cec"
#define FLOOWER_NAME_UUID "ab130585-2b27-498e-a5a5-019391317350" // string
#define FLOOWER_STATE_UUID "ac292c4b-8bd0-439b-9260-2d9526fff89a" // see StatePacketData
#define FLOOWER_STATE_CHANGE_UUID "11226015-0424-44d3-b854-9fc332756cbf" // see StateChangePacketData
#define FLOOWER_COLORS_SCHEME_UUID "7b1e9cff-de97-4273-85e3-fd30bc72e128" // array of 3 bytes per pre-defined color [(R + G + B), (R +G + B), ..]
//#define FLOOWER__UUID "c380596f-10d2-47a7-95af-95835e0361c7"
//#define FLOOWER__UUID "10b8879e-0ea0-4fe2-9055-a244a1eaca8b"
//#define FLOOWER__UUID "03c6eedc-22b5-4a0e-9110-2cd0131cd528"

// https://docs.springcard.com/books/SpringCore/Host_interfaces/Physical_and_Transport/Bluetooth/Standard_Services
// Device Information profile
#define DEVICE_INFORMATION_UUID "180A"
#define DEVICE_INFORMATION_MODEL_NUMBER_STRING_UUID "2A24" // string
#define DEVICE_INFORMATION_SERIAL_NUMBER_UUID "2A25" // string
#define DEVICE_INFORMATION_FIRMWARE_REVISION_UUID "2A26" // string M.mm.bbbbb
#define DEVICE_INFORMATION_HARDWARE_REVISION_UUID "2A27" // string
#define DEVICE_INFORMATION_SOFTWARE_REVISION_UUID "2A28" // string M.mm.bbbbb
#define DEVICE_INFORMATION_MANUFACTURER_NAME_UUID "2A29" // string

// Battery level profile
#define BATTERY_UUID "180F"
#define BATTERY_LEVEL_UUID "2A19" // uint8
#define BATTERY_POWER_STATE_UUID "2A1A" // uint8 of states

#define BATTERY_POWER_STATE_CHARGING B00111011
#define BATTERY_POWER_STATE_DISCHARGING B00101111

Remote::Remote(Floower *floower, Config *config)
    : floower(floower), config(config) {
}

void Remote::init() {
  ESP_LOGI(LOG_TAG, "Initializing BLE server");

  // Create the BLE Device
  BLEDevice::init(config->name.c_str());

  // Create the BLE Server
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks(this));

  // Device Information profile service
  BLEService *deviceInformationService = server->createService(DEVICE_INFORMATION_UUID);
  createROCharacteristics(deviceInformationService, DEVICE_INFORMATION_MODEL_NUMBER_STRING_UUID, "Floower");
  createROCharacteristics(deviceInformationService, DEVICE_INFORMATION_SERIAL_NUMBER_UUID, String(config->serialNumber).c_str());
  createROCharacteristics(deviceInformationService, DEVICE_INFORMATION_FIRMWARE_REVISION_UUID, String(config->firmwareVersion).c_str());
  createROCharacteristics(deviceInformationService, DEVICE_INFORMATION_HARDWARE_REVISION_UUID, String(config->hardwareRevision).c_str());
  createROCharacteristics(deviceInformationService, DEVICE_INFORMATION_MANUFACTURER_NAME_UUID, "Floower Lab s.r.o.");
  deviceInformationService->start();

  // Battery level profile service
  batteryService = server->createService(BATTERY_UUID);
  BLECharacteristic* batteryLevelCharacteristic = batteryService->createCharacteristic(BATTERY_LEVEL_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  batteryLevelCharacteristic->addDescriptor(new BLE2902());
  BLECharacteristic* batteryStateCharacteristic = batteryService->createCharacteristic(BATTERY_POWER_STATE_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  batteryStateCharacteristic->addDescriptor(new BLE2902());
  batteryService->start();

  // Floower customer service
  floowerService = server->createService(FLOOWER_SERVICE_UUID);

  // device name characteristics
  BLECharacteristic* nameCharacteristic = floowerService->createCharacteristic(FLOOWER_NAME_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  nameCharacteristic->setValue(config->name.c_str());
  nameCharacteristic->setCallbacks(new NameCharacteristicsCallbacks(this));

  // state + state change characteristics
  floowerService->createCharacteristic(FLOOWER_STATE_UUID, BLECharacteristic::PROPERTY_READ); // read
  BLECharacteristic* stateChangeCharacteristic = floowerService->createCharacteristic(FLOOWER_STATE_CHANGE_UUID, BLECharacteristic::PROPERTY_WRITE); // write
  stateChangeCharacteristic->setCallbacks(new StateChangeCharacteristicsCallbacks(this));

  // color scheme characteristics
  BLECharacteristic* colorsSchemeCharacteristic = floowerService->createCharacteristic(FLOOWER_COLORS_SCHEME_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  size_t size = config->colorSchemeSize * 3;
  uint8_t bytes[size];
  for (uint8_t b = 0, i = 0; b < size; b += 3, i++) {
    bytes[b] = config->colorScheme[i].R;
    bytes[b + 1] = config->colorScheme[i].G;
    bytes[b + 2] = config->colorScheme[i].B;
  }
  colorsSchemeCharacteristic->setValue(bytes, size);
  //colorsSchemeCharacteristic->setCallbacks(new ColorsSchemeCharacteristicsCallbacks(this));

  floowerService->start();

  // Start advertising
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(FLOOWER_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
  advertising->setMinPreferred(0x12);
  advertising->start();

  ESP_LOGI(LOG_TAG, "Waiting a client connection to notify...");
  initialized = true;
}

void Remote::update() {
  if (floower->isIdle() && initialized) {
    // do something
    // TODO: do this only on change, probably do this from the outside?
    setState(floower->getPetalOpenLevel(), floower->getColor());
  }
}

void Remote::setState(uint8_t petalsOpenLevel, RgbColor color) {
  if (floowerService != NULL) {
    StatePacket statePacket = {{petalsOpenLevel, color.R, color.G, color.B}};
    BLECharacteristic* stateCharacteristic = floowerService->getCharacteristic(FLOOWER_STATE_UUID);
    stateCharacteristic->setValue(statePacket.bytes, STATE_PACKET_SIZE);
  }
}

void Remote::setBatteryLevel(uint8_t level, bool charging) {
  if (deviceConnected && batteryService != NULL && floower->isIdle()) {
    ESP_LOGD(LOG_TAG, "level: %d, charging: %d", level, charging);

    BLECharacteristic* characteristic = batteryService->getCharacteristic(BATTERY_LEVEL_UUID);
    characteristic->setValue(&level, 1);
    characteristic->notify();

    uint8_t batteryState = charging ? BATTERY_POWER_STATE_CHARGING : BATTERY_POWER_STATE_DISCHARGING;
    characteristic = batteryService->getCharacteristic(BATTERY_POWER_STATE_UUID);
    characteristic->setValue(&batteryState, 1);
    characteristic->notify();
  }
}

BLECharacteristic* Remote::createROCharacteristics(BLEService *service, const char *uuid, const char *value) {
  BLECharacteristic* characteristic = service->createCharacteristic(uuid, BLECharacteristic::PROPERTY_READ);                      
  characteristic->setValue(value);
}

void Remote::StateChangeCharacteristicsCallbacks::onWrite(BLECharacteristic *characteristic) {
  std::string bytes = characteristic->getValue();
  if (bytes.length() == STATE_CHANGE_PACKET_SIZE) {
    StateChangePacket statePacket;
    for (int i = 0; i < STATE_CHANGE_PACKET_SIZE; i ++) {
      statePacket.bytes[i] = bytes[i]; 
    }
    if (statePacket.data.mode & B1) {
      remote->floower->setColor(statePacket.data.getColor(), FloowerColorMode::TRANSITION, statePacket.data.duration * 100);
    }
    if (statePacket.data.mode & B10) {
      remote->floower->setPetalsOpenLevel(statePacket.data.petalsOpenLevel, statePacket.data.duration * 100);
    }
  }
}

void Remote::NameCharacteristicsCallbacks::onWrite(BLECharacteristic *characteristic) {
  std::string bytes = characteristic->getValue();
  if (bytes.length() > 0) {
    size_t length = min(bytes.length(), (unsigned int) NAME_MAX_LENGTH) + 1;
    char data[length];
    for (int i = 0; i < length; i ++) {
      data[i] = bytes[i]; 
    }
    data[length] = '\0';

    ESP_LOGI(LOG_TAG, "New name: %s", data);
    remote->config->setName(String(data));
    remote->config->commit();
  }
}

void Remote::ServerCallbacks::onConnect(BLEServer* server) {
  ESP_LOGI(LOG_TAG, "Connected to client");
  remote->deviceConnected = true;
};

void Remote::ServerCallbacks::onDisconnect(BLEServer* server) {
  ESP_LOGI(LOG_TAG, "Disconnected, start advertising");
  remote->deviceConnected = false;
  remote->floower->setColor(RgbColor(0), FloowerColorMode::TRANSITION, 100);
  server->startAdvertising(); // restart advertising
};