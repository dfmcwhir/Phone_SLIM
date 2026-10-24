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
* Every board must connect to the same WiFi router.  The SSID and Password for the router must be defined by editing the "arduino_secrets.h" file before compiling the sketch.<br>
* Phone numbers are set in the phone directory in "voip_config.h": one line per board with its phone number and IP address.  All boards run identical firmware; each board finds its own entry at boot by its MAC address (then it sets that static IP itself) or, if no MAC is given, by the IP address the router assigns it (give each board a DHCP reservation in the router).  Each board prints its MAC address on the serial monitor at boot.<br>
* When a number is dialed it is matched against the directory and the call is placed to that board's IP address.  Numbers not in the directory get the "this is a recording" message, dialing your own number or a board that is in use gives a busy signal.<br>
* Call audio: the local phone is sampled from the SLIC on GPIO32 (ADC1), and far end audio is played to the SLIC on GPIO25 (DAC1) through Mozzi.  Audio is G.711 u-law, 8192 samples/sec, sent as UDP packets on port 4210.  Microphone/speaker gain, buffering, and an optional echo suppressor are adjustable in "voip_config.h".<br>
* Requires ESP32 Arduino core 1.0.6 - 2.0.x (2.0.17 recommended) and Mozzi 1.1.0.  ESP32 core 3.x is not supported by Mozzi 1.x.<br>
* The region can be configured by uncommenting the desired line in the "project_globals.cpp" file, setting myRegion equal to North America or UK.<br><BR>

* Each board should be powered from an external 5 volt DC supply capable of 500mA since each board can draw several hundred mA.<BR>
* USB power can be used for programming and some testing, but the board may not be as stable as when powered externally.<BR>
* Set the power jumper J8 for USB or EXT as needed.<BR><BR>

# Troubleshooting call audio
Three built in tests help find where noise comes from:
* **Echo test with WiFi off - dial \*0.**  Same as the echo test below, but the WiFi radio is switched completely off during the test (it reconnects when you hang up).  If the noise is much lower with the radio off, it is coming from WiFi current spikes on the 3.3V supply (see "Supply noise" below).
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

# Supply noise
The SLIC, its audio filters (LM358s) and the 15K/7.5K divider that biases the receive audio at ~1.1V all run from the same 3.3V regulator as the ESP32.  WiFi transmit bursts pull current spikes from that rail, and about 1/5 of any ripple on it goes straight into the receive audio through the bias divider.  That audio feeds both the ADC (GPIO32) and the MT8870, so supply noise causes both noisy calls and missed DTMF digits.  Things that help:
* Filter the bias: lift the VCC end of the 15K bias resistor (Audio_Out side of the KS0835F sheet, the divider with the 7.5K to ground) and feed it from VCC through a 1K resistor with a 47-100uF capacitor to ground at the junction.
* Add bulk capacitance (100-470uF plus 0.1uF) on the 3.3V rail near the ESP32 and near the SLIC, and/or power the analog parts from their own regulator.
* The firmware decodes DTMF in software from the same audio as a backup to the MT8870 (VOIP_SOFTWARE_DTMF).  The serial monitor shows which decoder caught each key: `[dtmf MT8870]` or `[dtmf sw]`.<br><BR>

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
