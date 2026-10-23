/*
  voip.h
  Phone calls between SLIM nodes over WiFi.

  Two kinds of call:
    - VOIP: signaling and audio are UDP packets between the nodes.  Audio is
      G.711 u-law at 8192 samples/sec (exactly 1/4 of Mozzi's 32768 Hz audio
      rate so received audio can be played through Mozzi's DAC output).
    - phone line ("trunk"): for directory entries with no IP address.  The call
      is set up over WiFi (broadcast, so the far node's IP isn't needed) and the
      relay connects the two phones over the copper phone line between nodes.

  Local phone audio is sampled from the SLIC receive path on GPIO32 (ADC1_CH4).
  Far end audio is played on GPIO25 (DAC1) through Mozzi's updateAudio().
*/
#ifndef voip_h
#define voip_h

#include "Arduino.h"

// events reported to the main state machine
enum VoipEvent {
  VOIP_EV_NONE,
  VOIP_EV_INCOMING,     // another node is calling this node (ring the phone)
  VOIP_EV_RINGING,      // the node we called is ringing its phone
  VOIP_EV_BUSY,         // the node we called is busy
  VOIP_EV_ANSWERED,     // call is connected
  VOIP_EV_HANGUP,       // the other node hung up / cancelled the call
  VOIP_EV_TIMEOUT       // the other node stopped responding
};

// audio callbacks used by voicemail
typedef int  (*PcmSourceFn)(int16_t* buf, int n);        // fill up to n samples (8192 Hz), return count (0 = nothing now)
typedef void (*UlawSinkFn)(const uint8_t* data, int n);  // receives u-law samples (8192 Hz)

bool        voipBegin();                          // join WiFi, find own number, open UDP, set up audio
void        voipPoll();                           // call every loop(): receive packets, resend, timeouts
VoipEvent   voipGetEvent();                       // fetch (and clear) the latest event

int         voipLookupNumber(const String& num);  // directory index for a number, or -1
bool        voipIsPrefixOfAnyNumber(const String& digits); // could more digits still match a number?
bool        voipPlaceCall(const String& num);     // start calling a number from the directory
void        voipAnswer();                         // answer the incoming call
void        voipHangup();                         // end / cancel / reject whatever call is in progress
bool        voipCallActive();                     // any call (ringing or connected) in progress
bool        voipCallIsTrunk();                    // current call uses the phone line, not VOIP audio
String      voipPeerNumber();                     // phone number of the other end of the current call
bool        voipInAccessPointMode();              // couldn't join WiFi: running our own access point
String      voipIpAddress();                      // this board's IP address (config page)

// diagnostics
bool        voipStartLoopback(bool radioOff = false); // echo test: hear your own phone through the audio path, no network
bool        voipLineScan();                       // wideband noise scan of the receive audio (serial monitor)

// software DTMF decoder (backs up the MT8870)
void        voipSetDialListen(bool on);           // listen for DTMF on the ADC while dialing / in menus
char        voipGetDtmfDigit();                   // next software-decoded DTMF digit, or 0

// audio hooks for voicemail during a connected VOIP call
void        voipSetTxSource(PcmSourceFn fn);      // send this audio to the far end instead of the microphone (NULL = mic)
void        voipSetRxSink(UlawSinkFn fn);         // also pass audio received from the far end here (NULL = off)

// local audio with no call: play to this phone's earpiece and/or record its microphone
bool        voipLocalStart(PcmSourceFn play, UlawSinkFn mic);
void        voipLocalStop();
int         voipLocalBuffered();                  // samples queued for the earpiece but not played yet

// u-law helpers
uint8_t     voipLinearToUlaw(int32_t pcm);
int16_t     voipUlawToLinear(uint8_t u);

// used by Mozzi's updateAudio() - called at AUDIO_RATE
bool        voipAudioActive();                    // true while VOIP or local audio is playing to the earpiece
int16_t     voipNextPlaybackSample();             // next far-end audio sample (16 bit signed)

#endif
