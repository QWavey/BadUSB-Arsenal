#ifndef DEADNET_H
#define DEADNET_H

#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Attack modes (bitmask)
#define ATTACK_MODE_ARP    (1 << 0)
#define ATTACK_MODE_RA     (1 << 1)
#define ATTACK_MODE_DEAUTH (1 << 2)
#define ATTACK_MODE_BLIND  (1 << 3)
#define ATTACK_MODE_DNS    (1 << 4)
#define ATTACK_MODE_SNIFF  (1 << 5)
#define ATTACK_MODE_BOTH   (ATTACK_MODE_ARP | ATTACK_MODE_RA | ATTACK_MODE_DEAUTH | ATTACK_MODE_DNS | ATTACK_MODE_SNIFF)

struct Host {
    IPAddress ip;
    uint8_t   mac[6];
    bool      macKnown;
    uint16_t  pingMs;
    String    hostname;
};

class Deadnet {
public:
    Deadnet();
    void begin();

    // Attack lifecycle
    bool startAttack(uint8_t mode = ATTACK_MODE_ARP, const std::vector<IPAddress>& targets = {});
    void stopAttack();
    bool isRunning() const { return _running; }
    uint8_t getMode() const { return _mode; }

    // Status/diagnostics
    uint32_t getPacketsSent()  const { return _pktCount; }
    uint32_t getCycleCount()   const { return _cycleCount; }
    String   getGatewayIpStr() const { return _gatewayIp.toString(); }
    String   getGatewayMacStr() const;

    // Host list (populated by ARP scan / passive sniff)
    std::vector<Host> getHosts();
    void addHost(IPAddress ip, const uint8_t* mac, uint16_t pingMs = 0);
    void clearHosts();

    // ---- Sniffing ----
    struct SniffedData {
        String source;
        String type;
        String content;
        uint32_t timestamp;
    };
    std::vector<SniffedData> getSniffedData();
    void addSniffLog(String src, String type, String content);
    void clearSniffLogs();

private:
    // ---- state ----
    volatile bool    _running    = false;
    uint8_t          _mode       = ATTACK_MODE_ARP;
    volatile uint32_t _pktCount  = 0;
    volatile uint32_t _cycleCount = 0;

    // ---- network params ----
    IPAddress _gatewayIp;
    uint8_t   _gatewayMac[6];
    uint8_t   _myMac[6];
    uint8_t   _bssid[6];
    std::vector<IPAddress> _targets;

    // ---- host list ----
    SemaphoreHandle_t _hostsMutex = nullptr;
    std::vector<Host> _hosts;

    // ---- FreeRTOS task handles ----
    TaskHandle_t _attackTaskHandle = nullptr;
    TaskHandle_t _scanTaskHandle   = nullptr;
    TaskHandle_t _dnsTaskHandle    = nullptr;

    // ---- internal helpers ----
    void     resolveNetworkParams();
    bool     sendArpPacket(IPAddress srcIp, const uint8_t* srcMac,
                           IPAddress dstIp, const uint8_t* dstMac,
                           IPAddress senderIp, const uint8_t* senderMac,
                           IPAddress targetIp, const uint8_t* targetMac,
                           uint16_t opcode);
    void     sendRaSpoof();
    void     sendDeauth(const uint8_t* targetMac);
    void     scanSubnetArp();
    void     poisonSingleHost(const Host& host);
    void     poisonAllHosts();

    void     generateRandomMac(uint8_t* out);
    bool     parseMac(const String& macStr, uint8_t* out);
    bool     isZeroMac(const uint8_t* mac) const;
    String   resolveHostname(IPAddress target);

    static void attackTask(void* pv);
    static void subnetScanTask(void* pv);
    static void dnsSpoofTask(void* pv);
};

// Global instance, defined in GlobalState.cpp — every .cpp/.ino that
// includes deadnet.h can just call g_deadnet.startAttack() etc.
extern class Deadnet g_deadnet;

#endif // DEADNET_H
