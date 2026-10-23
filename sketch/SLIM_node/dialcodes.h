/*
  dialcodes.h
  Works out what the digits dialed so far mean.

    <number>        call a number in the phone directory
    <own number>    voicemail (also works from rotary/pulse phones)
    *4              voicemail
    #N              speed dial N (0-9)
    *2 N <number> # store speed dial N          *2 N #  clears it
    *5 N            voicemail answers after N rings (*50 = voicemail off)
    *6 <minutes> #  message waiting reminder ring every <minutes> (*60# = off)
    *7 <number> #   Batphone: dial <number> when the phone is picked up   *7# = off
    *8 N            Batphone waits N seconds of dial tone first (*80 = immediately)
    *9              print settings and messages on the serial monitor
    *3              echo test             *0  echo test with WiFi off
    *1              line noise scan (serial monitor)
*/
#ifndef dialcodes_h
#define dialcodes_h

#include "Arduino.h"

enum DialKind {
  DK_INCOMPLETE,     // keep dialing
  DK_INVALID,        // not a number or code
  DK_CALL,           // call result.number
  DK_VOICEMAIL,
  DK_ECHO,
  DK_ECHO_RADIO_OFF,
  DK_SCAN,
  DK_SETTING_OK,     // a setting was changed (or printed)
  DK_SETTING_ERROR   // a setting code with a bad value
};

struct DialResult {
  DialKind kind;
  String   number;   // DK_CALL: number to call
};

DialResult classifyDialed(const String& digits);   // changes settings when a setting code completes

#endif
