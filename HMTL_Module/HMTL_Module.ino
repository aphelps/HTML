/*******************************************************************************
 * Author: Adam Phelps
 * License: MIT
 * Copyright: 2014
 *
 * Code for a fully contained module which handles HMTL formatted messages
 * from a serial, RS485, or XBee connection.   It also functions as a bridge
 * router, retransmitting messages from one connections over any others that
 * are present.
 ******************************************************************************/

#include "EEPROM.h"
#include <RS485_non_blocking.h>
#include <SoftwareSerial.h>
#include "SPI.h"
#include "FastLED.h"
#include "Wire.h"
#include "XBee.h"

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
#include "RS485Utils.h"
#include "XBeeSocket.h"
#include "MPR121.h"

#include "TimeSync.h"

/* Auto update build number */
#define HMTL_MODULE_BUILD 29 // %META INCR

/*
 * Communications
 */

#define SEND_BUFFER_SIZE 64 // The data size for transmission buffers

RS485Socket rs485;
byte rs485_data_buffer[RS485_BUFFER_TOTAL(SEND_BUFFER_SIZE)];

XBeeSocket xbee;
byte xbee_data_buffer[RS485_BUFFER_TOTAL(SEND_BUFFER_SIZE)];

Socket *serial_socket = NULL;

#define NUM_SOCKETS 2
Socket *sockets[NUM_SOCKETS] = { NULL, NULL };

config_hdr_t config;
output_hdr_t *outputs[HMTL_MAX_OUTPUTS];
config_max_t readoutputs[HMTL_MAX_OUTPUTS];
void *objects[HMTL_MAX_OUTPUTS];

PixelUtil pixels;



/*
 * Data from sensors, set to highest analog value
 */
uint16_t level_data = 1023;
uint16_t light_data = 1023;

byte sound_channels = 0;
#define SOUND_CHANNELS 8
uint16_t sound_data[SOUND_CHANNELS] = { 0, 0, 0, 0, 0, 0, 0, 0 };

// Time synchronization
TimeSync time = TimeSync();

/*
 * Program management
 */

/* List of available programs */
hmtl_program_t program_functions[] = {
  { HMTL_PROGRAM_NONE, NULL, NULL},
  { HMTL_PROGRAM_BLINK, program_blink, program_blink_init },
  { HMTL_PROGRAM_TIMED_CHANGE, program_timed_change, program_timed_change_init },
  { HMTL_PROGRAM_LEVEL_VALUE, program_level_value, program_level_value_init },
  { HMTL_PROGRAM_SOUND_VALUE, program_sound_value, program_sound_value_init },
  { HMTL_PROGRAM_FADE, program_fade, program_fade_init },
  { PROGRAM_SENSOR_DATA, process_sensor_data, NULL }
};
#define NUM_PROGRAMS (sizeof (program_functions) / sizeof (hmtl_program_t))

program_tracker_t *active_programs[HMTL_MAX_OUTPUTS];
ProgramManager manager;
MessageHandler handler;

void setup() {

  //  Serial.begin(115200);
  Serial.begin(57600);

  int32_t outputs_found = hmtl_setup(&config, readoutputs,
                                     outputs, objects, HMTL_MAX_OUTPUTS,
                                     &rs485, 
                                     &xbee,
                                     &pixels, 
                                     NULL, // MPR121
                                     NULL, // RGB
                                     NULL, // Value
                                     NULL);

  if (!(outputs_found & (1 << HMTL_OUTPUT_RS485))) {
    DEBUG_ERR("No RS485 config found");
    DEBUG_ERR_STATE(1);
  }

  if (outputs_found & (1 << HMTL_OUTPUT_RS485)) {
    /* Setup the RS485 connection */  
    rs485.setup();
    rs485.initBuffer(rs485_data_buffer, SEND_BUFFER_SIZE);
    serial_socket = &rs485;
    sockets[0] = &rs485;
  }

  if (outputs_found & (1 << HMTL_OUTPUT_XBEE)) {
    /* Setup the RS485 connection */  
    xbee.setup();
    xbee.initBuffer(xbee_data_buffer, SEND_BUFFER_SIZE);
    serial_socket = &xbee;
    sockets[1] = &xbee;
  }

  if (serial_socket == NULL) {
    DEBUG_ERR("No sockets configured");
    DEBUG_ERR_STATE(2);
  }

  /* Setup the program manager */
  manager = ProgramManager(outputs, active_programs, objects, HMTL_MAX_OUTPUTS,
                           program_functions, NUM_PROGRAMS);

  handler = MessageHandler(config.address, &manager, sockets, NUM_SOCKETS);

  DEBUG2_VALUELN("HMTL Module initialized, v", HMTL_MODULE_BUILD);
  Serial.println(F(HMTL_READY));

  //startup_commands();
}

#if 0
/*******************************************************************************
 * Execute any startup commands
 */
void startup_commands() {
  // TODO: These should be stored in EEPROM

  #define STARTUP_MSGS 4
  msg_hdr_t *startupmsg[STARTUP_MSGS];

  for (byte i = 0; i < STARTUP_MSGS; i++) {
    byte length = sizeof (msg_hdr_t) + sizeof (msg_program_t);
    startupmsg[i] = (msg_hdr_t *)malloc(length);

    startupmsg[i]->version = HMTL_MSG_VERSION;
    startupmsg[i]->type = MSG_TYPE_OUTPUT;
    startupmsg[i]->flags = 0;
    startupmsg[i]->address = 0; // XXX: This address?

#if 1
    msg_value_t *value = (msg_value_t *)(startupmsg[i] + 1);
    value->hdr.type = HMTL_OUTPUT_VALUE;
    value->hdr.output = i;
    value->value = 128;
#else
    msg_program_t *program = (msg_program_t *)(startupmsg[i] + 1);
    programs->hdr.type = HMTL_OUTPUT_PROGRAM;
    programs->hdr.output = i;
    programs->type = ;
#endif
  }

  /* 
   * Process the startup commands, forwarding them if they're broadcast or
   * to some other address, and then apply them locally
   */
  for (byte i = 0; i < STARTUP_MSGS; i++) {
    boolean forwarded = false;
    if (has_rs485) {
      forwarded = check_and_forward(startupmsg[i], &rs485);
    }
    if (has_xbee) {
      forwarded = check_and_forward(startupmsg[i], &xbee);
    }

    handler.process_msg(startupmsg[i], NULL, serial_socket, &config);
  }

  /* Free the startup messages */
  for (byte i = 0; i < STARTUP_MSGS; i++) {
    free(startupmsg[i]);
  }
}
#endif

#define MSG_MAX_SZ (sizeof(msg_hdr_t) + sizeof(msg_max_t))
byte msg[MSG_MAX_SZ];
byte serial_offset = 0;


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

  /* Execute any active programs */
  if (manager.run()) {
    update = true;
  }

  if (update) {
    /* Update the outputs */
    for (byte i = 0; i < config.num_outputs; i++) {
      hmtl_update_output(outputs[i], objects[i]);
    }
  }
}


/*
 * Record relevant sensor data for programatic usage
 */
boolean process_sensor_data(output_hdr_t *output,
                            void *object,
                            program_tracker_t *tracker) {
  msg_sensor_data_t *sensor = (msg_sensor_data_t *)object;
  void *data = (void *)&sensor->data;

  switch (sensor->sensor_type) {
    case HMTL_SENSOR_SOUND: {
      byte copy_len;
      if (sizeof (uint16_t) * SOUND_CHANNELS < sensor->data_len) {
        copy_len = sizeof (uint16_t) * SOUND_CHANNELS;
      } else {
        copy_len = sensor->data_len;
      }

      memcpy(sound_data, data, copy_len);
      sound_channels = sensor->data_len / sizeof (uint16_t);
      DEBUG4_COMMAND(
                     DEBUG4_VALUE(" SOUND:", sound_channels);
                     for (byte i = 0; i < sound_channels; i++) {
                       DEBUG4_HEXVAL(" ", sound_data[i]);
                     }
                     );
      return true;
      break;
    }
    case HMTL_SENSOR_LIGHT: {
      light_data = *(uint16_t *)data;
      DEBUG4_VALUE(" LIGHT:", light_data);
      return true;
      break;
    }
    case HMTL_SENSOR_POT: {
      level_data = *(uint16_t *)data;
      DEBUG4_VALUE(" LEVEL:", level_data);
      return true;
      break;
    }
    default: {
      DEBUG1_PRINT(" ERROR: UNKNOWN TYPE");
      return false;
      break;
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
                                 program_tracker_t *tracker) {
  DEBUG3_PRINTLN("Initializing level value state");

  state_level_value_t *state = (state_level_value_t *)malloc(sizeof (state_level_value_t));
  state->value = 0;
  
  tracker->state = state;

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
 * Program to set the value based on the most recent sound data
 */

typedef struct {
  uint16_t value;
  uint32_t max;
} state_sound_value_t;

boolean program_sound_value_init(msg_program_t *msg,
                                 program_tracker_t *tracker) {
  DEBUG3_PRINTLN("Initializing sound value state");

  state_sound_value_t *state = (state_sound_value_t *)malloc(sizeof (state_sound_value_t));
  state->value = 0;
  state->max = 0;
  
  tracker->state = state;

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
    // To avoid noise while quiet set a minimumza
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

