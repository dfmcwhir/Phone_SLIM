/*
  settings.h
  Node settings kept in the ESP32's non-volatile storage so they survive power
  cycles.  First boot uses the defaults from voip_config.h / arduino_secrets.h.
  Changed from the phone with * codes, or from the config web page.

  The main loop owns `settings`.  The web page (another task) never writes it
  directly: it takes a copy with settingsSnapshot() and hands changes back with
  settingsSubmit(); the loop applies them in settingsPoll().
*/
#ifndef settings_h
#define settings_h

#include "Arduino.h"

struct DirectoryEntry {
  String number;
  String ip;        // "" = reached over the phone line
  String mac;       // optional
};

#define MAX_DIRECTORY 32

struct NodeSettings {
  // phone features (apply immediately)
  uint8_t vmRings;          // rings before voicemail answers (0 = voicemail off)
  uint8_t reminderMinutes;  // single ring reminder interval when messages are waiting (0 = off)
  String  speedDial[10];    // #0 - #9
  String  batphone;         // number dialed automatically when the phone is picked up ("" = off)
  uint8_t batphoneDelay;    // seconds of dial tone before the Batphone dials (0 = immediately)
  String  adminPassword;    // config web page and access point password

  // network and numbering (used at boot: change then reboot)
  String  wifiSsid;
  String  wifiPass;
  String  staticIp;         // "" = DHCP
  String  gateway;
  String  subnet;
  String  dns;
  String  myNumber;         // "" = find this board in the directory by MAC / IP
  DirectoryEntry directory[MAX_DIRECTORY];
  int     directoryCount;
};

extern NodeSettings settings;

void settingsBegin();               // load (or create from defaults)
void settingsSave();                // call with the lock held if other tasks may be reading
void settingsPrint();               // print everything on the serial monitor

// sharing with the web page task
void settingsLock();
void settingsUnlock();
NodeSettings settingsSnapshot();                     // copy for the web page
void settingsSubmit(const NodeSettings& s, bool rebootAfter);   // web page: apply these
void settingsPoll();                                 // main loop: apply submitted settings

// directory as text, one entry per line: "number ip [mac]" (no ip = phone line)
String directoryToText(const NodeSettings& s);
bool   directoryFromText(NodeSettings& s, const String& text, String& error);

#endif
