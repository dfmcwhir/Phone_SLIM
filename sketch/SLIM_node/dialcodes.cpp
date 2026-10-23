/*
  dialcodes.cpp
*/
#include "dialcodes.h"
#include "settings.h"
#include "voip.h"
#include "voicemail.h"
#include "project_globals.h"     // localPhoneNumber

static DialResult result(DialKind k, const String& n = "") {
  DialResult r;
  r.kind = k;
  r.number = n;
  return r;
}

static bool allDigits(const String& s) {
  for (unsigned i = 0; i < s.length(); i++) if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

// a number that can be called: in the directory, or this phone (voicemail)
static bool callable(const String& n) {
  return voipLookupNumber(n) >= 0 || (localPhoneNumber.length() && n == localPhoneNumber);
}

static DialResult classifyNumber(const String& n) {
  if (localPhoneNumber.length() && n == localPhoneNumber) return result(DK_VOICEMAIL);
  if (voipLookupNumber(n) >= 0) return result(DK_CALL, n);
  if (voipIsPrefixOfAnyNumber(n) || (localPhoneNumber.length() && localPhoneNumber.startsWith(n)))
    return result(DK_INCOMPLETE);
  return result(DK_INVALID);
}

static DialResult starCode(const String& d) {
  if (d.length() < 2) return result(DK_INCOMPLETE);
  char c = d[1];
  String rest = d.substring(2);
  bool ended = rest.endsWith("#");
  String body = ended ? rest.substring(0, rest.length() - 1) : rest;
  if (body.indexOf('#') >= 0 || body.indexOf('*') >= 0) return result(DK_INVALID);

  switch (c) {
    case '0': return result(DK_ECHO_RADIO_OFF);
    case '1': return result(DK_SCAN);
    case '3': return result(DK_ECHO);
    case '4': return result(DK_VOICEMAIL);
    case '9':
      settingsPrint();
      return result(DK_SETTING_OK);

    case '2': {                                  // *2 N number #
      if (rest.length() == 0) return result(DK_INCOMPLETE);
      if (!ended) return result(DK_INCOMPLETE);
      if (body.length() < 1 || !allDigits(body)) return result(DK_SETTING_ERROR);
      int slot = body[0] - '0';
      String num = body.substring(1);
      if (num.length() && !callable(num)) return result(DK_SETTING_ERROR);
      settingsLock();
      settings.speedDial[slot] = num;
      settingsSave();
      settingsUnlock();
      Serial.printf("[settings] speed dial #%d %s %s\r\n", slot, num.length() ? "=" : "cleared", num.c_str());
      return result(DK_SETTING_OK);
    }

    case '5': {                                  // *5 N
      if (rest.length() == 0) return result(DK_INCOMPLETE);
      if (rest.length() != 1 || !allDigits(rest)) return result(DK_SETTING_ERROR);
      settingsLock();
      settings.vmRings = rest[0] - '0';
      settingsSave();
      settingsUnlock();
      if (settings.vmRings) Serial.printf("[settings] voicemail answers after %u rings\r\n", settings.vmRings);
      else Serial.println(F("[settings] voicemail off"));
      return result(DK_SETTING_OK);
    }

    case '6': {                                  // *6 minutes #
      if (!ended) return result(DK_INCOMPLETE);
      if (body.length() < 1 || body.length() > 3 || !allDigits(body)) return result(DK_SETTING_ERROR);
      int m = body.toInt();
      if (m > 255) return result(DK_SETTING_ERROR);
      settingsLock();
      settings.reminderMinutes = (uint8_t)m;
      settingsSave();
      settingsUnlock();
      if (m) Serial.printf("[settings] message waiting reminder every %d minutes\r\n", m);
      else Serial.println(F("[settings] message waiting reminder off"));
      return result(DK_SETTING_OK);
    }

    case '7': {                                  // *7 number #
      if (!ended) return result(DK_INCOMPLETE);
      if (body.length() && (!allDigits(body) || voipLookupNumber(body) < 0 || body == localPhoneNumber))
        return result(DK_SETTING_ERROR);
      settingsLock();
      settings.batphone = body;
      settingsSave();
      settingsUnlock();
      if (body.length()) Serial.printf("[settings] Batphone: picking up dials %s\r\n", body.c_str());
      else Serial.println(F("[settings] Batphone off"));
      return result(DK_SETTING_OK);
    }

    case '8': {                                  // *8 N
      if (rest.length() == 0) return result(DK_INCOMPLETE);
      if (rest.length() != 1 || !allDigits(rest)) return result(DK_SETTING_ERROR);
      settingsLock();
      settings.batphoneDelay = rest[0] - '0';
      settingsSave();
      settingsUnlock();
      Serial.printf("[settings] Batphone dials after %u seconds of dial tone\r\n", settings.batphoneDelay);
      return result(DK_SETTING_OK);
    }

    default:
      return result(DK_INVALID);
  }
}

DialResult classifyDialed(const String& d) {
  if (d.length() == 0) return result(DK_INCOMPLETE);
  if (d.length() > 24) return result(DK_INVALID);
  if (d[0] == '*') return starCode(d);
  if (d[0] == '#') {                             // speed dial
    if (d.length() < 2) return result(DK_INCOMPLETE);
    if (d.length() != 2 || d[1] < '0' || d[1] > '9') return result(DK_INVALID);
    String n = settings.speedDial[d[1] - '0'];
    if (n.length() == 0) {
      Serial.printf("[dial] speed dial #%c is not set\r\n", d[1]);
      return result(DK_INVALID);
    }
    Serial.printf("[dial] speed dial #%c -> %s\r\n", d[1], n.c_str());
    DialResult r = classifyNumber(n);
    return r.kind == DK_INCOMPLETE ? result(DK_INVALID) : r;
  }
  if (!allDigits(d)) return result(DK_INVALID);
  return classifyNumber(d);
}
