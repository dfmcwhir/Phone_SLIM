/*
  webconfig.cpp
  Config web page.  Every page needs the admin password (HTTP basic auth,
  user "admin").  This keeps casual visitors out on your own network; it is
  not strong security (plain HTTP).

  The page task never touches the main loop's data directly: settings are
  copied with settingsSnapshot() and changes handed back with settingsSubmit();
  voicemail deletes are queued with vmRequestDelete().
*/
#include "webconfig.h"
#include "settings.h"
#include "voip.h"
#include "voip_config.h"
#include "voicemail.h"
#include "project_globals.h"     // localPhoneNumber
#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static WebServer server(80);

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static String esc(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

static bool allDigits(const String& x) {
  if (x.length() == 0) return false;
  for (unsigned i = 0; i < x.length(); i++) if (x[i] < '0' || x[i] > '9') return false;
  return true;
}

static bool validIp(const String& x) {
  IPAddress ip;
  int dots = 0;
  for (unsigned i = 0; i < x.length(); i++) if (x[i] == '.') dots++;
  return dots == 3 && ip.fromString(x.c_str());
}

static bool inDirectory(const NodeSettings& s, const String& n) {
  for (int i = 0; i < s.directoryCount; i++) if (s.directory[i].number == n) return true;
  return false;
}

static bool checkAuth() {
  NodeSettings s = settingsSnapshot();
  if (server.authenticate("admin", s.adminPassword.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Phone SLIM", "Password required");
  return false;
}

static void redirectHome(const String& msg) {
  String q;
  const char* hex = "0123456789ABCDEF";
  for (unsigned i = 0; i < msg.length(); i++) {
    char c = msg[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.') q += c;
    else { q += '%'; q += hex[(c >> 4) & 15]; q += hex[c & 15]; }
  }
  server.sendHeader("Location", String("/?m=") + q);
  server.send(303, "text/plain", "");
}

static const char CSS[] PROGMEM = R"css(
<style>
:root{--bg:#f4f1ea;--card:#fff;--ink:#222;--mute:#666;--line:#ddd;--acc:#2f4468;--ok:#2e7d32;--bad:#b3261e}
@media (prefers-color-scheme:dark){:root{--bg:#1b1c1e;--card:#26282b;--ink:#eee;--mute:#aaa;--line:#3a3c40;--acc:#8fa8d8}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,sans-serif}
header{background:var(--acc);color:#fff;padding:14px 16px}header h1{margin:0;font-size:20px}header p{margin:2px 0 0;opacity:.85}
main{max-width:720px;margin:0 auto;padding:12px 16px 40px}
section{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px 14px;margin:12px 0}
h2{font-size:16px;margin:0 0 8px}label{display:block;margin:8px 0 2px;color:var(--mute);font-size:13px}
input,select,textarea{width:100%;padding:7px 8px;border:1px solid var(--line);border-radius:5px;background:var(--bg);color:var(--ink);font:inherit}
textarea{font-family:ui-monospace,monospace;min-height:120px}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.sd{display:grid;grid-template-columns:repeat(5,1fr);gap:8px}
button{margin-top:10px;padding:8px 14px;border:0;border-radius:5px;background:var(--acc);color:#fff;font:inherit;cursor:pointer}
button.del{background:var(--bad);padding:4px 10px;margin:0}
.note{color:var(--mute);font-size:13px}.msg{padding:8px 12px;border-radius:6px;background:var(--ok);color:#fff}.err{background:var(--bad)}
table{width:100%;border-collapse:collapse}td{padding:6px 4px;border-top:1px solid var(--line);vertical-align:middle}audio{width:100%;height:32px}
dl{display:grid;grid-template-columns:auto 1fr;gap:2px 12px;margin:0}dt{color:var(--mute)}dd{margin:0}
@media (max-width:520px){.row{grid-template-columns:1fr}.sd{grid-template-columns:repeat(2,1fr)}}
</style>)css";

static String pageStart(const String& title) {
  String h = F("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>");
  h += esc(title);
  h += F("</title>");
  h += FPSTR(CSS);
  h += F("</head><body>");
  return h;
}

// ---------------------------------------------------------------------------
// voicemail list straight from the flash directory
// ---------------------------------------------------------------------------
struct WebMsg { uint16_t id; bool isNew; String from; String file; uint32_t bytes; };

static int listMessages(WebMsg* out, int max) {
  int n = 0;
  File dir = LittleFS.open("/vm");
  if (!dir || !dir.isDirectory()) return 0;
  for (File f = dir.openNextFile(); f && n < max; f = dir.openNextFile()) {
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    uint32_t size = f.size();
    f.close();
    if (name.length() < 10 || (name[0] != 'N' && name[0] != 'O') || !name.endsWith(".ul") || name[6] != '_') continue;
    out[n].isNew = name[0] == 'N';
    out[n].id = (uint16_t)name.substring(1, 6).toInt();
    out[n].from = name.substring(7, name.length() - 3);
    out[n].file = name;
    out[n].bytes = size;
    n++;
  }
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && out[j].id < out[j - 1].id; j--) { WebMsg t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
  return n;
}

// a stored file name from the browser: only our own pattern, no paths
static bool safeName(const String& n) {
  if (n == "greet.ul") return true;
  if (n.length() < 10 || n.length() > 30 || (n[0] != 'N' && n[0] != 'O') || !n.endsWith(".ul")) return false;
  for (unsigned i = 1; i < n.length() - 3; i++) if (!((n[i] >= '0' && n[i] <= '9') || n[i] == '_' || (n[i] >= 'a' && n[i] <= 'z'))) return false;
  return true;
}

// stream a u-law recording as a 16 bit WAV so any browser can play it
static void handleAudio() {
  if (!checkAuth()) return;
  String name = server.arg("f");
  if (!safeName(name)) { server.send(400, "text/plain", "bad name"); return; }
  File f = LittleFS.open(String("/vm/") + name, "r");
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  uint32_t n = f.size();
  uint8_t hdr[44];
  uint32_t dataBytes = n * 2, rate = 8192, byteRate = rate * 2;
  memcpy(hdr, "RIFF", 4);
  uint32_t riff = 36 + dataBytes; memcpy(hdr + 4, &riff, 4);
  memcpy(hdr + 8, "WAVEfmt ", 8);
  uint32_t fmtLen = 16; memcpy(hdr + 16, &fmtLen, 4);
  uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
  memcpy(hdr + 20, &pcm, 2); memcpy(hdr + 22, &ch, 2);
  memcpy(hdr + 24, &rate, 4); memcpy(hdr + 28, &byteRate, 4);
  memcpy(hdr + 32, &align, 2); memcpy(hdr + 34, &bits, 2);
  memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &dataBytes, 4);

  server.setContentLength(sizeof(hdr) + dataBytes);
  server.sendHeader("Content-Disposition", String("inline; filename=\"") + name.substring(0, name.length() - 3) + ".wav\"");
  server.send(200, "audio/wav", "");
  WiFiClient client = server.client();
  client.write(hdr, sizeof(hdr));
  uint8_t in[256];
  int16_t out[256];
  int got;
  while ((got = f.read(in, sizeof(in))) > 0) {
    for (int i = 0; i < got; i++) out[i] = voipUlawToLinear(in[i]);
    if (client.write((const uint8_t*)out, got * 2) == 0) break;   // browser went away
  }
  f.close();
}

// ---------------------------------------------------------------------------
// main page
// ---------------------------------------------------------------------------
static void handleRoot() {
  if (!checkAuth()) return;
  NodeSettings s = settingsSnapshot();
  bool ap = voipInAccessPointMode();
  String h = pageStart("Phone SLIM " + localPhoneNumber);

  h += F("<header><h1>Phone SLIM ");
  h += esc(localPhoneNumber.length() ? localPhoneNumber : String("(no number)"));
  h += F("</h1><p>");
  h += ap ? F("Access point mode: couldn't join the WiFi network") : F("Connected to WiFi");
  h += F("</p></header><main>");

  if (server.hasArg("m")) {
    String m = server.arg("m");
    bool err = m.startsWith("Error");
    h += "<p class='msg" + String(err ? " err" : "") + "'>" + esc(m) + "</p>";
  }

  // ---- status ----
  size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  h += F("<section><h2>Status</h2><dl>");
  h += "<dt>Number</dt><dd>" + esc(localPhoneNumber) + "</dd>";
  h += "<dt>IP address</dt><dd>" + voipIpAddress() + (s.staticIp.length() ? " (static)" : " (DHCP)") + "</dd>";
  if (ap) h += "<dt>Access point</dt><dd>" + esc(WiFi.softAPSSID()) + "</dd>";
  else h += "<dt>WiFi</dt><dd>" + esc(WiFi.SSID()) + " (" + String(WiFi.RSSI()) + " dBm)</dd>";
  h += "<dt>MAC</dt><dd>" + WiFi.macAddress() + "</dd>";
  h += "<dt>Up</dt><dd>" + String(millis() / 3600000UL) + " h " + String((millis() / 60000UL) % 60) + " min</dd>";
  h += "<dt>Storage</dt><dd>about " + String((unsigned)((total > used ? total - used : 0) / 8192)) + " seconds of messages free</dd>";
  h += F("</dl></section>");

  // ---- voicemail ----
  static WebMsg msgs[VM_MAX_MESSAGES + 4];
  int n = listMessages(msgs, VM_MAX_MESSAGES + 4);
  h += F("<section><h2>Voicemail</h2>");
  if (n == 0) h += F("<p class=note>No messages.</p>");
  else {
    h += F("<table>");
    for (int i = 0; i < n; i++) {
      h += "<tr><td>#" + String(msgs[i].id) + "<br><span class=note>" + esc(msgs[i].from) + ", " +
           String((unsigned)(msgs[i].bytes / 8192)) + " s" + (msgs[i].isNew ? ", <b>new</b>" : "") + "</span></td>";
      h += "<td style='width:60%'><audio controls preload=none src='/audio?f=" + esc(msgs[i].file) + "'></audio></td>";
      h += "<td><form method=post action=/msgdel><input type=hidden name=id value=" + String(msgs[i].id) +
           "><button class=del>Delete</button></form></td></tr>";
    }
    h += F("</table>");
  }
  bool customGreeting = LittleFS.exists("/vm/greet.ul");
  h += F("<p><b>Greeting:</b> ");
  if (customGreeting) {
    h += F("recorded on the phone.</p><audio controls preload=none src='/audio?f=greet.ul'></audio>"
           "<form method=post action=/greetdel><button class=del>Use the built in greeting</button></form>");
  } else {
    h += F("built in.  Record your own from the phone: dial *4 then 5.</p>");
  }
  h += F("</section>");

  // ---- phone features ----
  h += F("<section><h2>Phone settings</h2><form method=post action=/phone><div class=row><div><label>Voicemail answers after</label><select name=vm>");
  for (int i = 0; i <= 9; i++)
    h += "<option value=" + String(i) + (s.vmRings == i ? " selected" : "") + ">" + (i ? String(i) + " rings" : String("off")) + "</option>";
  h += F("</select></div><div><label>Message waiting reminder ring every (minutes, 0 = off)</label>");
  h += "<input name=remind type=number min=0 max=255 value=" + String(s.reminderMinutes) + "></div></div>";
  h += F("<div class=row><div><label>Batphone: picking up dials</label><select name=bat><option value=''>off (normal phone)</option>");
  for (int i = 0; i < s.directoryCount; i++) {
    if (s.directory[i].number == localPhoneNumber) continue;
    h += "<option" + String(s.batphone == s.directory[i].number ? " selected" : "") + ">" + esc(s.directory[i].number) + "</option>";
  }
  h += F("</select></div><div><label>Batphone dial tone first (seconds)</label><select name=batdelay>");
  for (int i = 0; i <= 9; i++)
    h += "<option" + String(s.batphoneDelay == i ? " selected" : "") + ">" + String(i) + "</option>";
  h += F("</select></div></div><label>Speed dial (dial #N)</label><div class=sd>");
  for (int i = 0; i < 10; i++)
    h += "<div><span class=note>#" + String(i) + "</span><input name=sd" + String(i) + " inputmode=numeric value='" + esc(s.speedDial[i]) + "'></div>";
  h += F("</div><button>Save phone settings</button></form></section>");

  // ---- numbers ----
  h += F("<section><h2>Numbers</h2><form method=post action=/numbers><label>This phone's number (blank = find it in the directory by IP or MAC)</label>");
  h += "<input name=number inputmode=numeric value='" + esc(s.myNumber) + "'>";
  h += F("<label>Directory: one line per phone,  <code>number ip</code>  (use <code>line</code> instead of an IP for a phone reached over the phone line).  Copy the same list to every board.</label>");
  h += "<textarea name=dir spellcheck=false>" + esc(directoryToText(s)) + "</textarea>";
  h += F("<button>Save and restart</button></form></section>");

  // ---- network ----
  h += F("<section><h2>WiFi network</h2><form method=post action=/network><div class=row><div><label>Network name (SSID) &nbsp;<a href=/scan>scan</a></label>");
  String ssid = server.hasArg("ssid") ? server.arg("ssid") : s.wifiSsid;
  h += "<input name=ssid value='" + esc(ssid) + "'></div>";
  h += F("<div><label>Password (blank = keep the current one)</label><input name=pass type=password autocomplete=new-password></div></div>");
  h += F("<label>IP address</label><select name=mode onchange=\"document.getElementById('st').style.display=this.value=='s'?'':'none'\">");
  h += "<option value=d" + String(s.staticIp.length() ? "" : " selected") + ">Automatic (DHCP)</option>";
  h += "<option value=s" + String(s.staticIp.length() ? " selected" : "") + ">Static</option></select>";
  h += "<div id=st" + String(s.staticIp.length() ? "" : " style='display:none'") + "><div class=row>";
  h += "<div><label>IP address</label><input name=ip value='" + esc(s.staticIp) + "'></div>";
  h += "<div><label>Gateway (router)</label><input name=gw value='" + esc(s.gateway) + "'></div>";
  h += "<div><label>Subnet mask</label><input name=mask value='" + esc(s.subnet) + "'></div>";
  h += "<div><label>DNS</label><input name=dns value='" + esc(s.dns) + "'></div></div></div>";
  h += F("<button>Save and restart</button><p class=note>If the board can't join this network it starts its own access point (PhoneSLIM-xxxx, password = web password) with this page at http://192.168.4.1/</p></form></section>");

  // ---- admin ----
  h += F("<section><h2>Web password</h2><form method=post action=/admin><div class=row>"
         "<div><label>New password (8 or more characters)</label><input name=p1 type=password autocomplete=new-password></div>"
         "<div><label>Again</label><input name=p2 type=password autocomplete=new-password></div></div>"
         "<button>Change password</button><p class=note>User name is <b>admin</b>.  This is also the access point password.</p></form></section>");

  h += F("<section><h2>Restart</h2><form method=post action=/reboot><button>Restart the board</button></form></section>");
  h += F("</main></body></html>");
  server.send(200, "text/html", h);
}

// ---------------------------------------------------------------------------
// form handlers
// ---------------------------------------------------------------------------
static void handlePhone() {
  if (!checkAuth()) return;
  NodeSettings s = settingsSnapshot();
  int vm = server.arg("vm").toInt();
  int rem = server.arg("remind").toInt();
  int bd = server.arg("batdelay").toInt();
  String bat = server.arg("bat");
  if (vm < 0 || vm > 9 || rem < 0 || rem > 255 || bd < 0 || bd > 9) { redirectHome("Error: value out of range"); return; }
  if (bat.length() && !inDirectory(s, bat)) { redirectHome("Error: Batphone number isn't in the directory"); return; }
  for (int i = 0; i < 10; i++) {
    String v = server.arg("sd" + String(i));
    v.trim();
    if (v.length() && (!allDigits(v) || (!inDirectory(s, v) && v != localPhoneNumber))) {
      redirectHome("Error: speed dial #" + String(i) + " (" + v + ") isn't in the directory");
      return;
    }
    s.speedDial[i] = v;
  }
  s.vmRings = vm;
  s.reminderMinutes = rem;
  s.batphone = bat;
  s.batphoneDelay = bd;
  settingsSubmit(s, false);
  redirectHome("Phone settings saved");
}

static void restartingPage(const String& where) {
  String h = pageStart("Restarting");
  h += F("<header><h1>Saved</h1><p>Restarting...</p></header><main><section><p>The board is restarting with the new settings.</p>");
  h += "<p>Reconnect to it at <a href='" + where + "'>" + esc(where) + "</a> in about 20 seconds.</p>";
  h += F("<p class=note>If it can't join the WiFi network it will start its own access point again.</p></section></main></body></html>");
  server.send(200, "text/html", h);
}

static void handleNumbers() {
  if (!checkAuth()) return;
  NodeSettings s = settingsSnapshot();
  String num = server.arg("number");
  num.trim();
  if (num.length() && (!allDigits(num) || num.length() > 15)) { redirectHome("Error: the phone number must be digits only"); return; }
  String err;
  if (!directoryFromText(s, server.arg("dir"), err)) { redirectHome("Error: " + err); return; }
  s.myNumber = num;
  settingsSubmit(s, true);
  String host = num.length() ? num : localPhoneNumber;
  restartingPage(String("http://") + voipIpAddress() + "/");
  (void)host;
}

static void handleNetwork() {
  if (!checkAuth()) return;
  NodeSettings s = settingsSnapshot();
  String ssid = server.arg("ssid");
  ssid.trim();
  if (ssid.length() == 0 || ssid.length() > 32) { redirectHome("Error: enter the WiFi network name"); return; }
  String pass = server.arg("pass");
  if (pass.length() && (pass.length() < 8 || pass.length() > 63)) { redirectHome("Error: WiFi passwords are 8 to 63 characters"); return; }
  bool isStatic = server.arg("mode") == "s";
  String ip = server.arg("ip"), gw = server.arg("gw"), mask = server.arg("mask"), dns = server.arg("dns");
  ip.trim(); gw.trim(); mask.trim(); dns.trim();
  if (isStatic && (!validIp(ip) || !validIp(gw) || !validIp(mask) || (dns.length() && !validIp(dns)))) {
    redirectHome("Error: check the static IP address, gateway, mask and DNS");
    return;
  }
  if (ssid != s.wifiSsid || pass.length()) s.wifiPass = pass;   // new network: use the password typed (may be open)
  s.wifiSsid = ssid;
  s.staticIp = isStatic ? ip : String("");
  if (isStatic) { s.gateway = gw; s.subnet = mask; s.dns = dns.length() ? dns : gw; }
  settingsSubmit(s, true);
  String where = isStatic ? String("http://") + ip + "/" :
                 (localPhoneNumber.length() ? String("http://slim-") + localPhoneNumber + ".local/" : String("the address your router gives it"));
  restartingPage(where);
}

static void handleAdmin() {
  if (!checkAuth()) return;
  String p1 = server.arg("p1"), p2 = server.arg("p2");
  if (p1 != p2) { redirectHome("Error: the two passwords don't match"); return; }
  if (p1.length() < 8 || p1.length() > 63) { redirectHome("Error: use 8 to 63 characters"); return; }
  NodeSettings s = settingsSnapshot();
  s.adminPassword = p1;
  settingsSubmit(s, false);
  redirectHome("Password changed - your browser will ask for it again");
}

static void handleMsgDel() {
  if (!checkAuth()) return;
  int id = server.arg("id").toInt();
  if (id > 0 && id < 65535) vmRequestDelete((uint16_t)id);
  delay(150);                                    // let the main loop delete it before the page reloads
  redirectHome("Message deleted");
}

static void handleGreetDel() {
  if (!checkAuth()) return;
  vmRequestDeleteGreeting();
  delay(150);
  redirectHome("Using the built in greeting");
}

static void handleReboot() {
  if (!checkAuth()) return;
  restartingPage(String("http://") + voipIpAddress() + "/");
  delay(500);
  ESP.restart();
}

static void handleScan() {
  if (!checkAuth()) return;
  int n = WiFi.scanNetworks();
  String h = pageStart("WiFi networks");
  h += F("<header><h1>WiFi networks</h1><p>Pick one, then enter its password</p></header><main><section><table>");
  if (n <= 0) h += F("<tr><td>No networks found.</td></tr>");
  for (int i = 0; i < n && i < 30; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    String q = ssid;
    q.replace("%", "%25"); q.replace("&", "%26"); q.replace("#", "%23"); q.replace("+", "%2B"); q.replace(" ", "%20");
    h += "<tr><td><a href='/?ssid=" + esc(q) + "#'>" + esc(ssid) + "</a></td><td class=note>" + String(WiFi.RSSI(i)) + " dBm" +
         (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? ", open" : "") + "</td></tr>";
  }
  WiFi.scanDelete();
  h += F("</table><p><a href=/>Back</a></p></section></main></body></html>");
  server.send(200, "text/html", h);
}

// captive portal / unknown pages: go to the config page
static void handleNotFound() {
  server.sendHeader("Location", String("http://") + voipIpAddress() + "/");
  server.send(302, "text/plain", "");
}

static void webTask(void*) {
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void webBegin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/audio", HTTP_GET, handleAudio);
  server.on("/phone", HTTP_POST, handlePhone);
  server.on("/numbers", HTTP_POST, handleNumbers);
  server.on("/network", HTTP_POST, handleNetwork);
  server.on("/admin", HTTP_POST, handleAdmin);
  server.on("/msgdel", HTTP_POST, handleMsgDel);
  server.on("/greetdel", HTTP_POST, handleGreetDel);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/scan", HTTP_GET, handleScan);
  server.onNotFound(handleNotFound);
  server.begin();
  xTaskCreatePinnedToCore(webTask, "web", 8192, NULL, 2, NULL, 0);
  Serial.printf("Config page: http://%s/  (user admin)\r\n", voipIpAddress().c_str());
  if (!voipInAccessPointMode() && localPhoneNumber.length())
    Serial.printf("         or: http://slim-%s.local/\r\n", localPhoneNumber.c_str());
}
