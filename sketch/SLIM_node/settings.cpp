/*
  settings.cpp
*/
#include "settings.h"
#include "voip_config.h"
#include "arduino_secrets.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

NodeSettings settings;
static Preferences prefs;
static SemaphoreHandle_t lockSem = NULL;

static NodeSettings pending;          // submitted by the web page
static volatile bool pendingReady = false;
static volatile bool pendingReboot = false;

void settingsLock()   { if (lockSem) xSemaphoreTakeRecursive(lockSem, portMAX_DELAY); }
void settingsUnlock() { if (lockSem) xSemaphoreGiveRecursive(lockSem); }

static void defaultDirectory(NodeSettings& s) {
  s.directoryCount = 0;
  for (unsigned i = 0; i < sizeof(phoneDirectory) / sizeof(phoneDirectory[0]) && s.directoryCount < MAX_DIRECTORY; i++) {
    DirectoryEntry& e = s.directory[s.directoryCount++];
    e.number = phoneDirectory[i].number;
    e.ip = phoneDirectory[i].ip;
    e.mac = phoneDirectory[i].mac;
  }
}

String directoryToText(const NodeSettings& s) {
  String t;
  for (int i = 0; i < s.directoryCount; i++) {
    t += s.directory[i].number;
    t += " ";
    t += s.directory[i].ip.length() ? s.directory[i].ip : String("line");
    if (s.directory[i].mac.length()) { t += " "; t += s.directory[i].mac; }
    t += "\n";
  }
  return t;
}

static bool allDigits(const String& x) {
  if (x.length() == 0) return false;
  for (unsigned i = 0; i < x.length(); i++) if (x[i] < '0' || x[i] > '9') return false;
  return true;
}

static bool validIp(const String& x) {
  int dots = 0, num = -1;
  for (unsigned i = 0; i < x.length(); i++) {
    char c = x[i];
    if (c == '.') { if (num < 0 || num > 255) return false; dots++; num = -1; }
    else if (c >= '0' && c <= '9') { num = (num < 0 ? 0 : num * 10) + (c - '0'); if (num > 255) return false; }
    else return false;
  }
  return dots == 3 && num >= 0;
}

// parse "number ip [mac]" lines.  ip can be "line" or "-" for a phone line entry.
bool directoryFromText(NodeSettings& s, const String& text, String& error) {
  NodeSettings t = s;
  t.directoryCount = 0;
  int lineNo = 0;
  unsigned pos = 0;
  while (pos <= text.length()) {
    int nl = text.indexOf('\n', pos);
    String line = nl < 0 ? text.substring(pos) : text.substring(pos, nl);
    pos = nl < 0 ? text.length() + 1 : nl + 1;
    lineNo++;
    line.replace("\r", "");
    line.replace(",", " ");
    line.replace("\t", " ");
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    String f[3];
    int nf = 0;
    unsigned p = 0;
    while (p < line.length() && nf < 4) {
      while (p < line.length() && line[p] == ' ') p++;
      int e = line.indexOf(' ', p);
      String w = e < 0 ? line.substring(p) : line.substring(p, e);
      if (w.length()) { if (nf < 3) f[nf] = w; nf++; }
      p = e < 0 ? line.length() : e;
    }
    if (nf > 3 || !allDigits(f[0]) || f[0].length() > 15) { error = "line " + String(lineNo) + ": expected  number ip [mac]"; return false; }
    if (f[1] == "line" || f[1] == "-") f[1] = "";
    if (f[1].length() && !validIp(f[1])) { error = "line " + String(lineNo) + ": bad IP address " + f[1]; return false; }
    if (t.directoryCount >= MAX_DIRECTORY) { error = "too many entries (max " + String(MAX_DIRECTORY) + ")"; return false; }
    for (int i = 0; i < t.directoryCount; i++)
      if (t.directory[i].number == f[0]) { error = "number " + f[0] + " is listed twice"; return false; }
    DirectoryEntry& d = t.directory[t.directoryCount++];
    d.number = f[0];
    d.ip = f[1];
    d.mac = f[2];
  }
  s = t;
  return true;
}

void settingsBegin() {
  if (!lockSem) lockSem = xSemaphoreCreateRecursiveMutex();
  prefs.begin("slim", false);
  settings.vmRings         = prefs.getUChar("vmRings",  VM_RINGS_DEFAULT);
  settings.reminderMinutes = prefs.getUChar("remind",   VM_REMINDER_MINUTES_DEFAULT);
  settings.batphone        = prefs.getString("bat",     BATPHONE_NUMBER_DEFAULT);
  settings.batphoneDelay   = prefs.getUChar("batDelay", BATPHONE_DELAY_DEFAULT);
  settings.adminPassword   = prefs.getString("admin",   WEB_PASSWORD_DEFAULT);
  for (int i = 0; i < 10; i++) {
    char key[6];
    snprintf(key, sizeof(key), "sd%d", i);
    settings.speedDial[i] = prefs.getString(key, "");
  }
  settings.wifiSsid = prefs.getString("ssid", SECRET_SSID);
  settings.wifiPass = prefs.getString("wpass", SECRET_PASS);
  settings.staticIp = prefs.getString("ip", "");
  settings.gateway  = prefs.getString("gw", VOIP_GATEWAY);
  settings.subnet   = prefs.getString("mask", VOIP_SUBNET);
  settings.dns      = prefs.getString("dns", VOIP_DNS);
  settings.myNumber = prefs.getString("number", MY_PHONE_NUMBER);
  String dirText = prefs.getString("dir", "");
  String err;
  if (dirText.length() == 0 || !directoryFromText(settings, dirText, err)) defaultDirectory(settings);
}

void settingsSave() {
  settingsLock();
  prefs.putUChar("vmRings", settings.vmRings);
  prefs.putUChar("remind", settings.reminderMinutes);
  prefs.putString("bat", settings.batphone);
  prefs.putUChar("batDelay", settings.batphoneDelay);
  prefs.putString("admin", settings.adminPassword);
  for (int i = 0; i < 10; i++) {
    char key[6];
    snprintf(key, sizeof(key), "sd%d", i);
    prefs.putString(key, settings.speedDial[i]);
  }
  prefs.putString("ssid", settings.wifiSsid);
  prefs.putString("wpass", settings.wifiPass);
  prefs.putString("ip", settings.staticIp);
  prefs.putString("gw", settings.gateway);
  prefs.putString("mask", settings.subnet);
  prefs.putString("dns", settings.dns);
  prefs.putString("number", settings.myNumber);
  prefs.putString("dir", directoryToText(settings));
  settingsUnlock();
}

NodeSettings settingsSnapshot() {
  settingsLock();
  NodeSettings copy = settings;
  settingsUnlock();
  return copy;
}

void settingsSubmit(const NodeSettings& s, bool rebootAfter) {
  settingsLock();
  pending = s;
  pendingReboot = rebootAfter;
  pendingReady = true;
  settingsUnlock();
}

void settingsPoll() {
  if (!pendingReady) return;
  settingsLock();
  settings = pending;
  pendingReady = false;
  bool reboot = pendingReboot;
  settingsUnlock();
  settingsSave();
  Serial.println(F("[settings] saved from the web page"));
  if (reboot) {
    Serial.println(F("[settings] restarting to use the new network / phone settings"));
    delay(500);
    ESP.restart();
  }
}

void settingsPrint() {
  Serial.println(F("---- settings ----"));
  if (settings.vmRings) Serial.printf("voicemail answers after %u rings (*5)\r\n", settings.vmRings);
  else Serial.println(F("voicemail off (*5)"));
  if (settings.reminderMinutes) Serial.printf("message waiting reminder ring every %u minutes (*6)\r\n", settings.reminderMinutes);
  else Serial.println(F("message waiting reminder ring off (*6)"));
  if (settings.batphone.length())
    Serial.printf("Batphone: dials %s %s (*7, *8)\r\n", settings.batphone.c_str(),
                  settings.batphoneDelay ? (String("after ") + settings.batphoneDelay + " s of dial tone").c_str() : "as soon as the phone is picked up");
  else Serial.println(F("Batphone off (*7)"));
  for (int i = 0; i < 10; i++)
    if (settings.speedDial[i].length()) Serial.printf("speed dial #%d = %s\r\n", i, settings.speedDial[i].c_str());
  Serial.printf("WiFi network: %s   IP: %s\r\n", settings.wifiSsid.c_str(), settings.staticIp.length() ? settings.staticIp.c_str() : "DHCP");
  Serial.println(F("directory:"));
  Serial.print(directoryToText(settings));
  Serial.println(F("------------------"));
}
