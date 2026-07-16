#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_mac.h>
#include "mbedtls/aes.h"
#include <stdarg.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
const uint8_t OLED_ADDRESS = 0x3C;

#define LORA_SCK     5
#define LORA_MISO    19
#define LORA_MOSI    27
#define LORA_SS      18
#define LORA_RST     23
#define LORA_DIO0    26
#define LORA_BAND    868E6

#define LED_PIN      25
const unsigned long LED_ON_MS = 100;
unsigned long ledOffTime = 0;

Preferences prefs;
static char apSSID[20];
static char nodeID[12];
static char apAddrStr[24];

#define AP_PASSWORD "change_me"

#define MESH_PSK_HEX "change_me"

static uint8_t MESH_PSK[16];

static void meshKeyInit() {
  const char *hex = MESH_PSK_HEX;
  size_t len = strlen(hex);
  if (len != 32) {
    Serial.println("[MESH] FATAL: MESH_PSK_HEX must be exactly 32 hex characters (16 bytes)");
    while (true) delay(1000);
  }
  for (int i = 0; i < 16; ++i) {
    char byteStr[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
    char *endptr = nullptr;
    long val = strtol(byteStr, &endptr, 16);
    if (endptr == byteStr || *endptr != '\0') {
      Serial.println("[MESH] FATAL: MESH_PSK_HEX contains invalid hex characters");
      while (true) delay(1000);
    }
    MESH_PSK[i] = (uint8_t)val;
  }
}

#define IRC_PORT        6667
#define MAX_CLIENTS     6
#define MAX_NICK_LEN    24
#define MAX_CHAN_LEN    32
#define MAX_LINE_LEN    512
#define IRC_MAX_LINE    512
#define DEFAULT_CHANNEL "#mesh"
#define SERVER_NAME     "archaeon.mesh"
#define MAX_CHANNELS_PER_CLIENT 4

WiFiServer ircServer(IRC_PORT);

struct IRCClient {
  WiFiClient conn;
  bool active = false;
  bool registered = false;
  char nick[MAX_NICK_LEN] = "";
  char user[MAX_NICK_LEN] = "";
  char channels[MAX_CHANNELS_PER_CLIENT][MAX_CHAN_LEN];
  uint8_t channelCount = 0;
  char lineBuf[MAX_LINE_LEN];
  size_t lineIdx = 0;
  bool capNegotiating = false;
  unsigned long lastActivity = 0;
  unsigned long pingSentAt = 0;
  bool awaitingPong = false;
};

#define IRC_PING_INTERVAL_MS    60000UL
#define IRC_PING_GRACE_MS       20000UL

IRCClient clients[MAX_CLIENTS];

const size_t MAX_SEND_LEN = 200;
#define OUT_QUEUE_SIZE      48
#define MESH_MAX_TTL        5
#define MESH_SEEN_CACHE     256
#define MAX_PLAIN_LEN       300
#define CIPHER_BUF_LEN      (MAX_PLAIN_LEN + 16)
#define CHUNK_HEX_LEN       100
#define MAX_CHUNKS_PER_MSG  8
#define MAX_REASSEMBLY      8
#define REASSEMBLY_TIMEOUT_MS 20000

const unsigned long CHUNK_DELAY_MS   = 2000;
const unsigned long CHUNK_JITTER_MS  = 500;
unsigned long nodeStaggerOffsetMs    = 0;

#define AIRTIME_WINDOW_MS      60000UL
#define AIRTIME_MAX_TX_PER_WINDOW 40
unsigned long airtimeWindowStart = 0;
uint16_t airtimeTxInWindow = 0;

static char outQueue[OUT_QUEUE_SIZE][MAX_SEND_LEN + 1];
static uint8_t qHead = 0, qTail = 0, qCount = 0;

unsigned long lastSendTime = 0;
unsigned long sentCount = 0, recvCount = 0;
int lastRSSI = 0;

static char lastSentBuf[MAX_SEND_LEN + 1] = "<none>";
static char lastReceivedBuf[256] = "<none>";

unsigned long lastOledUpdate = 0;
const unsigned long OLED_UPDATE_MS = 300;
bool oledDirty = true;

static uint32_t seenIDs[MESH_SEEN_CACHE];
static uint8_t seenHead = 0, seenCount = 0;

struct ReassemblyMsg {
  bool active = false;
  uint32_t msgid = 0;
  uint8_t total = 0;
  uint8_t receivedCount = 0;
  bool gotChunk[MAX_CHUNKS_PER_MSG] = {false};
  char chunkHex[MAX_CHUNKS_PER_MSG][CHUNK_HEX_LEN + 1];
  char channel[MAX_CHAN_LEN] = "";
  char srcNode[12] = "";
  unsigned long lastUpdate = 0;
};
ReassemblyMsg reassembly[MAX_REASSEMBLY];

ReassemblyMsg *reassemblyFind(uint32_t msgid);
ReassemblyMsg *reassemblyCreate(uint32_t msgid, uint8_t total, const char *channel, const char *src);
bool isInChannel(IRCClient &c, const char *chan);
bool joinChannel(IRCClient &c, const char *chan);
void partChannel(IRCClient &c, const char *chan);
IRCClient *ircFindClientByNick(const char *nick);
void ircSendLine(IRCClient &c, const char *line);
void ircNumericf(IRCClient &c, int code, const char *fmt, ...);
void ircJoinAndAnnounce(IRCClient &c, const char *chan);
void ircWelcome(IRCClient &c);
void ircHandleLine(IRCClient &c, char *line);
void ircQuitClient(IRCClient &c, const char *reason, bool sendError);
bool ircNickInUse(const char *nick, IRCClient *exclude);
bool ircNickValid(const char *nick);

static uint32_t crc32Table[256];
static bool crc32TableReady = false;

static void crc32InitTable() {
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int b = 0; b < 8; ++b)
      c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
    crc32Table[i] = c;
  }
  crc32TableReady = true;
}

uint32_t crc32_bytes(const uint8_t *data, size_t len) {
  if (!crc32TableReady) crc32InitTable();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc = crc32Table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

void bytesToHex(const uint8_t *data, size_t len, char *out) {
  static const char *hx = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[i * 2]     = hx[data[i] >> 4];
    out[i * 2 + 1] = hx[data[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t hexToBytes(const char *hex, uint8_t *out, size_t maxOut) {
  size_t hlen = strlen(hex);
  size_t n = hlen / 2;
  if (n > maxOut) n = maxOut;
  for (size_t i = 0; i < n; ++i) {
    int hi = hexNibble(hex[i * 2]);
    int lo = hexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return 0;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return n;
}

size_t meshEncrypt(const char *plaintext, uint8_t *outCipher, uint8_t *outIV) {
  size_t plainLen = strlen(plaintext);
  if (plainLen > MAX_PLAIN_LEN - 1) plainLen = MAX_PLAIN_LEN - 1;

  size_t padded = ((plainLen / 16) + 1) * 16;
  uint8_t padBuf[MAX_PLAIN_LEN + 16];
  memcpy(padBuf, plaintext, plainLen);
  uint8_t padVal = (uint8_t)(padded - plainLen);
  for (size_t i = plainLen; i < padded; ++i) padBuf[i] = padVal;

  esp_fill_random(outIV, 16);
  uint8_t ivWork[16];
  memcpy(ivWork, outIV, 16);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, MESH_PSK, 128);
  int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, ivWork, padBuf, outCipher);
  mbedtls_aes_free(&aes);
  if (rc != 0) return 0;
  return padded;
}

size_t meshDecrypt(const uint8_t *cipher, size_t cipherLen, const uint8_t *iv, char *outPlain, size_t outPlainMax) {
  if (cipherLen == 0 || (cipherLen % 16) != 0) return 0;
  uint8_t plainBuf[MAX_PLAIN_LEN + 16];
  if (cipherLen > sizeof(plainBuf)) return 0;

  uint8_t ivWork[16];
  memcpy(ivWork, iv, 16);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, MESH_PSK, 128);
  int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipherLen, ivWork, cipher, plainBuf);
  mbedtls_aes_free(&aes);
  if (rc != 0) return 0;

  uint8_t padVal = plainBuf[cipherLen - 1];
  if (padVal == 0 || padVal > 16 || padVal > cipherLen) return 0;
  size_t plainLen = cipherLen - padVal;
  if (plainLen >= outPlainMax) plainLen = outPlainMax - 1;

  memcpy(outPlain, plainBuf, plainLen);
  outPlain[plainLen] = '\0';
  return plainLen;
}

void initNodeID() {
  prefs.begin("archaeon", false);
  if (prefs.isKey("node_id")) {
    prefs.getString("node_id", nodeID, sizeof(nodeID));
  } else {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(nodeID, sizeof(nodeID), "node_%02x%02x%02x", mac[3], mac[4], mac[5]);
    prefs.putString("node_id", nodeID);
  }
  prefs.end();
  snprintf(apSSID, sizeof(apSSID), "%s", nodeID);

  uint32_t h = crc32_bytes((const uint8_t *)nodeID, strlen(nodeID));
  nodeStaggerOffsetMs = h % (CHUNK_DELAY_MS + CHUNK_JITTER_MS);
}

void ledOn()  { digitalWrite(LED_PIN, HIGH); ledOffTime = millis() + LED_ON_MS; }
void ledOff() { digitalWrite(LED_PIN, LOW); }

bool enqueueMessage(const char *msg) {
  if (!msg || qCount >= OUT_QUEUE_SIZE) return false;
  strncpy(outQueue[qTail], msg, MAX_SEND_LEN);
  outQueue[qTail][MAX_SEND_LEN] = '\0';
  qTail = (qTail + 1) % OUT_QUEUE_SIZE;
  qCount++;
  oledDirty = true;
  return true;
}

bool dequeueMessage(char *dest, size_t destLen) {
  if (qCount == 0 || !dest || destLen == 0) return false;
  strncpy(dest, outQueue[qHead], destLen - 1);
  dest[destLen - 1] = '\0';
  qHead = (qHead + 1) % OUT_QUEUE_SIZE;
  qCount--;
  oledDirty = true;
  return true;
}

bool airtimeBudgetAvailable() {
  unsigned long now = millis();
  if (now - airtimeWindowStart >= AIRTIME_WINDOW_MS) {
    airtimeWindowStart = now;
    airtimeTxInWindow = 0;
  }
  return airtimeTxInWindow < AIRTIME_MAX_TX_PER_WINDOW;
}

void sendLoRaStringNonBlocking(const char *s) {
  if (!s || s[0] == '\0') return;
  if (!airtimeBudgetAvailable()) {
    return;
  }
  strncpy(lastSentBuf, s, sizeof(lastSentBuf) - 1);
  lastSentBuf[sizeof(lastSentBuf) - 1] = '\0';
  ledOn();
  LoRa.beginPacket();
  LoRa.print(s);
  LoRa.endPacket();
  sentCount++;
  airtimeTxInWindow++;
  Serial.print("[MESH TX] "); Serial.println(s);
  oledDirty = true;
}

bool meshSeen(uint32_t key) {
  for (uint8_t i = 0; i < seenCount; ++i) if (seenIDs[i] == key) return true;
  return false;
}
void meshRemember(uint32_t key) {
  seenIDs[seenHead] = key;
  seenHead = (seenHead + 1) % MESH_SEEN_CACHE;
  if (seenCount < MESH_SEEN_CACHE) seenCount++;
}
uint32_t chunkKey(uint32_t msgid, uint8_t idx) {
  return msgid ^ ((uint32_t)(idx + 1) * 0x9E3779B1u);
}

ReassemblyMsg *reassemblyFind(uint32_t msgid) {
  for (int i = 0; i < MAX_REASSEMBLY; ++i)
    if (reassembly[i].active && reassembly[i].msgid == msgid) return &reassembly[i];
  return NULL;
}

ReassemblyMsg *reassemblyCreate(uint32_t msgid, uint8_t total, const char *channel, const char *src) {
  unsigned long now = millis();
  int victim = -1;
  unsigned long oldest = 0xFFFFFFFFu;
  for (int i = 0; i < MAX_REASSEMBLY; ++i) {
    if (!reassembly[i].active) { victim = i; break; }
    if (reassembly[i].lastUpdate < oldest) { oldest = reassembly[i].lastUpdate; victim = i; }
  }
  ReassemblyMsg &r = reassembly[victim];
  r = ReassemblyMsg();
  r.active = true;
  r.msgid = msgid;
  r.total = (total > MAX_CHUNKS_PER_MSG) ? MAX_CHUNKS_PER_MSG : total;
  r.receivedCount = 0;
  strncpy(r.channel, channel, sizeof(r.channel) - 1);
  strncpy(r.srcNode, src, sizeof(r.srcNode) - 1);
  r.lastUpdate = now;
  return &r;
}

void reassemblyExpireStale() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_REASSEMBLY; ++i) {
    if (reassembly[i].active && (now - reassembly[i].lastUpdate) > REASSEMBLY_TIMEOUT_MS) {
      reassembly[i].active = false;
    }
  }
}

void ircBroadcastLocal(const char *channel, const char *fromNick, const char *text, IRCClient *except);

void meshPublish(const char *channel, const char *nick, const char *text) {
  char plain[MAX_PLAIN_LEN];
  snprintf(plain, sizeof(plain), "%s\x1f%s", nick, text);

  uint8_t iv[16];
  uint8_t cipher[CIPHER_BUF_LEN];
  size_t cipherLen = meshEncrypt(plain, cipher, iv);
  if (cipherLen == 0) {
    Serial.println("[MESH] encrypt failed, dropping message");
    return;
  }

  uint8_t combined[16 + CIPHER_BUF_LEN];
  memcpy(combined, iv, 16);
  memcpy(combined + 16, cipher, cipherLen);
  size_t combinedLen = 16 + cipherLen;

  char hexBuf[(16 + CIPHER_BUF_LEN) * 2 + 1];
  bytesToHex(combined, combinedLen, hexBuf);
  size_t hexLen = combinedLen * 2;

  uint8_t totalChunks = (uint8_t)((hexLen + CHUNK_HEX_LEN - 1) / CHUNK_HEX_LEN);
  if (totalChunks > MAX_CHUNKS_PER_MSG) totalChunks = MAX_CHUNKS_PER_MSG;
  if (totalChunks == 0) totalChunks = 1;

  uint32_t salt = millis() ^ esp_random();
  char idsrc[64];
  snprintf(idsrc, sizeof(idsrc), "%s|%lu", nodeID, (unsigned long)salt);
  uint32_t id = crc32_bytes((const uint8_t *)idsrc, strlen(idsrc));

  for (uint8_t i = 0; i < totalChunks; ++i) {
    size_t start = (size_t)i * CHUNK_HEX_LEN;
    size_t remain = hexLen - start;
    size_t take = remain < CHUNK_HEX_LEN ? remain : CHUNK_HEX_LEN;

    char chunkHex[CHUNK_HEX_LEN + 1];
    memcpy(chunkHex, hexBuf + start, take);
    chunkHex[take] = '\0';

    meshRemember(chunkKey(id, i));

    char packet[MAX_SEND_LEN + 1];
    snprintf(packet, sizeof(packet), "C|%08lx|%d|%d|%d|%s|%s|%s",
             (unsigned long)id, i, totalChunks, MESH_MAX_TTL, nodeID, channel, chunkHex);
    enqueueMessage(packet);
  }
}

void meshHandleIncoming(const char *raw) {
  if (raw[0] != 'C' || raw[1] != '|') return;

  char buf[256];
  strncpy(buf, raw, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *save;
  strtok_r(buf, "|", &save);
  char *idStr   = strtok_r(NULL, "|", &save);
  char *idxStr  = strtok_r(NULL, "|", &save);
  char *totStr  = strtok_r(NULL, "|", &save);
  char *ttlStr  = strtok_r(NULL, "|", &save);
  char *src     = strtok_r(NULL, "|", &save);
  char *chan    = strtok_r(NULL, "|", &save);
  char *hexChunk = strtok_r(NULL, "", &save);

  if (!idStr || !idxStr || !totStr || !ttlStr || !src || !chan || !hexChunk) return;

  uint32_t id = strtoul(idStr, NULL, 16);
  int idx = atoi(idxStr);
  int total = atoi(totStr);
  int ttl = atoi(ttlStr);
  if (idx < 0 || idx >= MAX_CHUNKS_PER_MSG || total <= 0) return;

  uint32_t ckey = chunkKey(id, (uint8_t)idx);
  if (meshSeen(ckey)) return;
  meshRemember(ckey);

  if (ttl > 0) {
    char packet[MAX_SEND_LEN + 1];
    snprintf(packet, sizeof(packet), "C|%s|%d|%d|%d|%s|%s|%s",
             idStr, idx, total, ttl - 1, src, chan, hexChunk);
    enqueueMessage(packet);
  }

  reassemblyExpireStale();
  ReassemblyMsg *r = reassemblyFind(id);
  if (!r) r = reassemblyCreate(id, (uint8_t)total, chan, src);

  if (!r->gotChunk[idx]) {
    strncpy(r->chunkHex[idx], hexChunk, CHUNK_HEX_LEN);
    r->chunkHex[idx][CHUNK_HEX_LEN] = '\0';
    r->gotChunk[idx] = true;
    r->receivedCount++;
    r->lastUpdate = millis();
  }

  if (r->receivedCount < r->total) return;

  char fullHex[MAX_CHUNKS_PER_MSG * CHUNK_HEX_LEN + 1];
  fullHex[0] = '\0';
  for (uint8_t i = 0; i < r->total; ++i) strcat(fullHex, r->chunkHex[i]);

  uint8_t combined[(16 + CIPHER_BUF_LEN)];
  size_t combinedLen = hexToBytes(fullHex, combined, sizeof(combined));
  r->active = false;

  if (combinedLen < 32) {
    Serial.println("[MESH] reassembled message too short/corrupt, dropping");
    return;
  }
  uint8_t *iv = combined;
  uint8_t *cipher = combined + 16;
  size_t cipherLen = combinedLen - 16;

  char plain[MAX_PLAIN_LEN + 16];
  size_t plainLen = meshDecrypt(cipher, cipherLen, iv, plain, sizeof(plain));
  if (plainLen == 0) {
    Serial.println("[MESH] decrypt failed (wrong key or corrupt packet), dropping");
    return;
  }

  char *sep = strchr(plain, 0x1F);
  if (!sep) return;
  *sep = '\0';
  const char *nick = plain;
  const char *text = sep + 1;

  char fullNick[48];
  snprintf(fullNick, sizeof(fullNick), "%s!%s@mesh", nick, src);
  ircBroadcastLocal(chan, fullNick, text, NULL);

  Serial.printf("[MESH RX] id=%08lx src=%s chan=%s nick=%s text=%s\n",
                (unsigned long)id, src, chan, nick, text);
}

bool isInChannel(IRCClient &c, const char *chan) {
  for (uint8_t i = 0; i < c.channelCount; ++i)
    if (strcmp(c.channels[i], chan) == 0) return true;
  return false;
}

IRCClient *ircFindClientByNick(const char *nick) {
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    IRCClient &c = clients[i];
    if (c.active && c.registered && strcasecmp(c.nick, nick) == 0) return &c;
  }
  return NULL;
}

bool joinChannel(IRCClient &c, const char *chan) {
  if (isInChannel(c, chan)) return true;
  if (c.channelCount >= MAX_CHANNELS_PER_CLIENT) return false;
  strncpy(c.channels[c.channelCount], chan, MAX_CHAN_LEN - 1);
  c.channels[c.channelCount][MAX_CHAN_LEN - 1] = '\0';
  c.channelCount++;
  return true;
}

void partChannel(IRCClient &c, const char *chan) {
  for (uint8_t i = 0; i < c.channelCount; ++i) {
    if (strcmp(c.channels[i], chan) == 0) {
      for (uint8_t j = i; j < c.channelCount - 1; ++j)
        strcpy(c.channels[j], c.channels[j + 1]);
      c.channelCount--;
      return;
    }
  }
}

void ircSendLine(IRCClient &c, const char *line) {
  if (!c.active || !c.conn.connected()) return;
  size_t len = strlen(line);
  const size_t maxPayload = IRC_MAX_LINE - 2;
  if (len > maxPayload) len = maxPayload;
  c.conn.write((const uint8_t *)line, len);
  c.conn.print("\r\n");
}

size_t ircBuildTextLine(char *out, size_t outSize, const char *head, const char *text) {
  if (outSize == 0) return 0;
  size_t headLen = strlen(head);
  if (headLen >= outSize) headLen = outSize - 1;
  memcpy(out, head, headLen);

  size_t maxPayload = IRC_MAX_LINE - 2;
  if (maxPayload >= outSize) maxPayload = outSize - 1;

  size_t maxTextLen = (maxPayload > headLen) ? (maxPayload - headLen) : 0;
  size_t textLen = strlen(text);
  if (textLen > maxTextLen) textLen = maxTextLen;

  memcpy(out + headLen, text, textLen);
  out[headLen + textLen] = '\0';
  return headLen + textLen;
}

void ircNumericf(IRCClient &c, int code, const char *fmt, ...) {
  char args[220];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(args, sizeof(args), fmt, ap);
  va_end(ap);

  const char *nick = c.nick[0] ? c.nick : "*";
  char line[256];
  snprintf(line, sizeof(line), ":%s %03d %s %s", SERVER_NAME, code, nick, args);
  ircSendLine(c, line);
}

void ircBroadcastLocal(const char *channel, const char *fromNick, const char *text, IRCClient *except) {
  char line[IRC_MAX_LINE];
  char head[80];
  if (channel[0] == '@') {
    const char *targetNick = channel + 1;
    IRCClient *c = ircFindClientByNick(targetNick);
    if (!c || c == except) return;
    snprintf(head, sizeof(head), ":%s PRIVMSG %s :", fromNick, targetNick);
    ircBuildTextLine(line, sizeof(line), head, text);
    ircSendLine(*c, line);
    return;
  }
  snprintf(head, sizeof(head), ":%s PRIVMSG %s :", fromNick, channel);
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    IRCClient &c = clients[i];
    if (!c.active || !c.registered) continue;
    if (&c == except) continue;
    if (!isInChannel(c, channel)) continue;
    ircBuildTextLine(line, sizeof(line), head, text);
    ircSendLine(c, line);
  }
}

void ircJoinAndAnnounce(IRCClient &c, const char *chan) {
  char line[256];
  if (!joinChannel(c, chan)) {
    ircNumericf(c, 405, "%s :You have joined too many channels", chan);
    return;
  }
  snprintf(line, sizeof(line), ":%s JOIN :%s", c.nick, chan);
  ircSendLine(c, line);
  ircNumericf(c, 331, "%s :No topic is set", chan);
  ircNumericf(c, 353, "= %s :%s", chan, c.nick);
  ircNumericf(c, 366, "%s :End of /NAMES list", chan);
}

bool ircNickValid(const char *nick) {
  size_t len = strlen(nick);
  if (len == 0 || len >= MAX_NICK_LEN) return false;
  char first = nick[0];
  if (!isalpha((unsigned char)first) && strchr("_[]\\^{}|`", first) == NULL) return false;
  for (size_t i = 0; i < len; ++i) {
    char ch = nick[i];
    if (isalnum((unsigned char)ch)) continue;
    if (strchr("_[]\\^{}|`-", ch) != NULL) continue;
    return false;
  }
  return true;
}

bool ircNickInUse(const char *nick, IRCClient *exclude) {
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    IRCClient &c = clients[i];
    if (!c.active || &c == exclude) continue;
    if (c.nick[0] && strcasecmp(c.nick, nick) == 0) return true;
  }
  return false;
}

void ircQuitClient(IRCClient &c, const char *reason, bool sendError) {
  if (!c.active) return;
  if (c.registered) {
    char fullNick[48];
    snprintf(fullNick, sizeof(fullNick), "%s!%s@local", c.nick[0] ? c.nick : "*", c.user[0] ? c.user : "user");
    char quitLine[220];
    snprintf(quitLine, sizeof(quitLine), ":%s QUIT :%s", fullNick, reason ? reason : "Client quit");
    for (uint8_t ci = 0; ci < c.channelCount; ++ci) {
      for (int i = 0; i < MAX_CLIENTS; ++i) {
        IRCClient &other = clients[i];
        if (!other.active || !other.registered || &other == &c) continue;
        if (isInChannel(other, c.channels[ci])) ircSendLine(other, quitLine);
      }
    }
  }
  if (sendError) {
    char errLine[128];
    snprintf(errLine, sizeof(errLine), "ERROR :Closing link: %s", reason ? reason : "Client quit");
    ircSendLine(c, errLine);
  }
  c.conn.stop();
  c.active = false;
  oledDirty = true;
}

void ircWelcome(IRCClient &c) {
  ircNumericf(c, 1, ":Welcome to the Archaeon LoRa-mesh IRC network, %s!%s@local",
              c.nick, c.user[0] ? c.user : "user");
  ircNumericf(c, 2, ":Your host is %s, running on node %s", SERVER_NAME, nodeID);
  ircNumericf(c, 3, ":Mesh traffic is AES-128 encrypted and relayed over LoRa");
  ircNumericf(c, 4, "%s archaeon-mesh-1.0 o o", SERVER_NAME);
  ircNumericf(c, 5, "CHANTYPES=# NICKLEN=%d CHANNELLEN=%d CHANLIMIT=#:%d PREFIX= :are supported by this server",
              MAX_NICK_LEN - 1, MAX_CHAN_LEN - 1, MAX_CHANNELS_PER_CLIENT);
  ircNumericf(c, 375, ":- %s Message of the day -", SERVER_NAME);
  ircNumericf(c, 372, ":- Welcome to the mesh. Traffic on this server also flows over LoRa radio.");
  ircNumericf(c, 376, ":End of /MOTD command");
  ircJoinAndAnnounce(c, DEFAULT_CHANNEL);
}

void ircHandleLine(IRCClient &c, char *line) {
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
  if (len == 0) return;

  c.lastActivity = millis();
  c.awaitingPong = false;

  Serial.print("[IRC ");
  Serial.print(c.nick[0] ? c.nick : "?");
  Serial.print("] ");
  Serial.println(line);

  char *space = strchr(line, ' ');
  char cmd[16];
  if (space) {
    size_t cl = space - line;
    if (cl >= sizeof(cmd)) cl = sizeof(cmd) - 1;
    strncpy(cmd, line, cl);
    cmd[cl] = '\0';
    space++;
  } else {
    strncpy(cmd, line, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    space = line + len;
  }
  for (char *p = cmd; *p; ++p) *p = toupper(*p);

  static const char *PRE_REGISTER_OK[] = { "PASS", "CAP", "NICK", "USER", "PING", "PONG", "QUIT" };
  if (!c.registered) {
    bool allowed = false;
    for (size_t k = 0; k < sizeof(PRE_REGISTER_OK) / sizeof(PRE_REGISTER_OK[0]); ++k) {
      if (strcmp(cmd, PRE_REGISTER_OK[k]) == 0) { allowed = true; break; }
    }
    if (!allowed) {
      ircNumericf(c, 451, ":You have not registered");
      return;
    }
  }

  if (strcmp(cmd, "PASS") == 0) {

  } else if (strcmp(cmd, "CAP") == 0) {
    char *sub = space;
    char subCmd[16];
    sscanf(sub, "%15s", subCmd);
    for (char *p = subCmd; *p; ++p) *p = toupper(*p);
    if (strcmp(subCmd, "LS") == 0) {
      c.capNegotiating = true;
      char line[64];
      snprintf(line, sizeof(line), ":%s CAP * LS :", SERVER_NAME);
      ircSendLine(c, line);
    } else if (strcmp(subCmd, "REQ") == 0) {
      char line[64];
      snprintf(line, sizeof(line), ":%s CAP * NAK :", SERVER_NAME);
      ircSendLine(c, line);
    } else if (strcmp(subCmd, "END") == 0) {
      c.capNegotiating = false;
    }

  } else if (strcmp(cmd, "NICK") == 0) {
    char *n = space;
    if (*n == ':') n++;
    char newNick[MAX_NICK_LEN];
    strncpy(newNick, n, sizeof(newNick) - 1);
    newNick[sizeof(newNick) - 1] = '\0';
    for (char *p = newNick; *p; ++p) if (*p == ' ' || *p == '\r') { *p = '\0'; break; }

    if (!ircNickValid(newNick)) {
      ircNumericf(c, 432, "%s :Erroneous nickname", newNick);
    } else if (ircNickInUse(newNick, &c)) {
      ircNumericf(c, 433, "%s :Nickname is already in use", newNick);
    } else {
      bool hadOldNick = c.registered && c.nick[0];
      char oldFullNick[48];
      if (hadOldNick) snprintf(oldFullNick, sizeof(oldFullNick), "%s!%s@local", c.nick, c.user[0] ? c.user : "user");
      strncpy(c.nick, newNick, sizeof(c.nick) - 1);
      c.nick[sizeof(c.nick) - 1] = '\0';

      if (hadOldNick) {
        char line[96];
        snprintf(line, sizeof(line), ":%s NICK :%s", oldFullNick, c.nick);
        for (uint8_t ci = 0; ci < c.channelCount; ++ci) {
          for (int i = 0; i < MAX_CLIENTS; ++i) {
            IRCClient &other = clients[i];
            if (!other.active || !other.registered) continue;
            if (&other != &c && !isInChannel(other, c.channels[ci])) continue;
            ircSendLine(other, line);
          }
        }
      } else if (c.user[0] && !c.registered) {
        c.registered = true;
        ircWelcome(c);
      }
    }

  } else if (strcmp(cmd, "USER") == 0) {
    char uname[MAX_NICK_LEN];
    sscanf(space, "%23s", uname);
    strncpy(c.user, uname, sizeof(c.user) - 1);
    c.user[sizeof(c.user) - 1] = '\0';
    if (c.nick[0] && !c.registered) { c.registered = true; ircWelcome(c); }

  } else if (strcmp(cmd, "PING") == 0) {
    char pong[256];
    snprintf(pong, sizeof(pong), ":%s PONG %s %s", SERVER_NAME, SERVER_NAME, space);
    ircSendLine(c, pong);

  } else if (strcmp(cmd, "PONG") == 0) {

  } else if (strcmp(cmd, "MODE") == 0) {
    char target[MAX_CHAN_LEN];
    sscanf(space, "%31s", target);
    if (target[0] == '#') {
      ircNumericf(c, 324, "%s +", target);
    } else {
      ircNumericf(c, 221, "+");
    }

  } else if (strcmp(cmd, "WHO") == 0) {
    char target[MAX_CHAN_LEN];
    if (sscanf(space, "%31s", target) == 1 && target[0] == '#') {
      for (int i = 0; i < MAX_CLIENTS; ++i) {
        IRCClient &other = clients[i];
        if (!other.active || !other.registered) continue;
        if (!isInChannel(other, target)) continue;
        ircNumericf(c, 352, "%s %s local %s %s H :0 %s",
                    target, other.user[0] ? other.user : "user", SERVER_NAME, other.nick, other.nick);
      }
      ircNumericf(c, 315, "%s :End of /WHO list", target);
    } else {
      ircNumericf(c, 315, "* :End of /WHO list");
    }

  } else if (strcmp(cmd, "WHOIS") == 0) {
    char target[MAX_NICK_LEN];
    sscanf(space, "%23s", target);
    IRCClient *who = ircFindClientByNick(target);
    if (!who) {
      ircNumericf(c, 401, "%s :No such nick", target);
    } else {
      ircNumericf(c, 311, "%s %s local * :%s", who->nick, who->user[0] ? who->user : "user", who->nick);
      ircNumericf(c, 312, "%s %s :Archaeon mesh node", who->nick, SERVER_NAME);
      ircNumericf(c, 318, "%s :End of /WHOIS list", who->nick);
    }

  } else if (strcmp(cmd, "TOPIC") == 0) {
    char chan[MAX_CHAN_LEN];
    if (sscanf(space, "%31s", chan) == 1 && chan[0]) {
      ircNumericf(c, 331, "%s :No topic is set", chan);
    }

  } else if (strcmp(cmd, "LIST") == 0) {
    ircNumericf(c, 321, "Channel :Users  Name");
    ircNumericf(c, 323, ":End of /LIST");

  } else if (strcmp(cmd, "NOTICE") == 0) {
    char *target = space;
    char *colon = strchr(space, ':');
    char *text = "";
    if (colon) {
      *colon = '\0';
      text = colon + 1;
      char *tend = target + strlen(target);
      while (tend > target && *(tend - 1) == ' ') *(--tend) = '\0';
    }
    if (target[0] && text[0] && target[0] == '#' && isInChannel(c, target)) {
      char fullNick[48];
      snprintf(fullNick, sizeof(fullNick), "%s!%s@local", c.nick, c.user[0] ? c.user : "user");
      char head[80];
      char line[IRC_MAX_LINE];
      snprintf(head, sizeof(head), ":%s NOTICE %s :", fullNick, target);
      ircBuildTextLine(line, sizeof(line), head, text);
      for (int i = 0; i < MAX_CLIENTS; ++i) {
        IRCClient &other = clients[i];
        if (!other.active || !other.registered || &other == &c) continue;
        if (!isInChannel(other, target)) continue;
        ircSendLine(other, line);
      }
    }

  } else if (strcmp(cmd, "JOIN") == 0) {
    char *list = space;
    if (*list == ':') list++;
    char *save;
    char *chan = strtok_r(list, ", ", &save);
    while (chan) {
      if (chan[0]) ircJoinAndAnnounce(c, chan);
      chan = strtok_r(NULL, ", ", &save);
    }

  } else if (strcmp(cmd, "PART") == 0) {
    char *list = space;
    if (*list == ':') list++;
    char *save;
    char *chan = strtok_r(list, ", ", &save);
    while (chan) {
      if (isInChannel(c, chan)) {
        char partLine[96];
        snprintf(partLine, sizeof(partLine), ":%s PART %s", c.nick, chan);
        ircSendLine(c, partLine);
        partChannel(c, chan);
      }
      chan = strtok_r(NULL, ", ", &save);
    }

  } else if (strcmp(cmd, "PRIVMSG") == 0) {
    char *target = space;
    char *colon = strchr(space, ':');
    char *text = "";
    if (colon) {
      *colon = '\0';
      text = colon + 1;
      char *tend = target + strlen(target);
      while (tend > target && *(tend - 1) == ' ') *(--tend) = '\0';
    }
    if (target[0] && text[0]) {
      char fullNick[48];
      snprintf(fullNick, sizeof(fullNick), "%s!%s@local", c.nick, c.user[0] ? c.user : "user");

      if (target[0] == '#') {
        if (!isInChannel(c, target)) {
          ircNumericf(c, 442, "%s :You're not on that channel", target);
        } else {
          ircBroadcastLocal(target, fullNick, text, &c);
          meshPublish(target, c.nick, text);
        }
      } else {
        if (strcasecmp(target, c.nick) == 0) {
          ircNumericf(c, 401, "%s :You can't message yourself", target);
        } else {
          char dmTarget[MAX_CHAN_LEN];
          snprintf(dmTarget, sizeof(dmTarget), "@%s", target);
          ircBroadcastLocal(dmTarget, fullNick, text, &c);
          meshPublish(dmTarget, c.nick, text);
        }
      }
    }

  } else if (strcmp(cmd, "QUIT") == 0) {
    char *reason = space;
    if (*reason == ':') reason++;
    ircQuitClient(c, reason[0] ? reason : "Client quit", true);

  } else {
    ircNumericf(c, 421, "%s :Unknown command", cmd);
  }
}

void ircPollClients() {
  WiFiClient newConn = ircServer.available();
  if (newConn) {
    bool placed = false;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
      if (!clients[i].active) {
        clients[i] = IRCClient();
        clients[i].conn = newConn;
        clients[i].active = true;
        clients[i].lastActivity = millis();
        placed = true;
        oledDirty = true;
        Serial.println("[IRC] New client connected");
        break;
      }
    }
    if (!placed) {
      newConn.print(":" SERVER_NAME " ERROR :Server full\r\n");
      newConn.stop();
    }
  }

  unsigned long now = millis();
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    IRCClient &c = clients[i];
    if (!c.active) continue;
    if (!c.conn.connected()) { ircQuitClient(c, "Connection reset by peer", false); continue; }

    int avail;
    while ((avail = c.conn.available()) > 0) {
      uint8_t chunk[64];
      int n = c.conn.read(chunk, (avail > (int)sizeof(chunk)) ? (int)sizeof(chunk) : avail);
      for (int k = 0; k < n; ++k) {
        char ch = (char)chunk[k];
        if (ch == '\n') {
          c.lineBuf[c.lineIdx] = '\0';
          if (c.lineIdx > 0) ircHandleLine(c, c.lineBuf);
          c.lineIdx = 0;
        } else if (ch != '\r') {
          if (c.lineIdx < MAX_LINE_LEN - 1) c.lineBuf[c.lineIdx++] = ch;
        }
      }
    }
    if (!c.active) continue;

    if (c.awaitingPong) {
      if (now - c.pingSentAt > IRC_PING_GRACE_MS) {
        ircQuitClient(c, "Ping timeout", true);
      }
    } else if (now - c.lastActivity > IRC_PING_INTERVAL_MS) {
      char pingLine[64];
      snprintf(pingLine, sizeof(pingLine), "PING :%s", SERVER_NAME);
      ircSendLine(c, pingLine);
      c.awaitingPong = true;
      c.pingSentAt = now;
    }
  }
}

void updateOLED() {
  unsigned long now = millis();
  if (!oledDirty && (now - lastOledUpdate) < OLED_UPDATE_MS) return;
  lastOledUpdate = now;
  oledDirty = false;

  int activeClients = 0;
  for (int i = 0; i < MAX_CLIENTS; ++i) if (clients[i].active) activeClients++;
  int pendingReassembly = 0;
  for (int i = 0; i < MAX_REASSEMBLY; ++i) if (reassembly[i].active) pendingReassembly++;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextWrap(false);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("Archaeon");
  char rssiBuf[12];
  snprintf(rssiBuf, sizeof(rssiBuf), "%4ddBm", lastRSSI);
  display.setCursor(86, 0);
  display.print(rssiBuf);

  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

  display.setCursor(0, 13);
  display.print("Tx:"); display.print(sentCount);
  display.setCursor(64, 13);
  display.print("Rx:"); display.print(recvCount);

  display.setCursor(0, 23);
  display.print("Q:"); display.print(qCount); display.print("/"); display.print(OUT_QUEUE_SIZE);
  display.setCursor(64, 23);
  display.print("IRC:"); display.print(activeClients); display.print("/"); display.print(MAX_CLIENTS);

  display.setCursor(0, 33);
  display.print("Reasm:"); display.print(pendingReassembly);
  display.setCursor(64, 33);
  display.print("Air:"); display.print(airtimeTxInWindow); display.print("/"); display.print(AIRTIME_MAX_TX_PER_WINDOW);

  display.drawFastHLine(0, 43, SCREEN_WIDTH, SSD1306_WHITE);

  display.setCursor(0, 46);
  display.print("AP: "); display.print(apSSID);

  display.drawFastHLine(0, 56, SCREEN_WIDTH, SSD1306_WHITE);
  display.setCursor(0, 57);
  display.print(apAddrStr);

  display.display();
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(1);
  Serial.println();
  Serial.println("LilyGO T3 - Archaeon Node");

  meshKeyInit();

  pinMode(LED_PIN, OUTPUT);
  ledOff();

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("SSD1306 init failed");
    while (true) delay(1000);
  }
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(4, 6);
  display.print("Mesh IRC");
  display.setTextSize(1);
  display.setCursor(4, 30);
  display.print("Booting...");
  display.display();

  initNodeID();

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_BAND)) {
    Serial.println("LoRa init failed!");
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(0, 20);
    display.print("LoRa FAIL");
    display.display();
    while (true) delay(1000);
  }
  Serial.printf("LoRa init OK @ %.3f MHz\n", LORA_BAND / 1e6);

  WiFi.mode(WIFI_AP);
  if (strlen(AP_PASSWORD) < 8) {
    Serial.println("[WARN] AP_PASSWORD too short for WPA2 (need 8+ chars); AP will be OPEN");
  }
  const char *apPass = (strlen(AP_PASSWORD) >= 8) ? AP_PASSWORD : nullptr;
  WiFi.softAP(apSSID, apPass);
  IPAddress apIP = WiFi.softAPIP();
  snprintf(apAddrStr, sizeof(apAddrStr), "%d.%d.%d.%d:%d",
           apIP[0], apIP[1], apIP[2], apIP[3], IRC_PORT);
  Serial.printf("Wi-Fi AP  SSID:%s  Security:%s  IP:%s\n",
                apSSID, apPass ? "WPA2-PSK" : "OPEN", apAddrStr);

  ircServer.begin();
  Serial.printf("IRC server listening on port %d\n", IRC_PORT);

  qHead = qTail = qCount = 0;
  seenHead = seenCount = 0;
  for (int i = 0; i < MAX_REASSEMBLY; ++i) reassembly[i].active = false;
  randomSeed(esp_random());
  airtimeWindowStart = millis();
  airtimeTxInWindow = 0;

  delay(500);
  oledDirty = true;
}

void loop() {
  unsigned long now = millis();

  if (ledOffTime > 0 && now >= ledOffTime) {
    ledOff();
    ledOffTime = 0;
  }

  ircPollClients();

  unsigned long sendInterval = CHUNK_DELAY_MS + nodeStaggerOffsetMs + random(0, CHUNK_JITTER_MS);
  if (qCount > 0 && airtimeBudgetAvailable() && (now - lastSendTime) >= sendInterval) {
    char msg[MAX_SEND_LEN + 1];
    if (dequeueMessage(msg, sizeof(msg))) {
      sendLoRaStringNonBlocking(msg);
      lastSendTime = now;
    }
  }

  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    ledOn();
    size_t idx = 0;
    const size_t RX_BUF_LEN = sizeof(lastReceivedBuf);
    while (LoRa.available() && idx < (RX_BUF_LEN - 1)) {
      lastReceivedBuf[idx++] = (char)LoRa.read();
    }
    lastReceivedBuf[idx] = '\0';
    lastRSSI = LoRa.packetRssi();
    recvCount++;
    oledDirty = true;

    meshHandleIncoming(lastReceivedBuf);
  }

  updateOLED();
  delay(1);
}
