/*
  voicemail.h
  Voicemail stored in the ESP32's flash (LittleFS), no SD card needed.

  - Unanswered VOIP calls are answered after settings.vmRings rings: the caller
    hears the greeting and a beep, and their message is recorded on this node.
  - Messages and the greeting are stored as 8192 Hz u-law (8 KB per second).
  - Dial *4 (or this phone's own number) to listen: see the menu keys in
    voicemail.cpp / README.  There are no spoken prompts; beeps give feedback.
*/
#ifndef voicemail_h
#define voicemail_h

#include "Arduino.h"

void vmBegin();                     // mount storage and find messages
int  vmNewCount();                  // messages not listened to yet
int  vmTotalCount();
unsigned long vmLastNewMessageMs(); // millis() when the newest message arrived (0 = none since boot)

// answering an unanswered VOIP call (call voipAnswer() first)
void vmAnswerStart(const String& caller);
bool vmAnswerPoll();                // true when voicemail is finished and the call should be hung up
void vmAnswerEnd(bool keepMessage); // call ended (caller hung up: keep) or phone picked up (discard)

// listening to messages from this phone
enum VmMenuResult { VM_MENU_CONTINUE, VM_MENU_EXIT };
void vmMenuStart();
VmMenuResult vmMenuKey(char key);   // a dialed digit while in the menu
void vmMenuPoll();
void vmMenuEnd();                   // phone hung up

// config web page requests (safe to call from another task); vmPoll() in loop() carries them out
void vmRequestDelete(uint16_t id);
void vmRequestDeleteGreeting();
void vmPoll();

// short confirmation / error sounds in the earpiece (for * codes)
void vmFeedback(bool ok);
bool vmFeedbackBusy();
void vmFeedbackEnd();

#endif
