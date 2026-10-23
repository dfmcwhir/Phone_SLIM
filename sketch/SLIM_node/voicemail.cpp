/*
  voicemail.cpp

  Files in LittleFS:
    /vm/greet.ul                    custom greeting (built in "this is a recording" message if missing)
    /vm/N00012_8675309.ul           message 12 from 8675309, not listened to yet
    /vm/O00012_8675309.ul           ...after it has been listened to
  All audio is 8192 samples/sec u-law, one byte per sample.

  Menu keys (dial *4 or your own number):
    on entry: one high beep per new message, then the oldest new message plays
              (low beeps = number of old messages if there are no new ones,
               one long low tone = no messages)
    1 replay      2 next      3 previous      7 delete
    0 message count beeps     5 record greeting (press any key to finish)
    6 play greeting           8 go back to the built in greeting
    9 list messages and settings on the serial monitor
    * leave voicemail (dial tone)
*/
#include "voicemail.h"
#include "voip.h"
#include "voip_config.h"
#include "settings.h"
#include "mozzi_call_progress.h"  // built in recorded message
#include <FS.h>
#include <LittleFS.h>
#include <math.h>

#define SAMPLE_RATE      8192
#define BYTES_PER_SEC    8192
#define GREETING_PATH    "/vm/greet.ul"
#define GREETING_TMP     "/vm/greet.tmp"
#define MESSAGE_TMP      "/vm/msg.tmp"
#define MIN_FREE_BYTES   (24 * 1024)        // keep this much flash free
#define MIN_MESSAGE_BYTES (BYTES_PER_SEC)   // shorter messages (hang ups after the beep) are thrown away

static bool fsOk = false;

// ---------------------------------------------------------------------------
// message list
// ---------------------------------------------------------------------------
struct Message {
  uint16_t id;
  bool     isNew;
  char     from[16];
  uint32_t bytes;
};
static Message msgs[VM_MAX_MESSAGES];
static int     msgCount = 0;
static uint16_t nextId = 1;
static unsigned long lastNewMs = 0;

static String msgPath(const Message& m) {
  char p[48];
  snprintf(p, sizeof(p), "/vm/%c%05u_%s.ul", m.isNew ? 'N' : 'O', m.id, m.from);
  return String(p);
}

static void scanMessages() {
  msgCount = 0;
  nextId = 1;
  if (!fsOk) return;
  File dir = LittleFS.open("/vm");
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    uint32_t size = f.size();
    f.close();
    // N00012_8675309.ul
    if (name.length() < 10 || (name[0] != 'N' && name[0] != 'O') || !name.endsWith(".ul") || name[6] != '_') continue;
    if (msgCount >= VM_MAX_MESSAGES) continue;
    Message& m = msgs[msgCount];
    m.isNew = (name[0] == 'N');
    m.id = (uint16_t)name.substring(1, 6).toInt();
    String from = name.substring(7, name.length() - 3);
    strncpy(m.from, from.c_str(), sizeof(m.from) - 1);
    m.from[sizeof(m.from) - 1] = 0;
    m.bytes = size;
    if (m.id >= nextId) nextId = m.id + 1;
    msgCount++;
  }
  // oldest first
  for (int i = 1; i < msgCount; i++)
    for (int j = i; j > 0 && msgs[j].id < msgs[j - 1].id; j--) {
      Message t = msgs[j]; msgs[j] = msgs[j - 1]; msgs[j - 1] = t;
    }
}

static int findMessage(uint16_t id) {
  for (int i = 0; i < msgCount; i++) if (msgs[i].id == id) return i;
  return -1;
}

int vmNewCount() {
  int n = 0;
  for (int i = 0; i < msgCount; i++) if (msgs[i].isNew) n++;
  return n;
}

int vmTotalCount() { return msgCount; }
unsigned long vmLastNewMessageMs() { return lastNewMs; }

static uint32_t freeBytes() {
  if (!fsOk) return 0;
  size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  return used >= total ? 0 : (uint32_t)(total - used);
}

static void printMessages() {
  Serial.printf("[vm] %d message(s), %d new.  Room for about %u more seconds.\r\n",
                msgCount, vmNewCount(), (unsigned)(freeBytes() > MIN_FREE_BYTES ? (freeBytes() - MIN_FREE_BYTES) / BYTES_PER_SEC : 0));
  for (int i = 0; i < msgCount; i++)
    Serial.printf("[vm]   #%u from %s, %u s%s\r\n", msgs[i].id, msgs[i].from,
                  (unsigned)(msgs[i].bytes / BYTES_PER_SEC), msgs[i].isNew ? ", new" : "");
  Serial.printf("[vm] greeting: %s\r\n", (fsOk && LittleFS.exists(GREETING_PATH)) ? "recorded" : "built in");
}

void vmBegin() {
  fsOk = LittleFS.begin(true);                   // formats the storage the first time (takes a few seconds)
  if (!fsOk) {
    Serial.println(F("[vm] could not mount flash storage - voicemail disabled"));
    return;
  }
  if (!LittleFS.exists("/vm")) LittleFS.mkdir("/vm");
  LittleFS.remove(MESSAGE_TMP);                  // leftovers from a power cut while recording
  LittleFS.remove(GREETING_TMP);
  scanMessages();
  printMessages();
}

// ---------------------------------------------------------------------------
// player: a queue of tones, silences and recordings, read 8192 Hz samples at a time
// ---------------------------------------------------------------------------
enum ItemType : uint8_t { IT_TONE, IT_SILENCE, IT_FILE, IT_BUILTIN };
struct PlayItem {
  ItemType type;
  uint16_t freq;
  uint32_t samples;
  char     path[40];
};
#define PLAYQ_SIZE 24
static PlayItem playQ[PLAYQ_SIZE];
static int      playHead = 0, playCount = 0;
static uint32_t itemPos = 0;
static bool     itemOpen = false;
static File     playFile;
static float    tonePhase = 0;

static void playerClear() {
  if (playFile) playFile.close();
  playHead = playCount = 0;
  itemPos = 0;
  itemOpen = false;
}

static PlayItem* playerAdd(ItemType t) {
  if (playCount >= PLAYQ_SIZE) return NULL;
  PlayItem* it = &playQ[(playHead + playCount) % PLAYQ_SIZE];
  memset(it, 0, sizeof(*it));
  it->type = t;
  playCount++;
  return it;
}

static void playerTone(uint16_t freq, uint16_t ms) {
  PlayItem* it = playerAdd(IT_TONE);
  if (it) { it->freq = freq; it->samples = (uint32_t)ms * SAMPLE_RATE / 1000; }
}

static void playerSilence(uint16_t ms) {
  PlayItem* it = playerAdd(IT_SILENCE);
  if (it) it->samples = (uint32_t)ms * SAMPLE_RATE / 1000;
}

static void playerBeeps(uint16_t freq, int count, uint16_t onMs = 150, uint16_t offMs = 150) {
  for (int i = 0; i < count; i++) {
    playerTone(freq, onMs);
    playerSilence(offMs);
  }
}

static void playerFile(const String& path) {
  PlayItem* it = playerAdd(IT_FILE);
  if (it) strncpy(it->path, path.c_str(), sizeof(it->path) - 1);
}

static void playerGreeting() {
  if (fsOk && LittleFS.exists(GREETING_PATH)) playerFile(GREETING_PATH);
  else playerAdd(IT_BUILTIN);
}

static bool playerBusy() { return playCount > 0; }

static void nextItem() {
  if (playFile) playFile.close();
  playHead = (playHead + 1) % PLAYQ_SIZE;
  playCount--;
  itemPos = 0;
  itemOpen = false;
}

// produce up to n samples; returns 0 when there is nothing left to play
static int playerRead(int16_t* buf, int n) {
  int out = 0;
  while (out < n && playCount > 0) {
    PlayItem& it = playQ[playHead];
    switch (it.type) {
      case IT_TONE: {
        const int ramp = 40;                     // 5 mS fade in/out, no clicks
        float step = 2.0f * (float)M_PI * it.freq / SAMPLE_RATE;
        while (out < n && itemPos < it.samples) {
          float env = 1.0f;
          if (itemPos < (uint32_t)ramp) env = itemPos / (float)ramp;
          else if (it.samples - itemPos < (uint32_t)ramp) env = (it.samples - itemPos) / (float)ramp;
          buf[out++] = (int16_t)(8000.0f * env * sinf(tonePhase));
          tonePhase += step;
          if (tonePhase > 2.0f * (float)M_PI) tonePhase -= 2.0f * (float)M_PI;
          itemPos++;
        }
        if (itemPos >= it.samples) nextItem();
        break;
      }
      case IT_SILENCE:
        while (out < n && itemPos < it.samples) { buf[out++] = 0; itemPos++; }
        if (itemPos >= it.samples) nextItem();
        break;

      case IT_FILE: {
        if (!itemOpen) {
          itemOpen = true;
          playFile = fsOk ? LittleFS.open(it.path, "r") : File();
        }
        if (!playFile) { nextItem(); break; }
        uint8_t tmp[160];
        int want = n - out;
        if (want > (int)sizeof(tmp)) want = sizeof(tmp);
        int got = playFile.read(tmp, want);
        if (got <= 0) { nextItem(); break; }
        for (int i = 0; i < got; i++) buf[out++] = voipUlawToLinear(tmp[i]);
        break;
      }
      case IT_BUILTIN: {                         // 16384 Hz 8 bit -> 8192 Hz
        uint32_t len = recordingLength();
        while (out < n && itemPos + 1 < len) {
          buf[out++] = (int16_t)(((int32_t)recordingSample(itemPos) + recordingSample(itemPos + 1)) * 128);
          itemPos += 2;
        }
        if (itemPos + 1 >= len) nextItem();
        break;
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// recorder: writes u-law to a file in chunks; can hold back the last part so
// it can be trimmed off (key press or hang up click at the end)
// ---------------------------------------------------------------------------
#define REC_CHUNK 2048
static File     recFile;
static uint8_t  recBuf[REC_CHUNK * 2];
static uint32_t recLen = 0;          // bytes in recBuf
static uint32_t recWritten = 0;      // bytes written to the file
static uint32_t recHoldback = 0;
static uint32_t recMaxBytes = 0;
static bool     recActive = false;
static bool     recFull = false;     // reached max length or storage full
static bool     recNoSpace = false;  // storage full

static bool recStart(const char* path, uint32_t maxSeconds, uint32_t holdbackBytes) {
  if (!fsOk) return false;
  recFile = LittleFS.open(path, "w");
  if (!recFile) return false;
  recLen = recWritten = 0;
  recHoldback = holdbackBytes > REC_CHUNK ? REC_CHUNK : holdbackBytes;
  recMaxBytes = maxSeconds * BYTES_PER_SEC;
  recActive = true;
  recFull = false;
  recNoSpace = false;
  return true;
}

static void recFlush(uint32_t n) {
  if (n == 0) return;
  if (freeBytes() < MIN_FREE_BYTES + n) {        // storage full: stop here
    recFull = true;
    recNoSpace = true;
    return;
  }
  recWritten += recFile.write(recBuf, n);
  memmove(recBuf, recBuf + n, recLen - n);
  recLen -= n;
}

static void recWrite(const uint8_t* data, int n) {
  if (!recActive || recFull) return;
  while (n > 0) {
    if (recWritten + recLen >= recMaxBytes) { recFull = true; return; }
    uint32_t room = sizeof(recBuf) - recLen;
    uint32_t take = (uint32_t)n < room ? (uint32_t)n : room;
    memcpy(recBuf + recLen, data, take);
    recLen += take;
    data += take;
    n -= take;
    if (recLen >= REC_CHUNK + recHoldback) recFlush(REC_CHUNK);
    if (recFull) return;
  }
}

// finish recording.  trim = drop the held back tail.  returns bytes kept (0 = discarded)
static uint32_t recStop(const char* tmpPath, bool keep, bool trim, const String& finalPath, uint32_t minBytes) {
  if (!recActive) return 0;
  recActive = false;
  if (keep) {
    uint32_t n = trim ? (recLen > recHoldback ? recLen - recHoldback : 0) : recLen;
    if (!recNoSpace) recFlush(n);
  }
  recFile.close();
  uint32_t bytes = recWritten;
  if (!keep || bytes < minBytes) {
    LittleFS.remove(tmpPath);
    return 0;
  }
  LittleFS.remove(finalPath);
  LittleFS.rename(tmpPath, finalPath);
  return bytes;
}

static void recSinkFn(const uint8_t* data, int n) { recWrite(data, n); }

// ---------------------------------------------------------------------------
// answering unanswered calls
// ---------------------------------------------------------------------------
enum AnswerState { VMA_IDLE, VMA_GREETING, VMA_RECORDING, VMA_GOODBYE, VMA_DONE };
static AnswerState answerState = VMA_IDLE;
static bool        answerCanRecord = false;
static String      answerCaller;
static unsigned long answerRecStartMs = 0;

static void finishMessage(bool keep) {
  Message m;
  m.id = nextId;
  m.isNew = true;
  strncpy(m.from, answerCaller.length() ? answerCaller.c_str() : "unknown", sizeof(m.from) - 1);
  m.from[sizeof(m.from) - 1] = 0;
  uint32_t bytes = recStop(MESSAGE_TMP, keep, true, msgPath(m), MIN_MESSAGE_BYTES);
  if (bytes) {
    lastNewMs = millis();
    if (lastNewMs == 0) lastNewMs = 1;
    Serial.printf("[vm] new message #%u from %s, %u seconds\r\n", m.id, m.from, (unsigned)(bytes / BYTES_PER_SEC));
    scanMessages();
  } else if (keep) {
    Serial.println(F("[vm] caller didn't leave a message"));
  }
}

void vmAnswerStart(const String& caller) {
  answerCaller = caller;
  answerCanRecord = fsOk && msgCount < VM_MAX_MESSAGES &&
                    freeBytes() > MIN_FREE_BYTES + 10 * BYTES_PER_SEC;
  playerClear();
  playerSilence(300);
  playerGreeting();
  playerSilence(300);
  if (answerCanRecord) {
    playerTone(1000, 500);                       // record after the beep
  } else {
    Serial.println(F("[vm] mailbox full - greeting only"));
    playerBeeps(480, 3, 300, 200);               // full: three low beeps, then hang up
  }
  voipSetTxSource(playerRead);
  answerState = VMA_GREETING;
  Serial.printf("[vm] answering call from %s\r\n", caller.c_str());
}

bool vmAnswerPoll() {
  switch (answerState) {
    case VMA_GREETING:
      if (playerBusy()) break;
      if (answerCanRecord && recStart(MESSAGE_TMP, VM_MAX_MESSAGE_SEC, BYTES_PER_SEC / 4)) {
        voipSetRxSink(recSinkFn);
        answerRecStartMs = millis();
        answerState = VMA_RECORDING;
        Serial.println(F("[vm] recording message"));
      } else {
        answerState = VMA_DONE;
      }
      break;

    case VMA_RECORDING:
      if (recFull) {                             // max length or storage full
        voipSetRxSink(NULL);
        finishMessage(true);
        playerBeeps(1000, 2, 200, 150);          // goodbye beeps
        answerState = VMA_GOODBYE;
      }
      break;

    case VMA_GOODBYE:
      if (!playerBusy()) answerState = VMA_DONE;
      break;

    case VMA_DONE:
      return true;

    default:
      break;
  }
  return false;
}

void vmAnswerEnd(bool keepMessage) {
  voipSetRxSink(NULL);
  voipSetTxSource(NULL);
  if (answerState == VMA_RECORDING) finishMessage(keepMessage);
  playerClear();
  answerState = VMA_IDLE;
}

// ---------------------------------------------------------------------------
// menu
// ---------------------------------------------------------------------------
enum MenuState { MS_OFF, MS_SOUND, MS_PLAYING, MS_GREET_BEEP, MS_GREET_REC };
static MenuState menuState = MS_OFF;
static int       curId = -1;                     // message being listened to (-1 = none)
static unsigned long greetStartMs = 0;

static bool soundDone() { return !playerBusy() && voipLocalBuffered() == 0; }

static void sayCounts() {
  int n = vmNewCount();
  if (n > 0) playerBeeps(1200, n > 9 ? 9 : n);                 // high beeps: new messages
  else if (msgCount > 0) playerBeeps(600, msgCount > 9 ? 9 : msgCount);   // low beeps: old messages
  else playerTone(400, 800);                                    // no messages
}

static void sayOk()    { playerTone(800, 100); playerTone(1000, 100); playerTone(1200, 120); }
static void sayError() { playerTone(400, 600); }

static void playMessage(int idx) {
  if (idx < 0 || idx >= msgCount) { sayError(); menuState = MS_SOUND; return; }
  curId = msgs[idx].id;
  Serial.printf("[vm] playing #%u from %s%s\r\n", msgs[idx].id, msgs[idx].from, msgs[idx].isNew ? " (new)" : "");
  playerTone(1000, 120);                         // short tick, then the message
  playerSilence(300);
  playerFile(msgPath(msgs[idx]));
  menuState = MS_PLAYING;
}

static void markHeard(uint16_t id) {
  int i = findMessage(id);
  if (i < 0 || !msgs[i].isNew) return;
  String oldPath = msgPath(msgs[i]);
  msgs[i].isNew = false;
  LittleFS.rename(oldPath, msgPath(msgs[i]));
}

static void startLocalPlayback() {
  voipLocalStart(playerRead, NULL);
}

void vmMenuStart() {
  scanMessages();
  printMessages();
  playerClear();
  startLocalPlayback();
  curId = -1;
  playerSilence(300);
  sayCounts();
  menuState = MS_SOUND;
  for (int i = 0; i < msgCount; i++) {           // start with the oldest new message
    if (msgs[i].isNew) {
      playerSilence(500);
      playMessage(i);
      break;
    }
  }
}

static void stopGreetingRecording(bool trim) {
  uint32_t bytes = recStop(GREETING_TMP, true, trim, GREETING_PATH, BYTES_PER_SEC / 2);
  voipLocalStop();
  startLocalPlayback();
  playerClear();
  if (bytes) {
    Serial.printf("[vm] new greeting recorded, %u seconds\r\n", (unsigned)(bytes / BYTES_PER_SEC));
    sayOk();
    playerSilence(500);
    playerGreeting();                            // play it back
  } else {
    Serial.println(F("[vm] greeting too short - kept the old one"));
    sayError();
  }
  menuState = MS_SOUND;
}

VmMenuResult vmMenuKey(char key) {
  if (menuState == MS_GREET_REC) {               // any key ends the greeting
    stopGreetingRecording(true);
    return VM_MENU_CONTINUE;
  }
  if (key == '*') return VM_MENU_EXIT;

  int cur = curId >= 0 ? findMessage(curId) : -1;
  playerClear();                                 // a key interrupts whatever is playing
  menuState = MS_SOUND;

  switch (key) {
    case '1':                                    // replay
      playMessage(cur);
      break;
    case '2':                                    // next
    case '#':
      if (cur >= 0) markHeard(msgs[cur].id);     // skipping counts as listened to
      playMessage(cur < 0 ? 0 : cur + 1);
      break;
    case '3':                                    // previous
      playMessage(cur < 0 ? msgCount - 1 : cur - 1);
      break;
    case '7':                                    // delete current message
      if (cur < 0) { sayError(); break; }
      LittleFS.remove(msgPath(msgs[cur]));
      Serial.printf("[vm] deleted #%u\r\n", msgs[cur].id);
      scanMessages();
      playerTone(1000, 150); playerTone(600, 250);
      curId = -1;
      if (cur < msgCount) { playerSilence(500); playMessage(cur); }   // play the next one
      break;
    case '0':                                    // counts
      sayCounts();
      break;
    case '5':                                    // record greeting
      playerSilence(200);
      playerTone(1000, 400);
      menuState = MS_GREET_BEEP;
      break;
    case '6':                                    // play greeting
      playerGreeting();
      break;
    case '8':                                    // built in greeting
      if (fsOk) LittleFS.remove(GREETING_PATH);
      Serial.println(F("[vm] using the built in greeting"));
      sayOk();
      break;
    case '9':
      printMessages();
      settingsPrint();
      sayOk();
      break;
    default:
      sayError();
      break;
  }
  return VM_MENU_CONTINUE;
}

void vmMenuPoll() {
  switch (menuState) {
    case MS_PLAYING:
      if (soundDone()) {                         // message finished
        if (curId >= 0) markHeard((uint16_t)curId);
        playerSilence(200);
        playerTone(600, 150);                    // end of message: press 1 2 3 7...
        menuState = MS_SOUND;
      }
      break;

    case MS_GREET_BEEP:
      if (soundDone()) {
        voipLocalStop();
        if (recStart(GREETING_TMP, VM_MAX_GREETING_SEC, BYTES_PER_SEC / 4)) {
          voipLocalStart(playerRead, recSinkFn); // record the microphone
          greetStartMs = millis();
          menuState = MS_GREET_REC;
          Serial.println(F("[vm] recording greeting - press any key to finish"));
        } else {
          startLocalPlayback();
          sayError();
          menuState = MS_SOUND;
        }
      }
      break;

    case MS_GREET_REC:
      if (recFull) stopGreetingRecording(false); // max length
      break;

    default:
      break;
  }
}

void vmMenuEnd() {
  if (menuState == MS_GREET_REC) recStop(GREETING_TMP, true, true, GREETING_PATH, BYTES_PER_SEC / 2);
  playerClear();
  voipLocalStop();
  menuState = MS_OFF;
  scanMessages();
}

// ---------------------------------------------------------------------------
// requests from the config web page (another task): handled in the main loop
// ---------------------------------------------------------------------------
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#define REQ_DELETE_GREETING 0xFFFF
static QueueHandle_t vmRequests = NULL;

void vmRequestDelete(uint16_t id) {
  if (!vmRequests) vmRequests = xQueueCreate(8, sizeof(uint16_t));
  xQueueSend(vmRequests, &id, 0);
}

void vmRequestDeleteGreeting() { vmRequestDelete(REQ_DELETE_GREETING); }

void vmPoll() {
  if (!vmRequests || menuState != MS_OFF) return;     // not while someone is listening on the phone
  uint16_t id;
  while (xQueueReceive(vmRequests, &id, 0) == pdTRUE) {
    if (id == REQ_DELETE_GREETING) {
      if (fsOk) LittleFS.remove(GREETING_PATH);
      Serial.println(F("[vm] greeting deleted from the web page - using the built in one"));
      continue;
    }
    int i = findMessage(id);
    if (i >= 0) {
      LittleFS.remove(msgPath(msgs[i]));
      Serial.printf("[vm] deleted #%u from the web page\r\n", id);
      scanMessages();
    }
  }
}

// ---------------------------------------------------------------------------
// feedback sounds for * codes
// ---------------------------------------------------------------------------
void vmFeedback(bool ok) {
  playerClear();
  startLocalPlayback();
  playerSilence(150);
  if (ok) sayOk(); else sayError();
  playerSilence(200);
}

bool vmFeedbackBusy() { return !soundDone(); }

void vmFeedbackEnd() {
  playerClear();
  voipLocalStop();
}
