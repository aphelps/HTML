/*******************************************************************************
 * Author: Adam Phelps
 * License: MIT
 * Copyright: 2014
 *
 * Code for a fully contained module which handles HMTL formatted messages
 * from a serial, RS485, XBee, or other connection.   It also functions as a
 * bridge router, retransmitting messages from one connection over any others
 * that are present.
 ******************************************************************************/

#include <Arduino.h>

#include "EEPROM.h"
#include <RS485_non_blocking.h>

#ifdef __AVR__
#include <SoftwareSerial.h>
#endif

#include <HMTLTypes.h>
#include "SPI.h"
#include "FastLED.h"
#include "Wire.h"

#ifndef DEBUG_LEVEL
  #define DEBUG_LEVEL DEBUG_HIGH
#endif
#include "Debug.h"

#include "GeneralUtils.h"
#include "EEPromUtils.h"

#include "HMTLTypes.h"
#include "HMTLMessaging.h"
#include "HMTLPrograms.h"
#include "HMTLProtocol.h"
#include "ProgramManager.h"
#include "MessageHandler.h"

#include "PixelUtil.h"

#include "Socket.h"

#ifdef USE_MPR121
#include "MPR121.h"
#endif

#include "TimeSync.h"
#include "HMTL_Module.h"
#include "Module_Startup_Commands.h"

/* Auto update build number */
#define HMTL_MODULE_BUILD 29 // %META INCR

/*
 * Communications
 */
#ifdef USE_RS485
#include "RS485Utils.h"

// The data size for transmission buffers
#define RS485_SEND_BUFFER_SIZE RS485_BUFFER_TOTAL(64)

RS485Socket rs485;
byte rs485_data_buffer[RS485_BUFFER_TOTAL(RS485_SEND_BUFFER_SIZE)];
#endif

#ifdef USE_XBEE
#include "XBee.h"
#include "XBeeSocket.h"
XBeeSocket xbee;
#define XBEE_SEND_BUFFER_SIZE XBEE_BUFFER_TOTAL(64)
byte xbee_data_buffer[RS485_BUFFER_TOTAL(XBEE_SEND_BUFFER_SIZE)];
#endif

#ifdef USE_RFM69
#include "RFM69.h"
#include "RFM69Socket.h"

// TODO: This should be in the configuration
#define NETWORK 100

#ifndef IRQ_PIN
#define IRQ_PIN 2
#endif

#ifndef IS_RFM69HW
#define IS_RFM69HW true
#endif
// TODO: End of stuff for configuration

RFM69Socket rfm69;
#define RFM69_SEND_BUFFER_SIZE RF69_MAX_DATA_LEN
byte rfm69_data_buffer[RFM69_SEND_BUFFER_SIZE];
#endif

#if defined(ESP32)
#include <WiFiBase.h>
#include <TCPSocket.h>

WiFiBase wfb;
TCPSocket tcpSocket;

#define WIFI_SEND_BUFFER_SIZE TCP_BUFFER_TOTAL(64)
byte wifi_data_buffer[TCP_BUFFER_TOTAL(WIFI_SEND_BUFFER_SIZE)];
#endif

/* Period between updating */
#define STATUS_UPDATE_PERIOD 15000
unsigned long statusUpdateTime = 0;


/*
 * Set the BAUD if not over-riden by build flags
 */
#ifndef BAUD
  #define BAUD 115200
#endif


Socket *sockets[MAX_SOCKETS] = { NULL, NULL };

config_hdr_t config;
output_hdr_t *outputs[HMTL_MAX_OUTPUTS];
config_max_t readoutputs[HMTL_MAX_OUTPUTS];
void *objects[HMTL_MAX_OUTPUTS];

PixelUtil pixels;

/*
 * A timesync object must be defined and initialized here as some libraries
 * require it during initialization.
 */
TimeSync timesync;

/*
 * Data from sensors, set to highest analog value
 */
uint16_t level_data = 1023;
uint16_t light_data = 1023;

byte sound_channels = 0;
#define SOUND_CHANNELS 8
uint16_t sound_data[SOUND_CHANNELS] = { 0, 0, 0, 0, 0, 0, 0, 0 };
boolean sound_updated = false;

/*
 * Program management
 */

/* List of available programs */
hmtl_program_t program_functions[] = {
  { HMTL_PROGRAM_NONE, NULL, NULL},
  { HMTL_PROGRAM_BLINK, program_blink, program_blink_init },
  { HMTL_PROGRAM_TIMED_CHANGE, program_timed_change, program_timed_change_init },
  { HMTL_PROGRAM_FADE, program_fade, program_fade_init },
  { HMTL_PROGRAM_SPARKLE, program_sparkle, program_sparkle_init },
  { HMTL_PROGRAM_CIRCULAR, program_circular, program_circular_init},
  { PROGRAM_BRIGHTNESS, NULL,  program_brightness },
  { PROGRAM_COLOR, NULL, program_color},

  { HMTL_PROGRAM_LEVEL_VALUE, program_level_value, program_level_value_init },
  { HMTL_PROGRAM_SOUND_VALUE, program_sound_value, program_sound_value_init },
  { HMTL_PROGRAM_SOUND_PIXELS, program_sound_pixels, program_sound_pixels_init },

  { PROGRAM_SENSOR_DATA, process_sensor_data, program_sensor_data_init },
};
#define NUM_PROGRAMS (sizeof (program_functions) / sizeof (hmtl_program_t))

program_tracker_t *active_programs[HMTL_MAX_OUTPUTS];
ProgramManager manager;
MessageHandler handler;

#ifdef ENABLE_PUSH_BUTTON
  #define PUSH_BUTTON_PIN 8
#endif

// Prototypes
void additional_setup();
void additional_loop();

void setup() {

  Serial.begin(BAUD);

  int32_t outputs_found = hmtl_setup(&config, readoutputs,
                                     outputs, objects, HMTL_MAX_OUTPUTS,
#ifdef USE_RS485
                                     &rs485,
#else
                                     NULL,
#endif
#ifdef USE_XBEE
                                     &xbee,
#else
                                     NULL,
#endif
                                     &pixels,
                                     NULL, // MPR121
                                     NULL, // RGB
                                     NULL, // Value
                                     NULL);

  byte num_sockets = 0;

#ifdef USE_RS485
  if (outputs_found & (1 << HMTL_OUTPUT_RS485)) {
    /* Setup the RS485 socket */
    rs485.setup();
    rs485.initBuffer(rs485_data_buffer, RS485_SEND_BUFFER_SIZE);
    sockets[num_sockets++] = &rs485;
  }
#endif

#ifdef USE_XBEE
  if (outputs_found & (1 << HMTL_OUTPUT_XBEE)) {
    /* Setup the XBee socket */
    xbee.setup();
    xbee.initBuffer(xbee_data_buffer, XBEE_SEND_BUFFER_SIZE);
    sockets[num_sockets++] = &xbee;
  }
#endif

#ifdef USE_RFM69
  if (true) {
    // XXX: RFM69!
    rfm69.init(config.address, NETWORK, IRQ_PIN, IS_RFM69HW, RF69_915MHZ); // TODO: Put this in config
    rfm69.setup();
    rfm69.initBuffer(rfm69_data_buffer, RFM69_SEND_BUFFER_SIZE);
    sockets[num_sockets++] = &rfm69;
  }
#endif

#if defined(ESP32)
  if (true) { // TODO: Should any of this be put into the stored config?
    /* Startup a connection and add a wifi socket */
    wfb = WiFiBase();
    wfb.configureAccessPoint("HMTL_Module", "12345678");
    wfb.startup();

    wfb.addRESTEndpoint("/clear", clearHandler, "\"description\":\"clear all patterns\"");
    wfb.addRESTEndpoint("/rgb", rgbHandler,
                        "\"description\":\"set to a single color\",\"args\":[\"r\",\"g\",\"b\"]");
    wfb.addRESTEndpoint("/circular", circularHandler,
                        "\"description\":\"run circular pattern\",\"args\":[\"pattern\",\"length\",\"period\"]");
    wfb.addRESTEndpoint("/sparkle", sparkleHandler,
                        "\"description\":\"run sparkle pattern\",\"args\":[\"r\",\"g\",\"b\",\"threshold\",\"bg\",\"hum_min\",\"hue_max\",\"sat_min\",\"sat_max\",\"val_min\",\"val_max\"]");

    wfb.addRESTEndpoint("/RGB", rgbColorPicker, "");

    tcpSocket.init(config.address, HMTL_PORT);
    tcpSocket.initBuffer(wifi_data_buffer, WIFI_SEND_BUFFER_SIZE);
    tcpSocket.setup();
    sockets[num_sockets++] = &tcpSocket;
  }
#endif

  if (num_sockets == 0) {
    DEBUG_ERR("No sockets configured");
    DEBUG_ERR_STATE(2);
  }

  /* Setup the program manager */
  manager = ProgramManager(outputs, active_programs, objects, HMTL_MAX_OUTPUTS,
                           program_functions, NUM_PROGRAMS);

  handler = MessageHandler(config.address, &manager, sockets, num_sockets);

  /* Perform any additional setup that's required */
  additional_setup();

  DEBUG2_VALUELN("HMTL Module initialized, v", HMTL_MODULE_BUILD);

  // Indicate that module is ready for action
  Serial.println(F(HMTL_READY));
}

void additional_setup() {
#ifdef ENABLE_PUSH_BUTTON
  pinMode(PUSH_BUTTON_PIN, INPUT_PULLUP);
#endif

#ifdef STARTUP_COMMANDS
  startup_commands(&manager, &handler, sockets, &config);
#endif
}

#ifdef ENABLE_PUSH_BUTTON
byte get_button_value() {
  return digitalRead(PUSH_BUTTON_PIN);
}
#endif

#if defined(ESP32)

void clearHandler() {
  WebServer *server = wfb.getServer();

  DEBUG3_PRINTLN("/clear");

  hmtl_program_cancel_fmt(sockets[0]->send_buffer,
                          sockets[0]->send_data_size,
                          config.address, HMTL_ALL_OUTPUTS);
  msg_hdr_t *msg = (msg_hdr_t *)sockets[0]->send_buffer;
  handler.process_msg(msg, sockets[0], NULL, &config);

  server->send(200, "application/json", "ok");
}

void circularHandler() {
  WebServer *server = wfb.getServer();

  uint8_t pattern = 0;
  if (server->hasArg("pattern")) {
    pattern = server->arg("pattern").toInt();
  }

  uint16_t length = pixels.numPixels() / 8;
  if (server->hasArg("length")) {
    length = server->arg("length").toInt();
  }

  uint16_t period = 25;
  if (server->hasArg("period")) {
    period = server->arg("period").toInt();
  }

  DEBUG3_VALUE("/circular pattern=", pattern);
  DEBUG3_VALUE(" length=", length);
  DEBUG3_VALUELN(" period=", period);

  byte output = manager.lookup_output_by_type(HMTL_OUTPUT_PIXELS);
  if (output != HMTL_NO_OUTPUT) {
    DEBUG4_VALUELN("/circular: output ", output);
    program_circular_fmt(sockets[0]->send_buffer, sockets[0]->send_data_size,
                         config.address, output,
                         period, length, CRGB::Black,
                         pattern, 0);
    msg_hdr_t *msg = (msg_hdr_t *)sockets[0]->send_buffer;
    handler.process_msg(msg, sockets[0], NULL, &config);
  }

  DEBUG3_PRINTLN("/circular done");

  server->send(200, "application/json", "ok");
}

void sparkleHandler() {
  WebServer *server = wfb.getServer();

  uint16_t period = 50;
  if (server->hasArg("period")) {
    period = server->arg("period").toInt();
  }

  uint8_t r = 0, g = 0, b = 0;
  if (server->hasArg("r")) r = server->arg("r").toInt();
  if (server->hasArg("g")) b = server->arg("g").toInt();
  if (server->hasArg("b")) r = server->arg("b").toInt();
  CRGB bgColor = CRGB(r, g, b);

  uint8_t sparkle_threshold = 0;
  if (server->hasArg("threshold"))
    sparkle_threshold = server->arg("threshold").toInt();

  uint8_t bg_threshold = 0;
  if (server->hasArg("bg"))
    bg_threshold = server->arg("bg").toInt();

  uint8_t hue_min = 0;
  if (server->hasArg("hue_min"))
    hue_min = server->arg("hue_min").toInt();

  uint8_t hue_max = 255;
  if (server->hasArg("hue_max"))
    hue_max = server->arg("hue_max").toInt();

  uint8_t sat_min = 0;
  if (server->hasArg("sat_min"))
    sat_min = server->arg("sat_min").toInt();

  uint8_t sat_max = 255;
  if (server->hasArg("sat_max"))
    sat_max = server->arg("sat_max").toInt();

  uint8_t val_min = 0;
  if (server->hasArg("val_min"))
    val_min = server->arg("val_min").toInt();

  uint8_t val_max = 255;
  if (server->hasArg("val_max"))
    val_max = server->arg("val_max").toInt();

  DEBUG3_VALUE("/sparkle period=", period);
  DEBUG3_VALUE("/sparkle threshold=", sparkle_threshold);
  DEBUG3_VALUE("/sparkle bg=", bg_threshold);
  DEBUG3_VALUE("/sparkle hue_min=", hue_min);
  DEBUG3_VALUE("/sparkle hue_max=", hue_max);
  DEBUG3_VALUE("/sparkle sat_min=", sat_min);
  DEBUG3_VALUE("/sparkle sat_max=", sat_max);
  DEBUG3_VALUE("/sparkle val_min=", val_min);
  DEBUG3_VALUELN("/sparkle val_max=", val_max);

  byte output = manager.lookup_output_by_type(HMTL_OUTPUT_PIXELS);
  if (output != HMTL_NO_OUTPUT) {
    DEBUG4_VALUELN("/sparkle: output ", output);
    program_sparkle_fmt(sockets[0]->send_buffer, sockets[0]->send_data_size,
                        config.address, output,
                        period,
                        bgColor,
                        sparkle_threshold, bg_threshold,
                        hue_min, hue_max, sat_min, sat_max, val_min, val_max);

    msg_hdr_t *msg = (msg_hdr_t *)sockets[0]->send_buffer;
    handler.process_msg(msg, sockets[0], NULL, &config);
  }

  DEBUG3_PRINTLN("/sparkle done");

  server->send(200, "application/json", "ok");
}


void rgbHandler() {
  WebServer *server = wfb.getServer();

  uint8_t rgb[3];
  rgb[0] = server->hasArg("r") ? server->arg("r").toInt() : 0;
  rgb[1] = server->hasArg("g") ? server->arg("g").toInt() : 0;
  rgb[2] = server->hasArg("b") ? server->arg("b").toInt() : 0;

  DEBUG3_VALUE("/rgb r:", rgb[0]);
  DEBUG3_VALUE(" g:", rgb[1]);
  DEBUG3_VALUELN(" b:", rgb[2]);

  byte output = manager.lookup_output_by_type(HMTL_OUTPUT_PIXELS);
  if (output != HMTL_NO_OUTPUT) {
    DEBUG4_VALUELN("/rgb output:", output);
    hmtl_set_output_rgb(outputs[output], objects[output], rgb);
    hmtl_update_output(outputs[output], objects[output]);
  }

  DEBUG3_PRINTLN("/rgb done");

  server->send(200, "application/json", "ok");
}

void rgbColorPicker() {
  WebServer *server = wfb.getServer();

  String data =
          "<!DOCTYPE html><html>\n"
          "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
          "<link rel=\"icon\" href=\"data:,\">\n"
          "<link rel=\"stylesheet\" href=\"https://stackpath.bootstrapcdn.com/bootstrap/4.3.1/css/bootstrap.min.css\">\n"
          "<script src=\"https://cdnjs.cloudflare.com/ajax/libs/jscolor/2.0.4/jscolor.min.js\"></script>\n"
          "</head>\n"
          "<body><div class=\"container\"><div class=\"row\"><h1>Select color</h1></div>\n"
//          "<a class=\"btn btn-primary btn-lg\" href=\"#\" id=\"change_color\" role=\"button\">Change Color</a> \n"
          "<input class=\"jscolor {onFineChange:'update(this)'}\" id=\"rgb\"></div>\n"
          "<script>\n"
            "function update(picker) {\n"
              "document.getElementById('rgb').innerHTML = Math.round(picker.rgb[0]) + ', ' +  Math.round(picker.rgb[1]) + ', ' + Math.round(picker.rgb[2]);\n"
              "var xhttp = new XMLHttpRequest();\n"
              "xhttp.open(\"POST\", \"/rgb?r=\" + Math.round(picker.rgb[0]) + \"&g=\" + Math.round(picker.rgb[1]) + \"&b=\" + Math.round(picker.rgb[2]), true);\n"
              "xhttp.setRequestHeader(\"Content-type\", \"application/json\");\n"
              "xhttp.send(\"\");\n"
            "}\n"
          "</script>\n"
          "</body></html>\n"
          "\n";

  server->send(200, "text/html", data);

  DEBUG3_PRINTLN("/RGB done");
}

#endif

void status_update() {
  DEBUG3_PRINTLN("Status:");
  DEBUG3_VALUELN(" * uptime:", millis())

#if defined(ESP32)
  DEBUG3_VALUE(" * wifi connected:", wfb.connected());
  DEBUG3_VALUE(" ssid:", WiFi.SSID());
  DEBUG3_VALUE(" ip:", WiFi.localIP().toString());
  DEBUG3_VALUE(" ap_ip:", WiFi.softAPIP().toString());
#endif
}

#define MSG_MAX_SZ (sizeof(msg_hdr_t) + sizeof(msg_max_t))
byte msg[MSG_MAX_SZ];
byte serial_offset = 0;
boolean first_run = true;

/*******************************************************************************
 * The main event loop
 *
 * - Checks for and handles messages over all interfaces
 * - Runs any enabled programs
 * - Updates any outputs
 */
void loop() {
  // Check and send a serial-ready message if needed
  handler.serial_ready();

  /*
   * Check the serial device and all sockets for messages, forwarding them and
   * processing them if they are for this module.
   */
  boolean update = handler.check(&config);

  additional_loop();

  /* Execute any active programs */
  if (manager.run()) {
    update = true;
  }

  /* If this is the first execution then update to set initial values */
  if (first_run) {
    update = true;
    first_run = false;
  }

  if (update) {
    /* Update the outputs */
    for (byte i = 0; i < config.num_outputs; i++) {
      hmtl_update_output(outputs[i], objects[i]);
    }
  }

  unsigned long now = millis();
  if (now - statusUpdateTime > STATUS_UPDATE_PERIOD) {
    status_update();
    statusUpdateTime = now;
  }

#if defined(ESP32)
  wfb.checkServer();
#endif
}

void additional_loop() {
#ifdef ENABLE_PUSH_BUTTON
  //Use a push button to override a particular output
  static byte prev_value = HIGH;
  if (get_button_value() != prev_value) {
    prev_value = get_button_value();
    DEBUG1_VALUELN("Push button to ", prev_value);
    if (prev_value == LOW) {
      uint32_t value = (prev_value == LOW ? pixel_color(255,255,255) : 0);
      hmtl_program_timed_change_fmt(sockets[0]->send_buffer,
                             sockets[0]->send_data_size,
                             config.address, (byte)3,
                             500, value,
                             0);
      handler.process_msg((msg_hdr_t *)sockets[0]->send_buffer, sockets[0], NULL, &config);

    }
  }
#endif
}

/*******************************************************************************
 * Program handler for processing incoming sensor data
 */

boolean program_sensor_data_init(msg_program_t *msg,
                                 program_tracker_t *tracker,
                                 output_hdr_t *output, void *object,
                                 ProgramManager *managet) {
  DEBUG3_PRINTLN("Initializing sensor data handler");
  return true;
}

boolean process_sensor_data(output_hdr_t *output,
                            void *object,
                            program_tracker_t *tracker) {
  msg_sensor_data_t *sensor = (msg_sensor_data_t *)object;
  void *data = (void *)&sensor->data;

  switch (sensor->sensor_type) {
    case HMTL_SENSOR_SOUND: {
      byte copy_len;
      if (sizeof (uint16_t) * SOUND_CHANNELS < sensor->data_len) {
        // TODO: This indicates that more data was returned than is expected
        copy_len = sizeof (uint16_t) * SOUND_CHANNELS;
      } else {
        copy_len = sensor->data_len;
      }

      memcpy(sound_data, data, copy_len);
      sound_channels = sensor->data_len / (byte)sizeof (uint16_t);
      DEBUG4_COMMAND(
                     DEBUG4_VALUE(" SOUND:", sound_channels);
                     for (byte i = 0; i < sound_channels; i++) {
                       DEBUG4_HEXVAL(" ", sound_data[i]);
                     }
                     );
      sound_updated = true;
      return true;
    }
    case HMTL_SENSOR_LIGHT: {
      light_data = *(uint16_t *)data;
      DEBUG4_VALUE(" LIGHT:", light_data);
      return true;
    }
    case HMTL_SENSOR_POT: {
      level_data = *(uint16_t *)data;
      DEBUG4_VALUE(" LEVEL:", level_data);
      return true;
    }
    default: {
      DEBUG1_PRINT(" ERROR: UNKNOWN TYPE");
      return false;
    }
  }
}




/*******************************************************************************
 * Program to set the value level based on the most recent sensor data
 */
typedef struct {
  uint16_t value;
} state_level_value_t;

boolean program_level_value_init(msg_program_t *msg,
                                 program_tracker_t *tracker,
                                 output_hdr_t *output, void *object,
                                 ProgramManager *manager) {
  if ((output == NULL) || !IS_HMTL_RGB_OUTPUT(output->type)) {
    return false;
  }

  DEBUG3_PRINTLN("Initializing level value state");

  state_level_value_t *state =
          (state_level_value_t *)manager->get_program_state(tracker,
                                                            sizeof (state_level_value_t));
  state->value = 0;

  return true;
}

boolean program_level_value(output_hdr_t *output, void *object,
                            program_tracker_t *tracker) {
  state_level_value_t *state = (state_level_value_t *)tracker->state;
  if (state->value != level_data) {
    state->value = level_data;
    uint8_t mapped = map(level_data, 0, 1023, 0, 255);
    uint8_t values[3] = {mapped, mapped, mapped};
    hmtl_set_output_rgb(output, object, values);

    DEBUG3_VALUELN("Level value:", mapped);

    return true;
  }

  return false;
}


/*******************************************************************************
 * Program to set the value of a given output based on the most recent sound
 * data.
 */

typedef struct {
  uint16_t value;
  uint32_t max;
} state_sound_value_t;

boolean program_sound_value_init(msg_program_t *msg,
                                 program_tracker_t *tracker,
                                 output_hdr_t *output, void *object,
                                 ProgramManager *manager) {
  if ((output == NULL) || !IS_HMTL_RGB_OUTPUT(output->type)) {
    return false;
  }

  DEBUG3_PRINTLN("Initializing sound value state");

  state_sound_value_t *state =
          (state_sound_value_t *)manager->get_program_state(tracker,
                                                            sizeof (state_sound_value_t));
  state->value = 0;
  state->max = 0;

  return true;
}

boolean program_sound_value(output_hdr_t *output, void *object,
                            program_tracker_t *tracker) {
  state_sound_value_t *state = (state_sound_value_t *)tracker->state;

  uint32_t total = 0;
  for (int i = 0; i < sound_channels; i++) {
    total += sound_data[i];
  }
  if (total > state->max) {
    state->max = total;
    DEBUG3_VALUELN("Sound max:", total);
  }

  DEBUG3_VALUE("Check out:", output->output);
  DEBUG3_VALUELN(" value:", total);

  uint8_t mapped = 0;
  if (total > 2) {
    // To avoid noise while quiet set a minimum
    mapped = map(total, 0, state->max, 0, 255);
  }
  if (mapped != state->value) {
    state->value = mapped;

    uint8_t values[3] = {mapped, mapped, mapped};
    hmtl_set_output_rgb(output, object, values);

    return true;
  }

  return false;
}

/*******************************************************************************
 * Program which uses the columns in the sound data to light individual pixels.
 * This is intended for separate leds controlled as the individual pixels, rather than
 * RGB pixels.
 */

typedef struct {
  uint16_t num_leds;
} program_sound_pixels_t;

typedef struct {
  program_sound_pixels_t msg;
  uint16_t max[SOUND_CHANNELS];
} state_sound_pixels_t;

boolean program_sound_pixels_init(msg_program_t *msg,
                                  program_tracker_t *tracker,
                                  output_hdr_t *output, void *object,
                                  ProgramManager *manager) {
  if ((output == NULL) || !IS_HMTL_PIXEL_OUTPUT(output->type)) {
    return false;
  }

  DEBUG4_PRINT("Initializing sound pixels:");
  state_sound_pixels_t *state =
          (state_sound_pixels_t *)manager->get_program_state(tracker,
                                                             sizeof (state_sound_pixels_t));
  memcpy(&state->msg, msg->values, sizeof (state->msg));
  memset(state->max, 0, sizeof(uint16_t) * SOUND_CHANNELS);

  DEBUG4_VALUE(" chans:", SOUND_CHANNELS);
  DEBUG4_VALUELN(" leds:", state->msg.num_leds);

  return true;
}

boolean program_sound_pixels(output_hdr_t *output, void *object,
                            program_tracker_t *tracker) {
  PixelUtil *pixels = (PixelUtil*)object;
  state_sound_pixels_t *state = (state_sound_pixels_t *)tracker->state;

  if (sound_updated) {
#if 0
    // TODO: Beginning code for sound reactive high-power pixels
    for (uint16_t led = 0; led < state->msg.num_leds; led++) {
      /*
       * Example: 3 channels, 8 LEDs
       * led   low  high  distance
       *   0   0    0     0
       *   1   0    1
       *   2   0    1
       *   3   0    1
       *   4   1    2
       *   5   1    2
       *   6   1    2
       *   7   2    2     0
       *
       *   How to compute the distance?
       *   - Convert channels to float [0,1]
       *   - Convert led to float [0,1]
       *   - Determine which channel is above and below....
       */

      // Interpolate the value
      uint8_t low_channel;
      uint8_t high_channel;
      uint8_t distance;


    }
#else
    for (byte channel = 0; channel < SOUND_CHANNELS; channel++) {

      // Track the maximum value for each channel
      if (sound_data[channel] > state->max[channel]) {
        state->max[channel] = sound_data[channel];
      }

      // Map the value of the channel compared to its max into a byte
      uint8_t value = (uint8_t)map(sound_data[channel],
                                   0, state->max[channel],
                                   0, 255);

      // Set the led value for this channel
      pixels->setDistinct(channel, value);
    }
#endif

    sound_updated = false;
    return true;
  }

  return false;
}
