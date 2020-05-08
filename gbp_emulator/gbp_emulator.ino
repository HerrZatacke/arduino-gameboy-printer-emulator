/*************************************************************************
 *
 * GAMEBOY PRINTER EMULATION PROJECT (Arduino)
 *
 * Creation Date: 2017-4-6
 * PURPOSE: To capture gameboy printer images without a gameboy printer
 * AUTHOR: Brian Khuu
 *
 */



/* Gameboy Link Cable Mapping to Arduino Pin */
// Note: Serial Clock Pin must be attached to an interrupt pin of the arduino
//  ___________
// |  6  4  2  |
//  \_5__3__1_/   (at cable)
//
//                  | Arduino Pin | Gameboy Link Pin  |
#define GBP_VCC_PIN               // Pin 1            : 5.0V (Unused)
#define GBP_SO_PIN       4        // Pin 2            : Serial OUTPUT
#define GBP_SI_PIN       3        // Pin 3            : Serial INPUT
#define GBP_SD_PIN                // Pin 4            : Serial Data  (Unused)
#define GBP_SC_PIN       2        // Pin 5            : Serial Clock (Interrupt)
#define GBP_GND_PIN               // Pin 6            : GND (Attach to GND Pin)

#define LED_STATUS_PIN 13 // Blink on packet reception

#define GBP_PACKET_PRETEND_PRINT_TIME_MS 2000 // ms to pretend to print for

// You can disable the LCD screen with no problems. It only displays my name
#define LCD_DISPLAY_MESSAGE_ENABLE 0

#define PRINT_LENGTH_AND_CRC 0

/*******************************************************************************
*******************************************************************************/

#include "gameboy_printer_protocol.h"

#if LCD_DISPLAY_MESSAGE_ENABLE
#include <LiquidCrystal.h>
#endif

#define NO_NEW_BIT -1
#define NO_NEW_BYTE -1
#define GBP_PACKET_TIMEOUT_MS 100 // ms timeout period to wait for next byte in a packet


/************************************************************************/

// include the library code:
#if LCD_DISPLAY_MESSAGE_ENABLE
LiquidCrystal lcd(9,8,7,6,5,4);
#endif

/* Packet Parsing State Machine State */
typedef enum gbp_parse_state_t
{ // Indicates the stage of the parsing processing (syncword is not parsed)
    GBP_PARSE_STATE_COMMAND                   = 0,
    GBP_PARSE_STATE_COMPRESSION               = 1,
    GBP_PARSE_STATE_DATA_LENGTH_LOW           = 2,
    GBP_PARSE_STATE_PACKET_DATA_LENGTH_HIGH   = 3,
    GBP_PARSE_STATE_VARIABLE_PAYLOAD          = 4,
    GBP_PARSE_STATE_CHECKSUM_LOW              = 5,
    GBP_PARSE_STATE_CHECKSUM_HIGH             = 6,
    GBP_PARSE_STATE_DEVICE_ID                 = 7,
    GBP_PARSE_STATE_PRINTER_STATUS            = 8,
    GBP_PARSE_STATE_PACKET_RECEIVED           = 9,
    GBP_PARSE_STATE_DIAGNOSTICS               = 10
} gbp_parse_state_t;


/* Gameboy Printer Packet Parsing*/
typedef struct gbp_packet_t
{ // This represents the structure of a gbp printer packet (excluding the sync word)
  // Received
  uint8_t   command;
  uint8_t   compression;
  uint16_t  data_length;
  uint8_t   *data_ptr;   // Variable length field determined by data_length
  uint16_t  checksum;

  // Send
  uint8_t   acknowledgement;
  uint8_t   printer_status;
} gbp_packet_t;


/******************************************************************************/
//  GAMEBOY PRINTER FUNCTIONS (Stream Byte Version)
/******************************************************************************/

/*
    Structs
*/

// Reads the bitstream and outputs a bytes stream after sync
typedef struct gbp_rx_tx_byte_buffer_t
{
  bool      initialized;

  // Bit State
  int       serial_clock_state_prev;  // Previous Serial Clock State

  // Sync word
  bool      syncronised; // Is true when byte is aligned
  uint16_t  sync_word;   // Sync word to match against
  uint16_t  sync_buffer; // Streaming sync buffer

  // Bit position within a byte frame
  uint8_t   byte_frame_bit_pos;

  // Used for receiving byte
  uint8_t   rx_byte_buffer;

  // Used for transmitting byte
  uint8_t   tx_byte_staging;
  uint8_t   tx_byte_buffer;
} gbp_rx_tx_byte_buffer_t;

// This deals with interpreting bytes stream as a packet structure
typedef struct gbp_packet_parser_t
{
  gbp_parse_state_t parse_state;
  uint16_t data_index;
  uint16_t calculated_checksum;

  // Debug Record
  uint8_t   crc_high;
  uint8_t   crc_low;
} gbp_packet_parser_t;

// Printer Status and other stuff
typedef struct gbp_printer_t
{ // This is the overall information about the printer
  bool      initialized;

  gbp_printer_status_t    gbp_printer_status;
  gbp_rx_tx_byte_buffer_t gbp_rx_tx_byte_buffer;
  gbp_packet_parser_t     gbp_packet_parser;
  gbp_packet_t            gbp_packet;

  // Triggered upon successful read of a packet
  bool packet_ready_flag;

  // Buffers
  uint8_t                 gbp_print_settings_buffer[4];
  uint8_t                 gbp_print_buffer[650];  // 640 bytes usually

  // Timeout if bytes not received in time
  unsigned long uptime_til_timeout_ms;
  unsigned long uptime_til_pretend_print_finish_ms;
} gbp_printer_t;


/*
    Global Vars
*/
gbp_printer_t gbp_printer; // Overall Structure

/*
    Static Functions
*/
void gbp_status_byte_print_as_json_fields(struct gbp_printer_status_t *printer_status_ptr)
{ // This is returns a gameboy printer status byte (Based on description in http://gbdev.gg8.se/wiki/articles/Gameboy_Printer )
  Serial.print("{");
  Serial.print("\"lowbatt\":");
  Serial.print(printer_status_ptr->low_battery, DEC);
  Serial.print(",\"jam\":");
  Serial.print(printer_status_ptr->paper_jam, DEC);
  Serial.print(",\"err\":");
  Serial.print(printer_status_ptr->other_error, DEC);
  Serial.print(",\"pkterr\":");
  Serial.print(printer_status_ptr->packet_error, DEC);
  Serial.print(",\"unproc\":");
  Serial.print(printer_status_ptr->unprocessed_data, DEC);
  Serial.print(",\"full\":");
  Serial.print(printer_status_ptr->print_buffer_full, DEC);
  Serial.print(",\"bsy\":");
  Serial.print(printer_status_ptr->printer_busy, DEC);
  Serial.print(",\"chk_err\":");
  Serial.print(printer_status_ptr->checksum_error, DEC);
  Serial.print("}");
}

/******************************************************************************/
/*------------------------- BYTE STREAMER ------------------------------------*/
/******************************************************************************/


static bool gbp_rx_tx_byte_reset(struct gbp_rx_tx_byte_buffer_t *ptr)
{ // Resets the byte reader, back into scanning for the next packet.
  *ptr = {0}; // Clear

  ptr->initialized  = true;
  ptr->syncronised  = false;
  ptr->sync_word    = GBP_SYNC_WORD;
}

static bool gbp_rx_tx_byte_set(struct gbp_rx_tx_byte_buffer_t *ptr, const uint8_t tx_byte)
{ // Stages the next byte to be transmitted
  ptr->tx_byte_staging = tx_byte;
}

static bool gbp_rx_tx_byte_update(struct gbp_rx_tx_byte_buffer_t *ptr, uint8_t *rx_byte,  int *rx_bitState)
{ // This is a byte scanner to allow this to read gameboy printer protocol formatted messages
  bool byte_ready = false;

  int serial_clock_state  = digitalRead(GBP_SC_PIN);
  int serial_out_state    = digitalRead(GBP_SO_PIN);

  *rx_bitState = NO_NEW_BIT;

  if (!(ptr->initialized))
  { // Record initial clock pin state
    gbp_rx_tx_byte_reset(ptr);
    ptr->initialized = true;
    ptr->serial_clock_state_prev = serial_clock_state;
    return byte_ready;
  }

  // Clock Edge Detection
  if (serial_clock_state != ptr->serial_clock_state_prev)
  { // Clock Pin Transition Detected

    if (serial_clock_state)
    { // Rising Clock (Bit Rx Read)

      // Current Bit State (Useful for diagnostics)
      *rx_bitState = serial_out_state;

      // Is this syncronised to a byte frame yet?
      if (!(ptr->syncronised))
      { // Preamble Sync Scan

        // The sync buffer is seen as a FIFO stream of bits
        (ptr->sync_buffer) <<= 1;

        // Push in a `1` else leave as `0`
        if(serial_out_state)
        {
          (ptr->sync_buffer) |= 1;
        }

        // Check if Sync Word is found
        if (ptr->sync_buffer == ptr->sync_word)
        { // Syncword detected
          ptr->syncronised = true;
          ptr->byte_frame_bit_pos = 7;
        }

      }
      else
      { // Byte Read Mode

        if(serial_out_state)
        { // Get latest incoming bit and insert to next bit position in a byte
          ptr->rx_byte_buffer |= (1 << ptr->byte_frame_bit_pos);
        }

        if (ptr->byte_frame_bit_pos > 0)
        { // Need to read a few more bits to make a byte
          ptr->byte_frame_bit_pos--;
        }
        else
        { // All bits in a byte frame has been received
          byte_ready = true;

          // Set Byte Result
          *rx_byte = ptr->rx_byte_buffer;

          // Reset Rx Buffer
          ptr->byte_frame_bit_pos = 7;
          ptr->rx_byte_buffer = 0;
        }
      }
    }
    else
    { // Falling Clock (Bit Tx Set)

      if ((ptr->syncronised) );
      { // Only start transmitting when syncronised

        // Loading new TX Bytes on new byte frames
        if(7 == ptr->byte_frame_bit_pos)
        { // Start of a new byte cycle
            if (ptr->tx_byte_staging)
            { // Byte ready to be sent
              ptr->tx_byte_buffer = ptr->tx_byte_staging;
            }
            else
            { // No new bytes present. Just keep transmitting zeros.
              ptr->tx_byte_buffer = 0;
            }
        }

        // Send next bit in a byte
        if(ptr->tx_byte_buffer & (1 << ptr->byte_frame_bit_pos))
        { // Send High Bit
          digitalWrite(GBP_SI_PIN, HIGH);
        }
        else
        { // Send Low Bit
          digitalWrite(GBP_SI_PIN, LOW);
        }

      }

    }

  }

  // Save current state for next edge detection
  ptr->serial_clock_state_prev = serial_clock_state;
  return byte_ready;
}


/******************************************************************************/
/*------------------------- MESSAGE PARSER -----------------------------------*/
/******************************************************************************/


static bool gbp_parse_message_reset(struct gbp_packet_parser_t *ptr)
{
  *ptr =
  {
    (gbp_parse_state_t) 0,
    0
  };
}


static bool gbp_parse_message_update
(
  struct gbp_packet_parser_t *ptr,             // Parser Variables
  bool *packet_ready_flag,                     // OUTPUT: Packet Ready Semaphore
  struct gbp_packet_t *packet_ptr,             // INPUT/OUTPUT: Packet Data Buffer
  struct gbp_printer_t *printer_ptr,           // INPUT/OUTPUT: Printer Variables
  const bool new_rx_byte,                      // INPUT: New Incoming Byte Flag
  const uint8_t rx_byte,                       // INPUT: New Incoming Byte Value
  bool *new_tx_byte,                           // OUTPUT: New Outgoing Byte Ready
  uint8_t *tx_byte                             // OUTPUT: New Outgoing Byte Value
)
{ // Return false if there was no error detected
  bool error_status = false;
  gbp_parse_state_t parse_state_prev = ptr->parse_state;

  *new_tx_byte = false;

  //-------------------------- NEW BYTES

  if (new_rx_byte)
  {
    // This keeps track of each stage and how to handle each incoming byte
    switch (ptr->parse_state)
    {
      case GBP_PARSE_STATE_COMMAND:
      {
        ptr->parse_state = GBP_PARSE_STATE_COMPRESSION;

        packet_ptr->command = rx_byte;

        switch (packet_ptr->command)
        {
          case GBP_COMMAND_INIT:
            packet_ptr->data_ptr = NULL;
            break;
          case GBP_COMMAND_DATA:
            packet_ptr->data_ptr = printer_ptr->gbp_print_buffer;
            break;
          case GBP_COMMAND_PRINT:
            packet_ptr->data_ptr = printer_ptr->gbp_print_settings_buffer;
            break;
          case GBP_COMMAND_INQUIRY:
            packet_ptr->data_ptr = NULL;
            break;
          default:
            packet_ptr->data_ptr = NULL;
        }

        // Checksum Tally
        ptr->calculated_checksum = 0; // Initialise Count
        ptr->calculated_checksum += rx_byte;
      } break;
      case GBP_PARSE_STATE_COMPRESSION:
      {
        ptr->parse_state = GBP_PARSE_STATE_DATA_LENGTH_LOW;
        packet_ptr->compression = rx_byte;

        // Checksum Tally
        ptr->calculated_checksum += rx_byte;
      } break;
      case GBP_PARSE_STATE_DATA_LENGTH_LOW:
      {
        ptr->parse_state = GBP_PARSE_STATE_PACKET_DATA_LENGTH_HIGH;
        packet_ptr->data_length |= ( (rx_byte << 0) & 0x00FF );

        // Checksum Tally
        ptr->calculated_checksum += rx_byte;
      } break;
      case GBP_PARSE_STATE_PACKET_DATA_LENGTH_HIGH:
      {
        packet_ptr->data_length |= ( (rx_byte << 8) & 0xFF00 );

        // Check data length
        if (packet_ptr->data_length > 0)
        {
          if (packet_ptr->data_ptr == NULL)
          { // SIMPLE ASSERT
            Serial.println("# ERROR: Serial data length should be non zero");
            while(1);
          }
          ptr->parse_state = GBP_PARSE_STATE_VARIABLE_PAYLOAD;
        }
        else
        { // Skip variable payload stage if data_length is zero
          ptr->parse_state = GBP_PARSE_STATE_CHECKSUM_LOW;
        }

        // Checksum Tally
        ptr->calculated_checksum += rx_byte;
      } break;
      case GBP_PARSE_STATE_VARIABLE_PAYLOAD:
      {
        /*
          The logical flow of this section is similar to
          `for (data_index = 0 ; (data_index > packet_ptr->data_length) ; data_index++ )`
        */
        // Record Byte
        packet_ptr->data_ptr[ptr->data_index] = rx_byte;

        // Checksum Tally
        ptr->calculated_checksum += rx_byte;

        // Increment to next byte position in the data field
        ptr->data_index++;

        // Escape and move to next stage
        if (ptr->data_index > packet_ptr->data_length)
        {
          ptr->parse_state = GBP_PARSE_STATE_CHECKSUM_LOW;
        }
      } break;
      case GBP_PARSE_STATE_CHECKSUM_LOW:
      {
        ptr->parse_state = GBP_PARSE_STATE_CHECKSUM_HIGH;
        packet_ptr->checksum = 0;
        packet_ptr->checksum |= ( (rx_byte << 0) & 0x00FF );
        ptr->crc_low = rx_byte; // For debugging
      } break;
      case GBP_PARSE_STATE_CHECKSUM_HIGH:
      {
        ptr->parse_state = GBP_PARSE_STATE_DEVICE_ID;
        packet_ptr->checksum |= ( (rx_byte << 8) & 0xFF00 );
        ptr->crc_high = rx_byte; // For debugging
      } break;
      case GBP_PARSE_STATE_DEVICE_ID:
      {
        ptr->parse_state = GBP_PARSE_STATE_PRINTER_STATUS;
      } break;
      case GBP_PARSE_STATE_PRINTER_STATUS:
      {
        ptr->parse_state = GBP_PARSE_STATE_PACKET_RECEIVED;
      } break;
      case GBP_PARSE_STATE_PACKET_RECEIVED:
      {
      } break;
      case GBP_PARSE_STATE_DIAGNOSTICS:
      {
      } break;
    }
  } // New Byte Detected

  //-------------------------- INIT FOR NEXT STAGE
  /*
    This section commonly deals with initialising the next parsing state.
    e.g. Initialising variables in addition to also staging the next response byte.
  */

  // Indicates if there was a change in state on last cycle
  if (ptr->parse_state != parse_state_prev)
  {
    // This keeps track of each stage and how to handle each incoming byte
    switch (ptr->parse_state)
    {
      case GBP_PARSE_STATE_COMMAND:
      {
      } break;
      case GBP_PARSE_STATE_COMPRESSION:
      {
      } break;
      case GBP_PARSE_STATE_DATA_LENGTH_LOW:
      {
        packet_ptr->data_length = 0;
      } break;
      case GBP_PARSE_STATE_PACKET_DATA_LENGTH_HIGH:
      {
      } break;
      case GBP_PARSE_STATE_VARIABLE_PAYLOAD:
      {
        ptr->data_index = 0;
      } break;
      case GBP_PARSE_STATE_CHECKSUM_LOW:
      {
      } break;
      case GBP_PARSE_STATE_CHECKSUM_HIGH:
      {
      } break;
      case GBP_PARSE_STATE_DEVICE_ID:
      {
        *new_tx_byte = true;
        *tx_byte = GBP_DEVICE_ID;

        packet_ptr->acknowledgement = *tx_byte;
      } break;
      case GBP_PARSE_STATE_PRINTER_STATUS:
      {

        // Checksum Verification
        if (ptr->calculated_checksum == packet_ptr->checksum)
        { // Checksum Passed
          printer_ptr->gbp_printer_status.checksum_error = false;
        }
        else
        { // Checksum Failed
          printer_ptr->gbp_printer_status.checksum_error = true;
        }

        switch (packet_ptr->command)
        {
          case GBP_COMMAND_INIT:
            break;
          case GBP_COMMAND_DATA:
            printer_ptr->gbp_printer_status.unprocessed_data      = true;
            break;
          case GBP_COMMAND_PRINT:
            printer_ptr->gbp_printer_status.unprocessed_data      = false;
            printer_ptr->gbp_printer_status.print_buffer_full      = true;
            printer_ptr->gbp_printer_status.printer_busy  = true;

            // pretend to print for 5 seconds or so
            gbp_printer.uptime_til_pretend_print_finish_ms = millis() + GBP_PACKET_PRETEND_PRINT_TIME_MS;
            break;
          case GBP_COMMAND_INQUIRY:
            break;
        }

        *new_tx_byte = true;
        *tx_byte = gbp_status_byte(&(printer_ptr->gbp_printer_status));
        packet_ptr->printer_status = *tx_byte;
      } break;
      case GBP_PARSE_STATE_PACKET_RECEIVED:
      {
        *packet_ready_flag = true;
      } break;
      case GBP_PARSE_STATE_DIAGNOSTICS:
      {
      } break;

    }
  } // Init Next State

  return error_status;
}

/*------------------------- Gameboy Printer --------------------------*/

static bool gbp_printer_init(struct gbp_printer_t *ptr)
{
  ptr->initialized = true;
  ptr->gbp_printer_status = {0};

  gbp_rx_tx_byte_reset(&(ptr->gbp_rx_tx_byte_buffer));
  gbp_parse_message_reset(&(ptr->gbp_packet_parser));
}



/**************************************************************
 **************************************************************/
void
#ifdef ESP8266
ICACHE_RAM_ATTR
#endif
serialClock_ISR(void)
{
  int rx_bitState;

  uint8_t rx_byte;
  bool new_rx_byte;

  uint8_t tx_byte;
  bool new_tx_byte;

  /***************** BYTE PARSER ***********************/

  new_rx_byte = gbp_rx_tx_byte_update(&(gbp_printer.gbp_rx_tx_byte_buffer), &rx_byte,  &rx_bitState);

  if (new_rx_byte)
  {
    // Update Timeout State
    if (gbp_printer.gbp_rx_tx_byte_buffer.syncronised)
    { // Push forward timeout since a byte was received.
      gbp_printer.uptime_til_timeout_ms = millis() + GBP_PACKET_TIMEOUT_MS;
    }
  }

  /***************** TX/RX BIT->BYTE DIAGNOSTICS ***********************/

#if 0 // Bit Scanning
  if ( NO_NEW_BIT != rx_bitState )
  { // New bit detected
    Serial.print(rx_bitState);
  }
#endif

#if 0 // Byte Scanning
  if (new_rx_byte)
  {
    // Diagnostics
    Serial.print(gbp_printer.gbp_packet_parser.parse_state,HEX);
    Serial.print(":");
    Serial.println(rx_byte,HEX);
  }
#endif

  /***************** PACKET PARSER ***********************/

  gbp_parse_message_update(
                            &(gbp_printer.gbp_packet_parser),
                            &gbp_printer.packet_ready_flag,
                            &(gbp_printer.gbp_packet),
                            &(gbp_printer),
                            new_rx_byte, rx_byte,
                            &new_tx_byte, &tx_byte
                          );

  /***************** TX BYTE SET ***********************/

  // Byte to be tranmitted to the gameboy received
  if (new_tx_byte)
  {
    gbp_rx_tx_byte_set(&(gbp_printer.gbp_rx_tx_byte_buffer), tx_byte);
  }

  /***************** TX BYTE SET DIAGNOSTICS ***********************/

#if 0 // warning: effects timing
  if (new_tx_byte)
  {
    Serial.print("T");
    Serial.println(new_tx_byte,HEX);
  }
#endif

}

void setup()
{

#if LCD_DISPLAY_MESSAGE_ENABLE
  // set up the LCD's number of columns and rows:
  lcd.begin(16, 2);
  // Print a message to the LCD.
  lcd.print("GameBody Link");
  lcd.setCursor(0, 1);
  lcd.print("By Brian Khuu");
#endif

  // Config Serial
  // Has to be fast or it will not trasfer the image fast enough to the computer
  Serial.begin(115200);

  /* Pins from gameboy link cable */
  pinMode(GBP_SC_PIN, INPUT);
  pinMode(GBP_SO_PIN, INPUT);
  pinMode(GBP_SI_PIN, OUTPUT);

  /* Default link serial out pin state */
  digitalWrite(GBP_SI_PIN, LOW);

  /* LED Indicator */
  pinMode(LED_STATUS_PIN, OUTPUT);
  digitalWrite(LED_STATUS_PIN, LOW);

  /* Welcome Message */
  Serial.print("# GAMEBOY PRINTER EMULATION PROJECT\n");
  Serial.print("# By Brian Khuu (2017)\n");

  /* Clear Byte Scanner and Parser */
  gbp_printer_init(&gbp_printer);

  /* attach ISR */
  attachInterrupt( digitalPinToInterrupt(GBP_SC_PIN), serialClock_ISR, CHANGE);  // attach interrupt handler

} // setup()

void loop() {

  // Packet received from gameboy
  if (gbp_printer.packet_ready_flag)
  {
    digitalWrite(LED_STATUS_PIN, HIGH);

    Serial.print("!");
    Serial.print("{");
    Serial.print("\"command\":");
    switch (gbp_printer.gbp_packet.command)
    {
      case GBP_COMMAND_INIT:
        Serial.print("\"INIT\"");
        break;
      case GBP_COMMAND_DATA:
        Serial.print("\"DATA\"");
        Serial.print(",\"compressed\":");
        Serial.print(gbp_printer.gbp_packet.compression, DEC);
        Serial.print(",\"more\":");
        Serial.print(gbp_printer.gbp_packet.data_length != 0, DEC);           
        break;
      case GBP_COMMAND_PRINT:
        Serial.print("\"PRNT\"");
        Serial.print(",\"sheets\":");
        Serial.print(gbp_printer.gbp_print_settings_buffer[GBP_PRINT_BYTE_INDEX_NUM_OF_SHEETS], DEC);
        Serial.print(",\"margin_upper\":");
        Serial.print((gbp_printer.gbp_print_settings_buffer[GBP_PRINT_BYTE_INDEX_NUM_OF_LINEFEED]>>4) & 0x0F, DEC);
        Serial.print(",\"margin_lower\":");
        Serial.print(gbp_printer.gbp_print_settings_buffer[GBP_PRINT_BYTE_INDEX_NUM_OF_LINEFEED] & 0x0F, DEC);
        Serial.print(",\"pallet\":");
        Serial.print(gbp_printer.gbp_print_settings_buffer[GBP_PRINT_BYTE_INDEX_PALETTE_VALUE], DEC);
        Serial.print(",\"density\":");
        Serial.print(gbp_printer.gbp_print_settings_buffer[GBP_PRINT_BYTE_INDEX_PRINT_DENSITY], DEC);
        break;
        break;
      case GBP_COMMAND_INQUIRY:
        Serial.print("\"INQY\"");
        Serial.print(",\"status\":");
        gbp_status_byte_print_as_json_fields(&(gbp_printer.gbp_printer_status));
        break;
      default:
        Serial.print("\"UKNO\"");
    }
    Serial.print("}");

#if PRINT_LENGTH_AND_CRC
    Serial.print(": length: ");
    Serial.print(gbp_printer.gbp_packet.data_length, DEC);
    Serial.print(" | CRC: ");
    Serial.print(gbp_printer.gbp_packet.checksum, DEC);
    Serial.print(" | CRC CALC: ");
    Serial.print(gbp_printer.gbp_packet_parser.calculated_checksum, DEC);
    Serial.print(" (");
    Serial.print(((gbp_printer.gbp_packet_parser.calculated_checksum & 0xFF00) >> 8), DEC);
    Serial.print(" ");
    Serial.print(gbp_printer.gbp_packet_parser.calculated_checksum & 0x00FF, DEC);
    Serial.print(") | crc raw: ");
    Serial.print(gbp_printer.gbp_packet_parser.crc_high, DEC);
    Serial.print(" ");
    Serial.print(gbp_printer.gbp_packet_parser.crc_low, DEC);
    Serial.print(" |");
#endif
    Serial.print("\n");

    // Indicate To The Byte Scanner and the Message Parser to scan for new packet
    gbp_rx_tx_byte_reset(&(gbp_printer.gbp_rx_tx_byte_buffer));
    gbp_parse_message_reset(&(gbp_printer.gbp_packet_parser));

    // Process this packet
    switch (gbp_printer.gbp_packet.command)
    {
      case GBP_COMMAND_INIT:
      { // This clears the printer status register (and buffers etc... in the real printer)
        gbp_printer.gbp_printer_status = {0};
        //Serial.println("PRINT START");
        break;
      }
      case GBP_COMMAND_DATA:
      { // This is called when new data is recieved.
        /*
          Design Note.
          Previously had an idea to do some processing to turn this into NetPBM formatted printout.
          However this microcontroller has such a wimpy ram... and the printer has no flow control.
          So we really really need to offload the data as soon as possible.
        */
        for (uint16_t i = 0 ; i < gbp_printer.gbp_packet.data_length ; i++)
        { // Display the data payload encoded in hex
          uint8_t data_8bit = gbp_printer.gbp_packet.data_ptr[i];

          if (data_8bit < 16)
          { // Cheap way to add padding without printf (for speed gains)
            Serial.print("0");
          }

          Serial.print(data_8bit, HEX);

          // Insert Newline Periodically
          if ((i+1)%16 == 0)
          {
            Serial.print("\n");
          }
          else
          {
            Serial.print(" ");
          }
        }
        break;
      }
      case GBP_COMMAND_PRINT:
      { // This would usually indicate to the GBP to start printing.
        break;
      }
      case GBP_COMMAND_INQUIRY:
      { // This is usally called multiple times to check if ready to print or has finished printing.

        // I like the print screen in the gameboy, so I would just let this play for a few seconds.
        if ( (0 != gbp_printer.uptime_til_pretend_print_finish_ms) && (millis() > gbp_printer.uptime_til_pretend_print_finish_ms) )
        { // reset printer byte and packet processor
          Serial.println("# Finished Pretending To Print for fun!");
          gbp_printer.uptime_til_pretend_print_finish_ms = 0;
          gbp_printer.gbp_printer_status.printer_busy = false;
        }

        break;
      }
    }

    digitalWrite(LED_STATUS_PIN, LOW);
    gbp_printer.packet_ready_flag = false; // Packet Processed
  }


  // Trigger Timeout and reset the printer if byte stopped being recieved.
  if ( (gbp_printer.gbp_rx_tx_byte_buffer.syncronised) )
  {
    if ( (0 != gbp_printer.uptime_til_timeout_ms) && (millis() > gbp_printer.uptime_til_timeout_ms) )
    { // reset printer byte and packet processor
      Serial.println("# ERROR: Timed Out");
      gbp_rx_tx_byte_reset(&(gbp_printer.gbp_rx_tx_byte_buffer));
      gbp_parse_message_reset(&(gbp_printer.gbp_packet_parser));
    }
  }
  else
  { // During scanning phase timeout is not required.
    gbp_printer.uptime_til_timeout_ms = 0;
  }

  // Diagnostics Console
  while (Serial.available() > 0)
  {
    switch (Serial.read())
    {
      case '?':
        Serial.print("parse_state:");
        Serial.println(gbp_printer.gbp_packet_parser.parse_state,HEX);
        Serial.print("data_index:");
        Serial.println(gbp_printer.gbp_packet_parser.data_index,HEX);
        Serial.print("next timeout in:");
        Serial.println(gbp_printer.uptime_til_timeout_ms);
        Serial.print("milis():");
        Serial.println(millis());
        Serial.print("synced:");
        Serial.println(gbp_printer.gbp_rx_tx_byte_buffer.syncronised);
        break;

      case '!':
        gbp_status_byte_print_as_json_fields(&(gbp_printer.gbp_printer_status));
        Serial.print("\n");
        break;
    }
  };


} // loop()
