// NimBLE-flavoured BLE bridge for the JC3636W518EN port.
//
// Shares the public API declared in src/ble_bridge.h so main_jc3636.cpp
// can use bleInit / bleRead / bleWrite exactly like the legacy Bluedroid
// build. Internally this talks to the pioarduino fork's BLE library,
// which runs on the NimBLE host — so the Bluedroid-only calls used by
// the original src/ble_bridge.cpp had to be dropped.
//
// Differences vs. the Bluedroid version:
//  - Uses the conn-desc NimBLE callback overloads.
//  - bleClearBonds() is a no-op for now (Step 1 doesn't need it, and the
//    pioarduino BLE shim doesn't expose NimBLE's bond-store API directly).
//  - Pairing uses JustWorks LE Secure Connections by default. passkey
//    display is still handled so the pairing flow mirrors the Bluedroid
//    build when the host asks for one.

#include "../ble_bridge.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>
#include <BLE2902.h>
#include <Arduino.h>
#include <string.h>

#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

static const size_t RX_CAP = 2048;
static uint8_t  rxBuf[RX_CAP];
static volatile size_t rxHead = 0;
static volatile size_t rxTail = 0;

static BLEServer*         server = nullptr;
static BLECharacteristic* txChar = nullptr;
static BLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
// NimBLE negotiates MTU automatically; 185 matches macOS and is a safe
// default to chunk against. The real value is discoverable via the
// characteristic's conn info but that isn't needed for Step 1.
static volatile uint16_t  mtu = 185;
// Set once the client writes non-zero to the TX CCCD (notify or
// indicate). pioarduino's NimBLE notify() is a silent no-op until this
// is true, which is exactly the kind of silent failure that'd explain
// "TX bytes counted but desktop stays No-response".
static volatile bool      txSubscribed = false;

static void rxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) return;
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = c->getValue();
    if (v.length() > 0) {
      rxPush((const uint8_t*)v.c_str(), v.length());
    }
  }
};

// TX char needs its own callbacks so we can intercept the CCCD
// subscribe / unsubscribe event. subValue bitmask: 0x01 = notify on,
// 0x02 = indicate on, 0 = off.
class TxCallbacks : public BLECharacteristicCallbacks {
  void onSubscribe(BLECharacteristic*, ble_gap_conn_desc*, uint16_t subValue) override {
    txSubscribed = (subValue != 0);
    Serial.printf("[ble] tx subscribe subValue=0x%02x\n", (unsigned)subValue);
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s, ble_gap_conn_desc* /*desc*/) override {
    connected = true;
    Serial.println("[ble] connected");
  }
  void onDisconnect(BLEServer* s, ble_gap_conn_desc* /*desc*/) override {
    connected = false;
    secure = false;
    passkey = 0;
    txSubscribed = false;
    Serial.println("[ble] disconnected");
    BLEDevice::startAdvertising();
  }
  void onMtuChanged(BLEServer*, ble_gap_conn_desc* /*desc*/, uint16_t new_mtu) override {
    mtu = new_mtu;
    Serial.printf("[ble] mtu=%u\n", (unsigned)new_mtu);
  }
};

class SecCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  bool onConfirmPIN(uint32_t) override { return true; }
  bool onSecurityRequest() override { return true; }
  void onPassKeyNotify(uint32_t pk) override {
    passkey = pk;
    Serial.printf("[ble] passkey %06lu\n", (unsigned long)pk);
  }
  // NimBLE delivers a ble_gap_conn_desc on auth complete, which doesn't
  // carry an explicit "success" flag — if we got here the link is
  // encrypted. Bluedroid's success bit is inferred from that.
  void onAuthenticationComplete(ble_gap_conn_desc* /*desc*/) override {
    passkey = 0;
    secure = true;
    Serial.println("[ble] auth ok");
  }
};

void bleInit(const char* deviceName) {
  BLEDevice::init(deviceName);
  BLEDevice::setMTU(517);
  BLEDevice::setSecurityCallbacks(new SecCallbacks());

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* svc = server->createService(NUS_SERVICE_UUID);

  // IMPORTANT: Don't gate the characteristics on ESP_GATT_PERM_*_ENCRYPTED.
  // Under the NimBLE host + JustWorks pairing (no passkey callback fired
  // in Step 1) the link never flips to "MITM-encrypted", so an
  // encrypted-only char is silently unreadable. Keeping them open lets
  // the desktop subscribe to TX-notify and deliver writes to RX, which
  // is the critical path for Step 2. Security can be re-added once the
  // passkey flow is wired up.
  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  txChar->setCallbacks(new TxCallbacks());
  // NimBLE auto-adds 2902 on notify-capable chars.

  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  // Request bonded, non-MITM SC — matches JustWorks, which is what
  // pioarduino's NimBLE shim seems to fall into by default. Dropping
  // the MITM bit stops the stack from silently refusing pairing when
  // no passkey handler runs.
  BLESecurity* sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);
  sec->setKeySize(16);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.printf("[ble] advertising as '%s'\n", deviceName);
}

bool bleConnected() { return connected; }
bool bleSecure()    { return secure; }
bool bleTxSubscribed() { return txSubscribed; }
uint32_t blePasskey() { return passkey; }

void bleClearBonds() {
  // NimBLE's bond store isn't wrapped by pioarduino's BLE library; for
  // Step 1 we don't need this. If needed later, drop to NimBLE's
  // ble_store_clear() directly via the NimBLE host headers.
  Serial.println("[ble] clearBonds: no-op (NimBLE)");
}

size_t bleAvailable() {
  return (rxHead + RX_CAP - rxTail) % RX_CAP;
}

int bleRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  size_t chunk = mtu > 3 ? (size_t)(mtu - 3) : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    delay(4);
  }
  return sent;
}
