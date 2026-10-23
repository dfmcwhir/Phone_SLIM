# Phone_SLIM
POTS Telephone Subscriber Line Interface Module
<br><br>![PCB](Assembled_PCB.jpg)<BR><BR>
# Usage Notes
> [!NOTE]
> * This project is intended for hobby/personal use to explore how the plain old telephone service works.<br>
> * The PCB isn't designed to integrate with a live phone line. It is intended to create an offline system for local use.<br><br>

* It is recommended to add a TO-220 heat sink to the 3.3v regulator in case it gets too hot. I used [Wakefield-Vette 274-3AB](https://www.digikey.com/en/products/detail/wakefield-vette/274-3AB/2351871)  along with thermal paste and a nut/screw I had around.  <br>
* One pcb is needed for each phone/modem/etc.<br>
* Calls between boards are made over WiFi (VOIP).  The phone line jack/relay between boards and the J5 Server/Sender jumper are no longer used.<br>
* Every board must connect to the same WiFi router.  Set the network on each board's config web page (below); the values in "arduino_secrets.h" are only used the first time.<br>
* Phone numbers and the directory (which number is at which IP) are also set on the config page; "voip_config.h" holds the first-boot defaults.  Copy the same directory to every board.  Each board finds its own number from the "This phone's number" setting, or by its MAC or IP address in the directory.  For DHCP, give each board a DHCP reservation in the router (or set a static IP on the config page) so its address never changes.<br>
* When a number is dialed it is matched against the directory and the call is placed to that board's IP address.  Numbers not in the directory get the "this is a recording" message, a board that is in use gives a busy signal.<br>
* Call audio: the local phone is sampled from the SLIC on GPIO32 (ADC1), and far end audio is played to the SLIC on GPIO25 (DAC1) through Mozzi.  Audio is G.711 u-law, 8192 samples/sec, sent as UDP packets on port 4210.  Microphone/speaker gain, buffering, and an optional echo suppressor are adjustable in "voip_config.h".<br>
* Requires ESP32 Arduino core 1.0.6 - 2.0.x (2.0.17 recommended) and Mozzi 1.1.0.  ESP32 core 3.x is not supported by Mozzi 1.x.<br>
* The region can be configured by uncommenting the desired line in the "project_globals.cpp" file, setting myRegion equal to North America or UK.<br><BR>

* Each board should be powered from an external 5 volt DC supply capable of 500mA since each board can draw several hundred mA.<BR>
* USB power can be used for programming and some testing, but the board may not be as stable as when powered externally.<BR>
* Set the power jumper J8 for USB or EXT as needed.<BR><BR>

# Config web page
Browse to the board's IP address (printed on the serial monitor at boot), or http://slim-NUMBER.local/ on computers that support mDNS.  User name **admin**, password **phoneslim** until you change it.  The page shows the board's status and lets you:
* listen to, download (WAV) and delete voicemail messages, and hear or remove the recorded greeting
* change voicemail rings, the reminder ring, Batphone and speed dials (saved immediately)
* set this phone's number and edit the directory (one line per phone: `number ip`, or `number line` for a phone reached over the phone line) - the board restarts
* choose the WiFi network (with a scan), password, and DHCP or a static IP - the board restarts
* change the web password, and restart the board

**Access point mode.** If a board can't join its WiFi network within 20 seconds of starting up, it starts its own access point named PhoneSLIM-xxxx (password = the web password, phoneslim by default) and its blue ESP32 LED blinks.  Join that network with a phone or laptop; most devices open the config page automatically, otherwise browse to http://192.168.4.1/.  Pick the right network, save, and the board restarts and joins it.

The password keeps casual visitors out on your own network; it is plain HTTP, not strong security.  Scanning for networks briefly interrupts WiFi, so don't do it during a call.<br><BR>

# Phone features
All codes are dialed from the phone at dial tone.  Settings are remembered through power cycles.  Rotary (pulse) phones can use voicemail by dialing their own number; the codes starting with * or # need a touch tone phone.

| Dial | Does |
|---|---|
| a number | calls it (VOIP, or over the phone line if its directory entry has no IP) |
| your own number, or \*4 | voicemail |
| #N | speed dial N (0-9) |
| \*2 N number # | store speed dial N.  \*2 N # clears it |
| \*5 N | voicemail answers after N rings.  \*50 turns voicemail off |
| \*6 minutes # | ring once every so many minutes while a message is waiting.  \*60# turns it off |
| \*7 number # | Batphone: picking up the handset dials this number.  \*7# turns it off |
| \*8 N | Batphone waits N seconds of dial tone before dialing (\*80 = right away) |
| \*9 | print settings and messages on the serial monitor |
| \*3 / \*0 / \*1 | echo test / echo test with WiFi off / line noise scan |

Three rising beeps = setting saved, one low tone = not accepted.  Then dial tone comes back.

**Voicemail.** Messages are stored in the ESP32's flash, no SD card needed.  The included partitions.csv (used automatically when it's in the sketch folder) gives about 5 minutes of storage; note that changing the partition layout erases what was stored before.  Unanswered VOIP calls hear the greeting and a beep, then can leave a message of up to 60 seconds.  Waiting messages give a stutter dial tone when you pick up, and (if \*6 is set) a single ring every few minutes; picking up during that ring goes straight to voicemail.  There are no spoken prompts.  The keys are:
* on entry: one high beep per new message, then the oldest new message plays (low beeps = old messages only, long low tone = no messages)
* 1 replay, 2 next, 3 previous, 7 delete, 0 message count
* 5 record your greeting (after the beep; press any key to finish), 6 hear the greeting, 8 go back to the built in greeting
* 9 list messages on the serial monitor, \* back to dial tone

Picking up the phone while a caller is hearing the greeting or leaving a message takes the call.  Voicemail doesn't answer phone line calls, because that audio doesn't go through the ESP32.

**Batphone.** Set two boards to each other's number with \*7.  Picking up either handset rings the other one; if both are picked up at the same time they connect to each other.  Press \* while it's calling to get a normal dial tone instead (touch tone phones), or set a delay with \*8 so there's time to dial something else first.

**Phone line calls.** A directory entry with no IP address is reached over the copper phone line between two boards, as in the original firmware: the call is set up over WiFi by broadcast and the relay connects the lines when it's answered.  A board whose own entry has no IP needs MY_PHONE_NUMBER set in voip_config.h.  Some routers block broadcasts between WiFi clients (guest networks / "AP isolation").<br><BR>

# Troubleshooting call audio
Three built in tests help find where noise comes from:
* **Echo test with WiFi off - dial \*0.**  Same as the echo test below, but the WiFi radio is switched completely off during the test (it reconnects when you hang up).  If the noise is much lower with the radio off, WiFi activity is getting into the audio (see "Isolating call audio noise" below).
* **Echo test - dial your own number.**  Your voice goes through the microphone sampling, encoder, buffer, decoder and earpiece of that one board and back to you about 80 mS late, without using WiFi.  Noisy here = that board's audio hardware or sampling, not the network.
* **Answering machine - call a board and don't pick up.**  After VOIP_ANSWER_MACHINE_RINGS rings (voip_config.h, default 8) the called board plays its recorded "this is a recording" message to the caller over the network, then hangs up.  The message is read from memory, so it contains no microphone noise.  If it sounds clean, the network and the caller's earpiece path are fine and the noise comes from the far board's microphone path.  If it is garbled, look at the network or the caller's playback.  Picking up the phone during the message takes the call.
* **Statistics** - while a call is connected, each board prints a line on the serial monitor (115200) every 2 seconds:<br>
`[stats] out 32768/s  rx 51 fr/s lost 0 late 0  underrun 0  buf 40-78 mS  skip 0 rep 3  rxpeak 9000  | mic 8192/s adc 1200..1600 mean 1400 noise 6.0 clip 0 txdrop 0`
  * `out` should be about 32768.  Much lower means the main loop is too slow to keep the DAC fed, which causes buzzing/whistling regardless of gain settings.
  * `rx` should be about 51 frames/sec.  `lost`, `late` and `underrun` should be 0 or close to it.  If they are high, it is a WiFi/network problem: move the boards closer to the router or raise VOIP_JITTER_MS.
  * `mic` should be about 8192.  `clip` should be 0: if not, the microphone signal is driving the ADC to its limits (distortion).
  * `noise` is the microphone level in ADC counts.  With the handset quiet it shows the hiss floor: under ~5 is good, 10-20+ is audible hiss (raise VOIP_NOISE_GATE, try VOIP_ADC_OVERSAMPLE 2).  It should go up a lot when you talk.
  * A second line `[noise] mic spectrum ...` breaks the microphone noise into bands and lists any tones standing out of it.  Stay quiet during the echo test to read the background noise: high `hum` = low frequency buzz/thumps (power supply, WiFi bursts), high `voice` with no tones = hiss, a listed tone = a whistle at that frequency.
  * A steady whistle that gets louder until it squeals is feedback from the earpiece back into the microphone through the SLIC.  Turn VOIP_MIC_GAIN or VOIP_SPEAKER_GAIN down, or set VOIP_ECHO_SUPPRESS 1.<br><BR>

# Isolating call audio noise
Power: the ESP32 module runs from its own regulator (VIN from VBUS, which comes from the external 5V through D9).  The SLIC, LM358 filters, receive audio bias divider and MT8870 run from the separate LD1117 3.3V regulator, and the LM358 negative rail (VSS) comes from the ICL7660 charge pump running directly from +5V.  The receive audio feeds both the ADC (GPIO32) and the MT8870, so noise on it can cause both noisy calls and missed DTMF digits.

Use the `noise` number in the call stats (handset quiet, during an echo test) to compare these:
* Dial \*0 (WiFi radio off) vs your own number (WiFi on).  A big difference means WiFi activity is getting into the audio, through the shared 5V input, ground, or radiated pickup.
* External power vs USB power (J8), and a different 5V supply.
* Add a capacitor from GPIO32 to ground.  The pin is driven through about 3.3K, so the capacitor forms a low pass filter that keeps noise above 4 kHz from aliasing into the call audio (the ADC samples at 8192 Hz, so anything above 4096 Hz folds back into the voice band, often as a drifting whistle).  On the prototypes 0.1uF dropped the noise floor from ~30 to ~3.5 counts and removed the whistle.  0.1uF puts the corner at ~470 Hz, which muffles speech, so set VOIP_MIC_EQ_HZ in voip_config.h to match the capacitor and the firmware restores the treble.
* Dial \*1 for a line scan: the receive audio is sampled as fast as the ADC allows and the serial monitor lists noise levels and tones up to well above 4 kHz, including the frequency each one is heard at in calls.  Use it to find the real frequency of a whistle and choose the capacitor size.  A busy tone means the scan is done.
* Check VSS with a scope: the ICL7660 switches at ~10kHz and follows any ripple on +5V.  10kHz sampled at 8192Hz shows up as a ~1.8kHz whistle unless the receive filter removes it.

The firmware also decodes DTMF in software from the same audio as a backup to the MT8870 (VOIP_SOFTWARE_DTMF).  The serial monitor shows which decoder caught each key: `[dtmf MT8870]` or `[dtmf sw]`.<br><BR>

# Parts
* The transformer is a 600:600 ohm audio isolation type.  The part used in the prototypes is from Ali Express, such as the red 4 pin ones here with description words such as EL14 included: https://s.click.aliexpress.com/e/_Dmlkp3z <BR><BR>
* PCBs (bare boards or almost-fully assembled) can be found at https://www.pcbway.com/project/shareproject/Phone_Line_Simulator_POTS_Land_Line_1d34078f.html
* I have no idea what PCBWay charges for board parts/assembly, I just shared the project there as an open project.
* PCBWay can't obtain the red 600:600 transformer so if someone has assembled boards made, they'll still have to obtain and solder that part themselves

# Powering Up
* Power up the boards in any order.  The blue LED on the ESP module lights when the board has joined the WiFi router and the RGB LED turns blue when the board is ready for calls.<BR><BR>

* Check out this similar project being worked on: https://github.com/ktownsend-personal/RetroPhone
<br><br>
Shield: [![CC BY-SA 4.0][cc-by-sa-shield]][cc-by-sa]

This work is licensed under a
[Creative Commons Attribution-ShareAlike 4.0 International License][cc-by-sa].

[![CC BY-SA 4.0][cc-by-sa-image]][cc-by-sa]

[cc-by-sa]: http://creativecommons.org/licenses/by-sa/4.0/
[cc-by-sa-image]: https://licensebuttons.net/l/by-sa/4.0/88x31.png
[cc-by-sa-shield]: https://img.shields.io/badge/License-CC%20BY--SA%204.0-lightgrey.svg
