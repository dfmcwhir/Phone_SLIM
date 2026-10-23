/*
  webconfig.h
  Config web page: browse to the board's IP address (user "admin", password
  from settings, default in voip_config.h).  Runs in its own task on core 0 so
  serving pages never interrupts call audio.
*/
#ifndef webconfig_h
#define webconfig_h

void webBegin();     // start the web server task (after voipBegin)

#endif
