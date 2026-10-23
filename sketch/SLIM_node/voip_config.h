/*
  voip_config.h
  User settings for calling between SLIM nodes over WiFi (VOIP).

  NOTE: the WiFi network, IP address, this board's number and the directory
  below are only first-boot defaults.  After that, change them on the board's
  config web page (browse to its IP address - see README).

  Every node runs the same firmware.  At boot each node looks itself up in
  phoneDirectory[] to learn its own phone number:
    1) If an entry's "mac" matches this ESP32's WiFi MAC address, the node
       takes that entry's number AND configures that entry's IP as a static IP.
    2) Otherwise the node gets an address from the router (DHCP) and uses the
       entry whose "ip" matches the address it was given.  In this case give
       the board a DHCP reservation in your router so the address never changes.
  The MAC address of each board is printed on the serial monitor at boot.

  Dialing: as digits are dialed they are compared against the directory.
  As soon as the dialed digits match a number, a call is placed to that IP.

  Phone line calls: leave an entry's IP empty ("") to reach that number over
  the copper phone line between two boards instead of VOIP, like the original
  firmware.  The call is still set up over WiFi (by broadcast, so that board's
  IP doesn't matter) and the relay connects the two phone lines when answered.
  Voicemail only works for VOIP calls (phone line audio doesn't pass through
  the ESP32).

  A board whose own entry has no IP should set MY_PHONE_NUMBER below.
*/
#ifndef voip_config_h
#define voip_config_h

struct PhoneDirectoryEntry {
  const char* number;   // phone number dialed to reach this node
  const char* ip;       // IP address of the node on your LAN
  const char* mac;      // optional: node's WiFi MAC "AA:BB:CC:DD:EE:FF" (empty = use DHCP)
};

// ---------------------- phone directory ----------------------
// add one line per module.  Numbers may be any length, but one number
// should not be the beginning of another (eg "555" and "5551234")
static const PhoneDirectoryEntry phoneDirectory[] = {
  // number      IP address          MAC address (optional)
  { "8675309",   "192.168.1.201",    "" },
  { "3927704",   "192.168.1.202",    "" },
  // { "5551212", "192.168.1.203",   "" },
  // { "5550100", "",                "" },   // reached over the phone line
};

// this board's phone number.  Leave "" to look it up in phoneDirectory by MAC
// or IP address.  Set it if this board's directory entry has no IP.
#ifndef MY_PHONE_NUMBER
#define MY_PHONE_NUMBER   ""
#endif

// network settings used only when a node assigns itself a static IP (MAC match)
#define VOIP_GATEWAY      "192.168.1.1"
#define VOIP_SUBNET       "255.255.255.0"
#define VOIP_DNS          "192.168.1.1"

// UDP port used for call signaling and audio (must be the same on all nodes)
#define VOIP_UDP_PORT     4210

// ---------------------- audio tuning ----------------------
// gain applied to the local phone's microphone before sending
// (1.0 = 12 bit ADC scaled straight to 16 bit audio).  Raise if the far end
// hears you too quietly, lower if you sound distorted.
#define VOIP_MIC_GAIN       2.0f
// gain applied to audio received from the far end before playing it (1.0 = unity)
#define VOIP_SPEAKER_GAIN   1.0f

// milliseconds of received audio to buffer before playback starts.
// raise this if audio is choppy on a busy WiFi network (adds delay)
#define VOIP_JITTER_MS      60

// noise gate: learns the background noise level and quiets the microphone
// whenever nobody is talking, removing hiss during pauses.  1 = on, 0 = off.
#define VOIP_NOISE_GATE     1

// ADC input range for the microphone signal (GPIO32): 0, 2, 6 or 11 dB.
// The SLIC audio sits at about 1.1 V, so 6 dB (0.15 - 1.75 V) gives ~1.8x
// more resolution than 11 dB.  If the stats show "clip" above 0, use 11.
#define VOIP_ADC_ATTEN_DB   6

// if you add a capacitor from GPIO32 to ground to filter noise (recommended),
// set this to the filter's corner frequency so the firmware can restore the
// treble it removes.  The pin is driven through about 3.3K, so:
//   0.1uF -> 470    47nF -> 1000    33nF -> 1450    22nF -> 2200
//   10nF or none -> 0 (off)
#define VOIP_MIC_EQ_HZ      470

// read the ADC this many times per audio sample and average (1, 2 or 4).
// reduces ADC hiss but uses more CPU on core 0.
#define VOIP_ADC_OVERSAMPLE 1

// echo suppression: turns the local microphone down while the far end is
// talking unless the local person is clearly louder than the echo would be.
// Breaks whistling/howling feedback loops through the SLIC hybrid.  0 = off.
#define VOIP_ECHO_SUPPRESS  1
#define VOIP_ECHO_RATIO     1.5f

// decode DTMF key presses in software from the same audio the MT8870 hears,
// as a backup for missed key presses.  1 = on, 0 = off (MT8870 only).
#define VOIP_SOFTWARE_DTMF  1

// WiFi power save.  0 = radio always on (lowest audio delay).  1 = radio
// sleeps between beacons: less supply noise but more delay - for testing
// whether WiFi current spikes are causing the noise.
#define VOIP_WIFI_POWERSAVE 0

// ---------------------- voicemail ----------------------
// these are the defaults used the first time; after that they're changed from
// the phone with * codes (see README) and remembered through power cycles.
#define VM_RINGS_DEFAULT             8   // rings before voicemail answers (0 = off)       *5
#define VM_REMINDER_MINUTES_DEFAULT  5   // ring once every N minutes while a message waits (0 = off)  *6
#define VM_MAX_MESSAGES              20
#define VM_MAX_MESSAGE_SEC           60  // longest message a caller can leave
#define VM_MAX_GREETING_SEC          30

// ---------------------- Batphone ----------------------
// a Batphone dials one number automatically when its handset is picked up.
// Set two boards to each other's number for a private line.  Defaults for
// first boot; change from the phone with *7 and *8.
#define BATPHONE_NUMBER_DEFAULT      ""  // "" = normal phone
#define BATPHONE_DELAY_DEFAULT       3   // seconds of dial tone first (0 = dial immediately)

// ---------------------- config web page ----------------------
// browse to the board's IP address (or http://slim-<number>.local/) to change
// settings.  User name "admin".  If the board can't join the WiFi network
// within VOIP_WIFI_CONNECT_SECONDS it starts its own access point named
// PhoneSLIM-xxxx (password = the web password, must be 8+ characters) and
// the page is at http://192.168.4.1/
#define WEB_PASSWORD_DEFAULT       "phoneslim"
#define VOIP_AP_SSID_PREFIX        "PhoneSLIM-"
#ifndef VOIP_WIFI_CONNECT_SECONDS
#define VOIP_WIFI_CONNECT_SECONDS  20
#endif

// ---------------------- diagnostics ----------------------
// print an audio/network statistics line on the serial monitor every 2 seconds
// while a call is connected.  See README "Troubleshooting call audio".
#define VOIP_STATS          1

#endif
