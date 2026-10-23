/*
    Plain Old Telephone Service (POTS) simulator
    Uses 30 pin ESP32 DOIT Devkit V1 module
    Choose Arduino IDE board "DOIT ESP32 DEVKIT V1"

    Uses a subscriber line interface circuit (SLIC) module KS0835F
    Can generate DTMF call progress tones (selectable regions) using mozzi audio library
    Can play back "This is a recording" audio sample stored in program memory using mozzi

    Calls between nodes are made over WiFi (VOIP), or over the phone line
    between two boards for directory entries with no IP address.  Every node joins the same WiFi
    router.  The number dialed is looked up in the phone directory (voip_config.h)
    to find the IP address of the node to call, and the call audio is sent between
    nodes as UDP packets:
      - local phone audio is sampled from the SLIC on GPIO32 (ADC1)
      - far end audio is played to the SLIC on GPIO25 (DAC1) through mozzi

    Some classes and function concepts were borrowed from a similar project:
    https://github.com/ktownsend-personal/RetroPhone


   Libraries:   RGBLED https://github.com/wilmouths/RGBLed    (install library by adding zip file downloaded from github
                                                                click "code" button and download zip)
                Mozzi sound library https://github.com/sensorium/Mozzi

                Mozzi documentation/API https://sensorium.github.io/Mozzi/doc/html/index.html

Tested with Arduino IDE 1.8.13
               ESP32 board file 1.0.6 / 2.0.17   (not 3.x)
               Mozzi 1.1.0

   Settings:    WiFi SSID/password      arduino_secrets.h
                phone numbers/IPs       config web page (browse to the board's IP), defaults in voip_config.h
                voicemail, speed dial,  * codes dialed from the phone (see dialcodes.h / README)
                Batphone
                region (tones)          project_globals.cpp


   Gadget Reboot
   https://www.youtube.com/@gadgetreboot
*/

#define DEBUG                     // uncomment to allow debug serial messages
#include "DebugUtils.h"
#include "project_globals.h"      // global variables that get referenced in multiple files
#include "miscFunctions.h"        // some things kept here to de-clutter main file
#include "voip.h"                 // calls between nodes over WiFi
#include "voicemail.h"            // voicemail stored in flash
#include "settings.h"             // settings changed from the phone
#include "dialcodes.h"            // what the dialed digits mean
#include "webconfig.h"            // config web page
#include "pulseHandler.h"         // pulse dialing handler
#include "mozzi_call_progress.h"  // class to generate region specific call progress tones
#include <RGBLed.h>               // RGB LED library for 4 pin LED

void maybeDialingStartedCallback();
void digitReceivedCallback(char digit);
void runStateMachine();
void addDialedDigit(char c, bool fromSoftware);

// pulse dial started callback
void dialingStartedCallback(bool isTone) {
  //
}

auto pulser  = pulseHandler(slic_SHK, dialingStartedCallback);  // pulse dial handler

// create call progress tone generator based on specified geographical region
mozzi_call_progress callProgressGen(myRegion);

// SHK interrupt routine (phone hook switch detection)
void IRAM_ATTR isrSHK() {
  SHK_irqFlag = true;             // set irq flag so it can be handled in main loop
}

// DTMF interrupt routine
void IRAM_ATTR isrDTMF() {
  DTMF_irqFlag = true;            // set irq flag so it can be handled in main loop
}

void setup() {
  Serial.begin(115200);
  Serial.println("\r\nSubscriber Line Interface Module");

  digitalWrite(relayPin, LOW);              // tip/ring trunk relay: only closed for phone line calls
  pinMode(relayPin, OUTPUT);

  // configure gpio
  pinMode(LED_BUILTIN, OUTPUT);             // show wifi connection status on ESP LED

  pulser.setMaybeCallback(maybeDialingStartedCallback);
  pulser.setDigitCallback(digitReceivedCallback);
  pulser.start();                                          // prepare pulse dial handler

  initSlic();                       // configure SLIC control pins and phone ring cadence based on set region
  initSystem();                     // init the system and set status flags
  initMT8870();                     // configure hardware dtmf decoder pins

  settingsBegin();                  // settings changed from the phone (* codes)
  voipBegin();                      // join WiFi, learn this phone's number, prepare call audio
  vmBegin();                        // voicemail storage
  settingsPrint();
  webBegin();                       // config web page at this board's IP address

  attachInterrupt(digitalPinToInterrupt(slic_SHK), isrSHK, CHANGE);  // SLIC hook switch triggers an interrupt for off hook and rotary dial signaling
  attachInterrupt(digitalPinToInterrupt(StD), isrDTMF, RISING);      // DTMF data valid interrupt
}

void loop() {

  voipPoll();                 // receive call signaling and audio from other nodes
  settingsPoll();             // apply settings saved on the web page
  vmPoll();                   // voicemail deletes from the web page
  runStateMachine();          // handle state machine operations
  checkIRQ();                 // process any pending interrupts for SHK or dialing events
  callProgressGen.update();   // handle audio generation operations if in progress
  pulser.run();               // handle pulse dial operations if in progress

  // debounce SHK if it has changed state
  // this could be implemented better but software isn't my first language
  unsigned long lapsedTime = millis() - shkDebounceTimer;
  if (shkTimerRunning && (lapsedTime >= shkDebounceInterval)) {  // if hook switch has been debounced, determine its final state
    shkTimerRunning = false;
    stateOffhook = digitalRead(slic_SHK);

    // if going off hook, update hook switch after debounce
    // if going on hook, wait for the longer on-hook delay to confirm it wasn't pulse dialing
    if (stateOffhook)
      bitSet(sysFlags, sysFlag_offHook);
  }


  // if phone went on-hook, verify if it was a hang up vs pulse dial event
  // this needs to be more robust eg. read SHK every 30 mS or something and be sure it was on hook each time?
  lapsedTime = millis() - shkOnHookTimer;
  if (shkOnHookTimerRunning && (lapsedTime >= shkOnHookInterval)) {
    shkOnHookTimerRunning = false;
    if (!pulseDialingNow) {                         // if a pulse dialed digit has not been detected by now, it was a hang-up event
      if (!digitalRead(slic_SHK))                   // if SHK is still low, register the hang up event
        bitClear(sysFlags, sysFlag_offHook);
    }
  }


  // when in dialing mode, accumulate DTMF digits until there is a phone number to dial.
  // digits come from the MT8870 chip and from the software decoder listening to the
  // same audio on GPIO32, so a key press either one misses is still caught.
  bool dialing = bitRead(sysFlags, sysFlag_dialMode);
  voipSetDialListen(dialing);
  if (dialing) {
    if (bitRead(sysFlags, sysFlag_StD_trig)) {
      bitClear(sysFlags, sysFlag_StD_trig);
      char c_read = readDtmf();
      if (c_read != ' ') addDialedDigit(c_read, false);   // if a new DTMF key has been read
    }
    char sw = voipGetDtmfDigit();
    if (sw) addDialedDigit(sw, true);
  }

}  // end loop

// add a DTMF digit to the number being dialed.  When both decoders hear the same
// key press, the second report (within 150 mS) is ignored.
static char lastDigit = 0;
static bool lastFromSoftware = false;
static unsigned long lastDigitTime = 0;

void addDialedDigit(char c, bool fromSoftware) {
  if (c == lastDigit && fromSoftware != lastFromSoftware && (millis() - lastDigitTime) < 150) {
    lastDigit = 0;                              // same key press, other decoder: already counted
    return;
  }
  lastDigit = c;
  lastFromSoftware = fromSoftware;
  lastDigitTime = millis();

  callProgressGen.audioStop();                  // stop playing dialtone when phone keys get pressed
  dialedNumber += c;                            // append dtmf character to dialed number
  DEBUG_PRINT(fromSoftware ? F("[dtmf sw] ") : F("[dtmf MT8870] "));
  DEBUG_PRINTLN(c);
  DEBUG_PRINT( F("Dialed digits so far: ") );
  DEBUG_PRINTLN(dialedNumber);
}

void maybeDialingStartedCallback() {
  // any purpose?
}


// pulse dial digit callback
void digitReceivedCallback(char digit) {
  if (!bitRead(sysFlags, sysFlag_dialMode)) return;   // ignore hook flashes/pulses during a call
  callProgressGen.audioStop();                // stop playing dialtone when phone keys get pressed
  dialedNumber += digit;          // accumulate the digit

  DEBUG_PRINT(F("[pulse digit] "));
  DEBUG_PRINTLN(digit);

  DEBUG_PRINT( F("Dialed digits so far: ") );
  DEBUG_PRINTLN(dialedNumber);

}



// read a key press from the MT8870 outside of dialing (Batphone escape)
char takeKeyMT8870() {
  if (!bitRead(sysFlags, sysFlag_StD_trig)) return 0;
  bitClear(sysFlags, sysFlag_StD_trig);
  char c = readDtmf();
  if (c == ' ') return 0;
  lastDigit = c;                    // so the software decoder's report of the same key press is ignored
  lastFromSoftware = false;
  lastDigitTime = millis();
  return c;
}

void runStateMachine() {
  enum states {
    startup,                   // initial state at system reset
    idle,                      // standby mode waiting for activity to occur
    acceptDialInput,           // phone went off hook, accumulate dialed digits
    attemptPlaceCall,          // try to place a call when a number is dialed
    waitForCallPlaceResponse,  // wait until remote node responds to call request, or a timeout occurs
    remotePhoneRinging,        // destination phone is ringing.  wait for remote phone to go off hook
    ringPhone,                 // ring the local phone to indicate there is an incoming call
    connectCall,               // call is connected (VOIP audio or phone line relay)
    voicemailAnswer,           // incoming call not picked up: caller hears the greeting and leaves a message
    voicemailMenu,             // this phone is listening to its messages
    featureFeedback,           // a * code was dialed: confirmation or error beeps, then dial tone again
    reminderRing,              // single ring: a message is waiting
    callFailed                 // call could not be completed (busy or unreachable), wait for hang up
  };

  static enum states currState = startup;                           // initial state is "startup"
  static boolean enteringNewState = true;                           // whether or not the current state is just beginning
  static unsigned long offhookTimer = millis();                     // timer to track if phone is off hook too long without a call connected
  static const unsigned long offhookTimerInterval_mS = 15000;       // timeout period before playing "this is a recording" message
  static unsigned long howlerTimer = millis();
  static const unsigned long howlerTimerInterval_mS = 20000;        // timeout period before playing howler tone when off hook too long
  static unsigned int digitsChecked = 0;                            // how many dialed digits have been checked
  static bool failWithBusy = false;                                 // callFailed: true = busy signal, false = recorded message
  static bool feedbackOk = false;                                   // featureFeedback: confirmation or error beeps
  static String callNumber;                                         // number being called
  static unsigned long stateTimer = millis();                       // general purpose timer for the current state
  static bool stutterTone = false;                                  // messages waiting: stutter dial tone playing
  static bool batphoneCall = false;                                 // this call was dialed by the Batphone
  static bool batphoneEscaped = false;                              // * was pressed: normal dial tone this time
  static unsigned long reminderBase = 0;                            // message waiting reminder timer
  static unsigned long menuKeyTimer = millis();                     // voicemail menu inactivity

  VoipEvent voipEvent = voipGetEvent();                             // latest call event from the network (if any)

  // if phone number is taking too long to finish dialing,
  // play "this is a recording" message
  if ((bitRead(sysFlags, sysFlag_enableTimer1)) && !(currState == connectCall)  && (!bitRead(sysFlags, sysFlag_timer1Exp))) {
    if (!(dialedNumber == "")) {                                 // only play recorded message if dialing was in progress and took too long
      if ((millis() - offhookTimer) > offhookTimerInterval_mS) {
        DEBUG_PRINTLN( F("[note] Start playing 'this is a recording' message") );
        howlerTimer = millis();                                  // if recorded message is played, restart howler timer to play after recording
        bitSet(sysFlags, sysFlag_timer1Exp);                     // indicate recording has started playing after timer delay expired
        bitClear(sysFlags, sysFlag_enableTimer1);                // disable recording delay timer once playback has started
        callProgressGen.audioStop();                             // cancel any call progress audio tones that were playing
        callProgressGen.thisIsARecordingStart();                 // begin playing recorded message
      }
    }
  }

  // if phone is off hook for too long without a call being connected, play howler tone
  if ((bitRead(sysFlags, sysFlag_enableTimer2)) && !(currState == connectCall) && (!bitRead(sysFlags, sysFlag_timer2Exp))) {
    if ((millis() - howlerTimer) > howlerTimerInterval_mS) {
      DEBUG_PRINTLN( F("[note] Howler tone playing") );
      bitSet(sysFlags, sysFlag_timer2Exp);                       // indicate howler has started after timer delay expires
      bitClear(sysFlags, sysFlag_enableTimer2);                  // disable howler delay timer once playback has started
      callProgressGen.audioStop();                               // cancel any call progress audio tones that were playing
      callProgressGen.howlerToneStart();                         // begin playing howler
    }
  }

  // message waiting reminder: timer counts from the newest message or the last reminder
  if (vmNewCount() == 0) reminderBase = 0;
  else if (reminderBase == 0 || vmLastNewMessageMs() > reminderBase) reminderBase = vmLastNewMessageMs() ? vmLastNewMessageMs() : millis();

  switch (currState) {
    case startup:                                                // initial state, perform any first-run operations and go to idle state
      callProgressGen.startup();                                 // initialize mozzi call progress tone generator
      remotePhoneNum = "";                                       // phone number of remote node contacting local node
      dialedNumber = "";                                         // no local dialing has started yet
      currState = idle;
      enteringNewState = true;
      break;

    case idle:
      if (enteringNewState == true) {                            // run once when transitioning into state
        DEBUG_PRINTLN( F("\r\n[enter state] idle") );
        if (voipCallActive()) voipHangup();                      // make sure any call in progress is ended
        voipLocalStop();
        digitalWrite (relayPin, LOW);                            // ensure tip/ring trunk line is disconnected
        digitalWrite(slic_audioMute, HIGH);                      // disable audio path out of SLIC
        led.setColor(RGBLed::BLUE);                              // set LED to indicate idle
        dialedNumber = "";                                       // prepare to accept a new sequence of digits to dial
        pulseDialingNow = false;                                 // prepare to look for new pulse dial events
        pulser.start();                                          // prepare pulse dial handler
        bitClear(sysFlags, sysFlag_dialMode);                    // don't process dialed digits unless placing a call
        bitClear(sysFlags, sysFlag_timer1Exp);                   // reset offhook timeout indicators
        bitClear(sysFlags, sysFlag_timer2Exp);
        bitClear(sysFlags, sysFlag_enableTimer1);
        bitClear(sysFlags, sysFlag_enableTimer2);
        callProgressGen.audioStop();                             // cancel any audio that may be playing
        setNodeAvailable(true);                                  // this node is available for calls
        batphoneCall = false;
        batphoneEscaped = false;
        enteringNewState = false;
      }
      if (voipEvent == VOIP_EV_INCOMING) {                       // another node is calling this node
        setNodeAvailable(false);                                 // this node is not available for other calls
        enteringNewState = true;
        currState = ringPhone;                                   // ring the local phone and wait for a pick up off hook
      }
      else if (offHook()) {                                      // if phone goes off hook, prepare for dialing
        setNodeAvailable(false);                                 // this node is not available for calls
        digitalWrite(slic_audioMute, LOW);                       // enable audio path out of SLIC
        led.setColor(RGBLed::MAGENTA);                           // set LED to indicate off hook
        enteringNewState = true;
        currState = acceptDialInput;
      }
      else if (reminderBase && settings.reminderMinutes &&
               (millis() - reminderBase) > (unsigned long)settings.reminderMinutes * 60000UL) {
        reminderBase = millis();                                 // ring once: a message is waiting
        enteringNewState = true;
        currState = reminderRing;
      }
      break;

    case reminderRing:
      if (enteringNewState == true) {
        DEBUG_PRINTLN( F("[enter state] reminderRing") );
        setNodeAvailable(false);
        ringGenStart();
        stateTimer = millis();
        enteringNewState = false;
      }
      if (bitRead(sysFlags, sysFlag_ringing)) slicRingGenerate();
      if (offHook()) {                                           // picked up during the reminder: go to voicemail
        slicRingStop();
        digitalWrite(slic_audioMute, LOW);
        led.setColor(RGBLed::MAGENTA);
        enteringNewState = true;
        currState = voicemailMenu;
      }
      else if (voipEvent == VOIP_EV_INCOMING) {                  // a call came in: ring for it instead
        slicRingStop();
        enteringNewState = true;
        currState = ringPhone;
      }
      else if ((millis() - stateTimer) >= (unsigned long)ringCadence[0]) {   // one ring burst
        slicRingStop();
        enteringNewState = true;
        currState = idle;
      }
      break;

    case acceptDialInput:
      if (enteringNewState == true) {                            // run once when transitioning into state
        DEBUG_PRINTLN( F("[enter state] acceptDialInput") );
        dialedNumber = "";
        digitsChecked = 0;
        offhookTimer = millis();                                 // enable offhook timers for recorded message and howler playback if there is a timeout
        howlerTimer = millis();
        bitClear(sysFlags, sysFlag_timer1Exp);
        bitClear(sysFlags, sysFlag_timer2Exp);
        bitSet(sysFlags, sysFlag_enableTimer1);
        bitSet(sysFlags, sysFlag_enableTimer2);
        bitSet(sysFlags, sysFlag_dialMode);                      // start accumulating dialed digits to dial a number
        stateTimer = millis();
        batphoneCall = false;
        enteringNewState = false;

        if (settings.batphone.length() && !batphoneEscaped && settings.batphoneDelay == 0) {
          callNumber = settings.batphone;                        // Batphone: dial straight away, no dial tone
          batphoneCall = true;
          bitClear(sysFlags, sysFlag_dialMode);
          bitClear(sysFlags, sysFlag_enableTimer1);
          bitClear(sysFlags, sysFlag_enableTimer2);
          DEBUG_PRINTLN( F("[batphone] dialing - press * for a normal dial tone") );
          enteringNewState = true;
          currState = attemptPlaceCall;
          break;
        }
        stutterTone = vmNewCount() > 0;                          // messages waiting: stutter dial tone
        if (stutterTone) callProgressGen.stutterDialToneStart();
        else callProgressGen.dialToneStart();                    // play call progress dial tone in local phone while waiting for dialed digits
      }
      if (!offHook()) {                                          // if phone goes on hook, cancel dialing
        callProgressGen.audioStop();                             // stop playing dialtone
        bitClear(sysFlags, sysFlag_dialMode);                    // don't process dialed digits unless placing a call
        enteringNewState = true;
        currState = idle;
        break;
      }
      if (stutterTone && dialedNumber.length() == 0 && (millis() - stateTimer) > 1500) {
        stutterTone = false;                                     // stutter for 1.5 s, then normal dial tone
        callProgressGen.dialToneStart();
      }
      // Batphone with a delay: dial tone first, then dial the Batphone if nothing was dialed
      if (settings.batphone.length() && !batphoneEscaped && dialedNumber.length() == 0 &&
          (millis() - stateTimer) > (unsigned long)settings.batphoneDelay * 1000UL) {
        callProgressGen.audioStop();
        callNumber = settings.batphone;
        batphoneCall = true;
        bitClear(sysFlags, sysFlag_dialMode);
        bitClear(sysFlags, sysFlag_enableTimer1);
        bitClear(sysFlags, sysFlag_enableTimer2);
        enteringNewState = true;
        currState = attemptPlaceCall;
        break;
      }

      // each time a new digit is dialed, see what the digits mean
      if (dialedNumber.length() != digitsChecked) {
        digitsChecked = dialedNumber.length();
        DialResult dr = classifyDialed(dialedNumber);
        if (dr.kind == DK_INCOMPLETE) break;

        bitClear(sysFlags, sysFlag_enableTimer1);                // a complete number or code: no more timeouts here
        bitClear(sysFlags, sysFlag_enableTimer2);
        if (dr.kind != DK_VOICEMAIL) bitClear(sysFlags, sysFlag_dialMode);
        enteringNewState = true;
        switch (dr.kind) {
          case DK_CALL:
            callNumber = dr.number;
            currState = attemptPlaceCall;
            break;
          case DK_VOICEMAIL:
            currState = voicemailMenu;
            break;
          case DK_ECHO:
          case DK_ECHO_RADIO_OFF:
            voipStartLoopback(dr.kind == DK_ECHO_RADIO_OFF);     // hear yourself through the audio path
            currState = connectCall;
            break;
          case DK_SCAN:                                          // line scan: wideband noise report on serial monitor
            callProgressGen.audioStop();
            delay(300);                                          // let the key press tone die away
            voipLineScan();
            failWithBusy = true;                                 // busy tone tells you it's done
            currState = callFailed;
            break;
          case DK_SETTING_OK:
          case DK_SETTING_ERROR:
            feedbackOk = (dr.kind == DK_SETTING_OK);
            currState = featureFeedback;
            break;
          default:                                               // not a number or code: recording
            failWithBusy = false;
            currState = callFailed;
            break;
        }
      }
      break;

    case featureFeedback:
      if (enteringNewState == true) {
        DEBUG_PRINTLN(feedbackOk ? F("[enter state] featureFeedback (ok)") : F("[enter state] featureFeedback (error)"));
        callProgressGen.audioStop();
        vmFeedback(feedbackOk);
        enteringNewState = false;
      }
      if (!offHook()) {
        vmFeedbackEnd();
        enteringNewState = true;
        currState = idle;
      }
      else if (!vmFeedbackBusy()) {                              // beeps done: dial tone again for another code or call
        vmFeedbackEnd();
        batphoneEscaped = true;                                  // don't let the Batphone jump in
        enteringNewState = true;
        currState = acceptDialInput;
      }
      break;

    case voicemailMenu:
      if (enteringNewState == true) {
        DEBUG_PRINTLN( F("[enter state] voicemailMenu") );
        callProgressGen.audioStop();
        bitClear(sysFlags, sysFlag_enableTimer1);
        bitClear(sysFlags, sysFlag_enableTimer2);
        bitSet(sysFlags, sysFlag_dialMode);                      // keys go to the menu
        dialedNumber = "";
        led.setColor(RGBLed::YELLOW);
        vmMenuStart();
        menuKeyTimer = millis();
        enteringNewState = false;
      }
      if (!offHook()) {
        vmMenuEnd();
        enteringNewState = true;
        currState = idle;
        break;
      }
      vmMenuPoll();
      while (dialedNumber.length()) {                            // keys pressed
        char k = dialedNumber[0];
        dialedNumber.remove(0, 1);
        menuKeyTimer = millis();
        if (vmMenuKey(k) == VM_MENU_EXIT) {                      // * = back to dial tone
          vmMenuEnd();
          led.setColor(RGBLed::MAGENTA);
          batphoneEscaped = true;
          enteringNewState = true;
          currState = acceptDialInput;
          break;
        }
      }
      if (currState == voicemailMenu && (millis() - menuKeyTimer) > 120000UL) {   // left off hook in the menu
        vmMenuEnd();
        bitClear(sysFlags, sysFlag_dialMode);
        failWithBusy = true;
        enteringNewState = true;
        currState = callFailed;
      }
      break;

    case attemptPlaceCall:
      DEBUG_PRINT( F("[enter state] attemptPlaceCall ") );
      DEBUG_PRINTLN(callNumber);
      enteringNewState = true;
      if (voipPlaceCall(callNumber)) {                           // look up the node and send it a call request
        currState = waitForCallPlaceResponse;
      } else {
        failWithBusy = false;
        currState = callFailed;
      }
      break;

    case waitForCallPlaceResponse:
      if (enteringNewState == true) {                            // run once when transitioning into state
        DEBUG_PRINTLN( F("[enter state] waitForCallPlaceResponse") );
        enteringNewState = false;
      }

      if (!offHook()) {                                          // if phone goes on hook before call is connected, end call attempt
        voipHangup();                                            // notify other node the call is aborted
        enteringNewState = true;
        currState = idle;
      }
      else if (batphoneCall && takeKeyMT8870() == '*') {         // Batphone: * = cancel and get a normal dial tone
        voipHangup();
        batphoneEscaped = true;
        enteringNewState = true;
        currState = acceptDialInput;
      }
      else if (voipEvent == VOIP_EV_RINGING) {                   // remote node is ringing its phone, play ring back tone
        enteringNewState = true;
        currState = remotePhoneRinging;
      }
      else if (voipEvent == VOIP_EV_ANSWERED) {                  // answered before we heard it was ringing
        enteringNewState = true;
        currState = connectCall;
      }
      else if (voipEvent == VOIP_EV_BUSY) {                      // remote node is busy, can't complete the call
        failWithBusy = true;
        enteringNewState = true;
        currState = callFailed;
      }
      else if (voipEvent == VOIP_EV_TIMEOUT || voipEvent == VOIP_EV_HANGUP) {  // remote node didn't respond
        failWithBusy = false;
        enteringNewState = true;
        currState = callFailed;
      }
      break;

    case remotePhoneRinging:
      if (enteringNewState == true) {                            // run once when transitioning into state
        DEBUG_PRINTLN( F("[enter state] remotePhoneRinging") );
        callProgressGen.ringToneStart();                         // start playing call progress ring tone to alert caller of status
        enteringNewState = false;
      }

      if (!offHook()) {                                          // if phone goes on hook before remote node answers, end the call
        voipHangup();                                            // notify other node the call is aborted
        enteringNewState = true;
        currState = idle;
      }
      else if (batphoneCall && takeKeyMT8870() == '*') {         // Batphone: * = cancel and get a normal dial tone
        voipHangup();
        batphoneEscaped = true;
        enteringNewState = true;
        currState = acceptDialInput;
      }
      else if (voipEvent == VOIP_EV_ANSWERED) {                  // remote phone has been answered (or voicemail picked up)
        enteringNewState = true;
        currState = connectCall;
      }
      else if (voipEvent == VOIP_EV_HANGUP || voipEvent == VOIP_EV_TIMEOUT || voipEvent == VOIP_EV_BUSY) {
        failWithBusy = true;                                     // remote node went away while ringing
        enteringNewState = true;
        currState = callFailed;
      }
      break;

    case ringPhone:
      if (enteringNewState == true) {                             // run once when transitioning into state
        DEBUG_PRINT( F("[enter state] ringPhone, call from ") );
        DEBUG_PRINTLN(remotePhoneNum);
        setNodeAvailable(false);                                  // this node is not available for new calls
        ringGenStart();                                           // ring the local phone
        stateTimer = millis();
        enteringNewState = false;
      }

      if (bitRead(sysFlags, sysFlag_ringing)) {                   // if SLIC is ringing the phone, handle the control signals
        slicRingGenerate();
      }

      if (offHook()) {                                            // if phone is answered, connect the call
        slicRingStop();                                           // stop ringing local phone
        voipAnswer();                                             // tell the calling node the call has been answered
        enteringNewState = true;
        currState = connectCall;
      }
      else if (voipEvent == VOIP_EV_HANGUP || voipEvent == VOIP_EV_TIMEOUT) {  // caller hung up before we answered
        slicRingStop();
        enteringNewState = true;
        currState = idle;
      }
      else if (settings.vmRings > 0 && !voipCallIsTrunk()) {      // nobody answered after N rings: voicemail
        unsigned long ringCycle_mS = ringCadence[0] + ringCadence[1] + ringCadence[2] + ringCadence[3];
        if ((millis() - stateTimer) > (unsigned long)settings.vmRings * ringCycle_mS) {
          slicRingStop();
          voipAnswer();
          vmAnswerStart(remotePhoneNum);                          // greeting, beep, record the caller
          enteringNewState = true;
          currState = voicemailAnswer;
        }
      }
      break;

    case voicemailAnswer:
      if (enteringNewState == true) {                             // run once when transitioning into state
        DEBUG_PRINTLN( F("[enter state] voicemailAnswer") );
        led.setColor(RGBLed::YELLOW);                             // set LED to indicate voicemail
        enteringNewState = false;
      }

      if (offHook()) {                                            // picked up during voicemail: take the call
        vmAnswerEnd(false);
        enteringNewState = true;
        currState = connectCall;
      }
      else if (voipEvent == VOIP_EV_HANGUP || voipEvent == VOIP_EV_TIMEOUT) {  // caller hung up: keep the message
        vmAnswerEnd(true);
        enteringNewState = true;
        currState = idle;
      }
      else if (vmAnswerPoll()) {                                  // message finished (time limit / mailbox full)
        vmAnswerEnd(true);
        voipHangup();
        enteringNewState = true;
        currState = idle;
      }
      break;

    case connectCall:
      if (enteringNewState == true) {                             // run once when transitioning into state
        callProgressGen.audioStop();                              // cancel any call progress audio tones that were playing
        if (voipCallIsTrunk())
          digitalWrite(relayPin, HIGH);                           // phone line call: connect tip/ring to the trunk line
        digitalWrite(slic_audioMute, LOW);                        // enable audio path out of SLIC so it can be sent to the far end
        led.setColor(RGBLed::GREEN);                              // set LED to indicate call connected
        DEBUG_PRINTLN( voipCallIsTrunk() ? F("[enter state] connectCall (phone line)") : F("[enter state] connectCall") );
        enteringNewState = false;
      }

      if (!offHook()) {                                           // if phone goes on hook, end call
        voipHangup();                                             // notify other node the call is over
        enteringNewState = true;
        currState = idle;
      }
      else if (voipEvent == VOIP_EV_HANGUP || voipEvent == VOIP_EV_TIMEOUT) {  // remote phone has been hung up
        enteringNewState = true;
        failWithBusy = true;                                      // far end hung up: busy tone until we hang up too
        currState = callFailed;
      }
      break;

    case callFailed:
      if (enteringNewState == true) {                             // run once when transitioning into state
        DEBUG_PRINTLN( failWithBusy ? F("[enter state] callFailed (busy)") : F("[enter state] callFailed (recording)") );
        if (voipCallActive()) voipHangup();
        digitalWrite(relayPin, LOW);
        callProgressGen.audioStop();
        if (failWithBusy)
          callProgressGen.busyToneStart();                        // busy signal
        else
          callProgressGen.thisIsARecordingStart();                // "this is a recording..." number can't be reached
        howlerTimer = millis();                                   // howler if the phone is left off hook
        bitClear(sysFlags, sysFlag_timer2Exp);
        bitSet(sysFlags, sysFlag_enableTimer2);
        led.setColor(RGBLed::MAGENTA);
        enteringNewState = false;
      }

      if (!offHook()) {                                           // wait for hang up
        enteringNewState = true;
        currState = idle;
      }
      else if (batphoneCall && takeKeyMT8870() == '*') {          // Batphone: * = normal dial tone
        batphoneEscaped = true;
        enteringNewState = true;
        currState = acceptDialInput;
      }
      break;
  }  // end switch case
}  // end state machine
