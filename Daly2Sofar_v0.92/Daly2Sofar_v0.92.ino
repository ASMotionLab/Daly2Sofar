/*
Daly2Sofar

An ESP32 based bridge that allows Daly Smart BMS to be used with a Sofar inverter/charger (and others that use SMA protocol).
It connects to Daly via UART and transmits to the inverter on CANBUS (SMA protocol).
At the same time, it will transmit the BMS data to an MQTT broker if it can connect to your WiFi.
If it can't connect to WiFi or your MQTT broker, it will still work as a BMS-to-Inverter bridge.

Data comms is one way only - data is requested from Daly and sent to the Inverter & over MQTT.
May be compatible with other inverters that use the SMA protocol.

My testing showed that Daly UART port works on 3.3v. PLEASE measure yours before connecting the ESP. Some users report 5V.


****** a point on isolation *
Some battery setups earth the battery negative, others don't.
For the ones that aren't earthed, you may get a ID05 Fault on the inverter when you connect canbus.
This is because the Daly UART ground is battery ground, and it makes its way to the inverter.
The Daly needs to be isolated in this case, using a digital isolation IC.
Opto-coupler's don't work as the signal becomes inverted. ESP32 serial can be inverted by software but Daly can't.
I used a IL716-3E isolator IC and soldered dupont pins to it's legs - it's tiny, it's not easy!
If you find an isolator IC module that is cheap and works without soldering, please let me know!
******


Using SMA_CAN_protocol.pdf for the CAN IDs and data format info (google will find you the PDF)

Using https://cryptii.com/pipes/integer-encoder for working out hex values
VIEW TEXT > DECODE INTEGER (LITTLE ENDIAN) U16 > VIEW BYTES HEX GROUPED BY BYTES

https://www.scadacore.com/tools/programming-calculators/online-hex-converter/ for checking hex values
icons made with https://www.pixilart.com/draw/icon-maker-82d9ebbc1fc1fec#
converted with https://diyusthad.com/image2cpp



****** a point on ESP32 pin remapping *
Make sure the I2C pins are mapped correctly for the Oled to work. For me, pins 21 and 22 work great
C:\Users\XXXXX\Documents\Arduino\libraries\Adafruit_SSD1306-1.1.2\Adafruit_SSD1306.cpp
#define I2C_SDA 21
#define I2C_SCL 22

And also Serial2 needs to be correctly mapped in this file (This is Windows. Sorry I don't know where it is on a Mac)
C:\Users\XXXXX\AppData\Local\Arduino15\packages\esp32\hardware\esp32\1.0.6\cores\esp32\HardwareSerial.cpp
Look for this near the top:
#ifndef RX2
#define RX2 16
#endif

#ifndef TX2
#define TX2 17
#endif
******


MQTT info:
Subscribe your MQTT client to:
Daly2Sofar/state

topics published are:
soc
voltage
current
power
temp
lowestcell
highestcell
cellimbalance

For extracting the data into Home Asisstant sensors, use Node-Red. Have a look at the github for an example Node-Red setup.

To do:
- Do proper CANBUS status check. Currently, CAN indicator will go live after the data send, whether it's succesfull or not. CAN only goes off if BMS not found.
- Need to figure out how to obtain battery cycles from BMS.
- Tidy up code/refactor where poss.
- Longer term stablity testing.

Notes/dependencies:
- Developed on Arduino IDE 1.8.5. IDE version 1.6.14 failed to compile. Downgrade if you have issues
- Using arduino ESP32 Core version 1.0.6
- https://github.com/maland16/daly-bms-uart
- https://github.com/miwagner/ESP32-Arduino-CAN
- https://github.com/me-no-dev/AsyncTCP
- https://github.com/marvinroger/async-mqtt-client

*/

// BATTERY CAPACITY (Doesn't really matter, as it's the BMS that works out the SOC)
uint16_t batteryCapacity = 190; // Battery capacity - in AH.
float batteryChargeVoltage = 55; // Set your charge voltage - this doesn't come from BMS.

// ENTER YOUR WIFI & MQTT BROKER DETAILS HERE

#define WIFI_SSID "yourwifiname"
#define WIFI_PASSWORD "yourwifipassword"
static const char mqttUser[] = "yourmqttlogin";
static const char mqttPassword[] = "yourmqttpassword";
const char* deviceName = "Daly2Sofar"; //Device name is used as the MQTT base topic.

#define MQTT_HOST IPAddress(192, 168, 0, 206) //replace with your MQTT server IP
#define MQTT_PORT 1883





#include <WiFi.h>
extern "C" {
  #include "freertos/FreeRTOS.h"
  #include "freertos/timers.h"
}
#include <AsyncMqttClient.h>

AsyncMqttClient mqttClient;


// ESP32 CAN Library: https://github.com/miwagner/ESP32-Arduino-CAN
#include <ESP32CAN.h>
#include <CAN_config.h>
CAN_device_t CAN_cfg;
CAN_frame_t tx_frame;
const int rx_queue_size = 10;       // Receive Queue size
///////////////////////////////////////////////////////////////////

// Daly UART library: https://github.com/maland16/daly-bms-uart
#include <daly-bms-uart.h>
Daly_BMS_UART bms(Serial2);
// Vars for BMS data
boolean BMSOnline = false;
boolean CANOnline = false;
float volts = 0;
float amps = 0;
float percentage = 0;
int8_t temp = 0;

float maxCellVoltage = 0;
float minCellVoltage = 0;
uint8_t maxCellNumber = 0;
uint8_t minCellNumber = 0;
int16_t watts = 0;
uint16_t cellImbalance = 0; // in millivolts
///////////////////////////////////////////////////////////////////

// OLED stuff
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define OLED_RESET 0  // GPIO0
Adafruit_SSD1306 display(OLED_RESET);
String oledLine1;
String oledLine2;
String oledLine3;
String oledLine4;
String oledLine5;
////////////////////////////////////////////////////////






TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;
TimerHandle_t mqttPublishXTimer;
TimerHandle_t dalyRetryXTimer;


bool WiFiStatus = false;
bool MQTTStatus = false;

uint8_t dalyRequestCounter = 0;

// USER SETUP VALUES:
unsigned long previousMillisUARTCAN = 0;   // will store last time a CAN Message was send
const int intervalUARTCAN = 1000;          // interval at which send CAN Messages (milliseconds)

unsigned long previousMillisWIFIMQTT = 0;
const int intervalWIFIMQTT = 2000; 


/*
// PYLON GENERAL LITHIUM EMULATION
// Constructing a test data packet. Little endian, 8 byte messages, 4 halfwords. CAN ID (11bit id 0x351 split into 2 bytes, little endian) for the CAN send function, to reduce manual labour.
byte CANData1[]=   {0x03, 0x51,  // CAN ID (11bit id 0x351 split into 2 bytes, little endian). Battery charge voltage, DC charge current limitation, DC discharge currentlimitation, discharge voltage
                    0x14, 0x02,   // Battery charge voltage - U16 - 0.1 scale. 41-63. Default 60 (560)
                    0x74, 0x0E,   // DC charge current limitation - S16 - 0.1 scale. 0-1200. Default 99.9 (999)
                    0x74, 0x0E,   // DC discharge current limitation - S16 - 0.1 scale. 0-1200. Test: 99.9 (999)
                    0xCC, 0x01};  // Voltage discharge limit - U16 - 0.1 scale. 41-48. Default 41.5 (415)

byte CANData2[]=   {0x03, 0x55,  // CAN ID (11bit id 0x355 split into 2 bytes, little endian).. SOC value, SOH value, HiResSOC
                    0x39, 0x00,    // State of Charge (SOC) - U16 - 1:1 scale. 0-100. (Should be 57%)
                    0x64, 0x00};   // State of Health (SOH) - U16 - 1:1 scale. 0-100. (Should be 99%)

byte CANData3[]=   {0x03,0x56,   // CAN ID (11bit id 0x356 split into 2 bytes, little endian). Battery Voltage, Battery Current, Battery Temperature
                    0x4E, 0x13,   // Battery Voltage - S16 - 0.01 scale. (Should be 52.13V - 5213)
                    0x02, 0x03,   // Battery Current - S16 - 0.01 scale. (Should be 7.31A - 731)
                    0x04, 0x05};   // Battery Temperature - S16 - 0.01 scale. (Should be 17.43c - 1743)

byte CANData4[]=   {0x03, 0x59,  // CAN ID (11bit id 0x359 split into 2 bytes, little endian). Alarms Warnings (See table in the PDF)
                    0x00, 0x00,     // See table
                    0x00, 0x00,     // See table
                    0x0A, 0x50,     // See table
                    0x4E};    // See table

byte CANData5[]=   {0x03, 0x5C,  // CAN ID (11bit id 0x35C split into 2 bytes, little endian). Events. Not sure if only 2 bytes or full 8
                    0xC0, 0x00};     // See table

byte CANData6[]=   {0x03, 0x5E,  // CAN ID (11bit id 0x35E split into 2 bytes, little endian). Manufacturer name - ASCII
                    0x50, 0x59,   // See table.
                    0x4C, 0x4F,    // See table.
                    0x4E, 0x20,     // See table.
                    0x20, 0x20};    // See table.
*/

// SMA GENERAL LITHIUM EMULATION
// Constructing a test data packet. Little endian, 8 byte messages, 4 halfwords. CAN ID (11bit id 0x351 split into 2 bytes, little endian) for the CAN send function, to reduce manual labour.
byte CANData351[]=   {0x03, 0x51,		// CAN ID (11bit id 0x351 split into 2 bytes, little endian). Battery charge voltage, DC charge current limitation, DC discharge currentlimitation, discharge voltage
                    0x27, 0x02,		// Battery charge voltage - U16 - 0.1 scale. 41-63. Default 55.2 (552)
                    0xE7, 0x03,		// DC charge current limitation - S16 - 0.1 scale. 0-1200. Default 99.9 (999)
                    0xBC, 0x02,		// DC discharge current limitation - S16 - 0.1 scale. 0-1200. Test: 70 (700)
                    0x9F, 0x01};	// Voltage discharge limit - U16 - 0.1 scale. 41-48. Default 41.5 (415)

byte CANData355[]=   {0x03, 0x55,		// CAN ID (11bit id 0x355 split into 2 bytes, little endian).. SOC value, SOH value, HiResSOC
                    0x39, 0x00,		// State of Charge (SOC) - U16 - 1:1 scale. 0-100. (Should be 57%)
                    0x63, 0x00,		// State of Health (SOH) - U16 - 1:1 scale. 0-100. (Should be 99%)
                    0xCC, 0x26,		// High resolution SOC value - U16 - 0.01 scale. 0-100 (Should be 99.32%)
                    0x00, 0x00};	// nothing

byte CANData356[]=   {0x03,0x56,		// CAN ID (11bit id 0x356 split into 2 bytes, little endian). Battery Voltage, Battery Current, Battery Temperature
                    0x5D, 0x14,		// Battery Voltage - S16 - 0.01 scale. (Should be 52.13V - 5213)
                    0x00, 0x00,		// Battery Current - S16 - 0.01 scale. (Should be 7.31A - 731)
                    0xCF, 0x06,		// Battery Temperature - S16 - 0.01 scale. (Should be 17.43c - 1743)
                    0x00, 0x00};	// nothing

byte CANData35A[]=   {0x03, 0x5A,		// CAN ID (11bit id 0x35A split into 2 bytes, little endian). Alarms Warnings (See table in the PDF)
                    0x00, 0x00,		// See table
                    0x00, 0x00,		// See table
                    0x00, 0x00,		// See table
                    0x00, 0x00};	// See table

byte CANData35B[]=   {0x03, 0x5B,		// CAN ID (11bit id 0x35B split into 2 bytes, little endian). Events. Not sure if only 2 bytes or full 8
                    0x00, 0x00,		// See table
                    0x00, 0x00,		// See table
                    0x00, 0x00,		// See table
                    0x00, 0x00};	// See table

byte CANData35E[]=   {0x03, 0x5E,		// CAN ID (11bit id 0x35E split into 2 bytes, little endian). Manufacturer name - ASCII
                    0x53, 0x4D,		// See table. S M
                    0x41, 0x00,		// See table. A
                    0x00, 0x00,		// See table
                    0x00, 0x00};	// See table

byte CANData35F[]=   {0x03, 0x5F,		// CAN ID (11bit id 0x35F split into 2 bytes, little endian). Bat-Type BMS Version Bat-Capacity reserved Manufacturer ID
                    0x00, 0x00,		// "Bat-Type". No more known
                    0x00, 0x00,		// "BMS Version". No more known
                    0xC8, 0x00,		// "Bat capacity". Need to figure out encoding. Currently shows as 51200
                    0x00, 0x00};	// "reserved Manufacturer ID". No more known.



// 'daly-on', 29x13px
const unsigned char dalyOn [] PROGMEM = {
  0xff, 0xff, 0xff, 0xf8, 0x80, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00, 0x08, 0x9e, 0x39, 0x04, 0x48, 
  0x91, 0x45, 0x02, 0x88, 0x91, 0x7d, 0x01, 0x08, 0x91, 0x45, 0x01, 0x08, 0x91, 0x45, 0x01, 0x08, 
  0x91, 0x45, 0x01, 0x08, 0x9e, 0x45, 0xf1, 0x08, 0x80, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00, 0x08, 
  0xff, 0xff, 0xff, 0xf8
};
// 'daly-off', 29x13px
const unsigned char dalyOff [] PROGMEM = {
  0xff, 0xff, 0xff, 0xf8, 0xb0, 0x00, 0x00, 0x08, 0x8c, 0x00, 0x00, 0x08, 0x9f, 0xb9, 0x04, 0x48, 
  0x91, 0x65, 0x02, 0x88, 0x91, 0x7d, 0x01, 0x08, 0x91, 0x47, 0x01, 0x08, 0x91, 0x45, 0xc1, 0x08, 
  0x91, 0x45, 0x31, 0x08, 0x9e, 0x45, 0xff, 0x08, 0x80, 0x00, 0x01, 0x88, 0x80, 0x00, 0x00, 0x68, 
  0xff, 0xff, 0xff, 0xf8
};
// 'can-on', 29x13px
const unsigned char canOn [] PROGMEM = {
  0xff, 0xff, 0xff, 0xf8, 0x80, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00, 0x08, 0x81, 0xc7, 0x22, 0x08, 
  0x82, 0x28, 0xb2, 0x08, 0x82, 0x0f, 0xaa, 0x08, 0x82, 0x08, 0xa6, 0x08, 0x82, 0x08, 0xa2, 0x08, 
  0x82, 0x28, 0xa2, 0x08, 0x81, 0xc8, 0xa2, 0x08, 0x80, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00, 0x08, 
  0xff, 0xff, 0xff, 0xf8
};
// 'can-off', 29x13px
const unsigned char canOff [] PROGMEM = {
  0xff, 0xff, 0xff, 0xf8, 0xb0, 0x00, 0x00, 0x08, 0x8c, 0x00, 0x00, 0x08, 0x83, 0xc7, 0x22, 0x08, 
  0x82, 0x68, 0xb2, 0x08, 0x82, 0x1f, 0xaa, 0x08, 0x82, 0x0f, 0xa6, 0x08, 0x82, 0x08, 0xe2, 0x08, 
  0x82, 0x28, 0xb2, 0x08, 0x81, 0xc8, 0xae, 0x08, 0x80, 0x00, 0x01, 0x88, 0x80, 0x00, 0x00, 0x68, 
  0xff, 0xff, 0xff, 0xf8
};
// 'wifi-on', 29x13px
const unsigned char wifiOn [] PROGMEM = {
  0xff, 0xff, 0xff, 0xf8, 0x80, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00, 0x08, 0x84, 0x49, 0xf2, 0x08, 
  0x84, 0x41, 0x00, 0x08, 0x84, 0x49, 0xc2, 0x08, 0x84, 0x49, 0x02, 0x08, 0x85, 0x49, 0x02, 0x08, 
  0x86, 0xc9, 0x02, 0x08, 0x84, 0x49, 0x02, 0x08, 0x80, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00, 0x08, 
  0xff, 0xff, 0xff, 0xf8
};
// 'wifi-off', 29x13px
const unsigned char wifiOff [] PROGMEM = {
  0xff, 0xff, 0xff, 0xf8, 0xb0, 0x00, 0x00, 0x08, 0x8c, 0x00, 0x00, 0x08, 0x87, 0xc9, 0xf2, 0x08, 
  0x84, 0x61, 0x00, 0x08, 0x84, 0x59, 0xc2, 0x08, 0x84, 0x4f, 0x02, 0x08, 0x85, 0x49, 0xc2, 0x08, 
  0x86, 0xc9, 0x32, 0x08, 0x84, 0x49, 0x0e, 0x08, 0x80, 0x00, 0x01, 0x88, 0x80, 0x00, 0x00, 0x68, 
  0xff, 0xff, 0xff, 0xf8
};
// 'mqtt-on', 29x13px
const unsigned char mqttOn [] PROGMEM = {
  0xff, 0xff, 0xff, 0xf8, 0x80, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00, 0x08, 0x91, 0x39, 0xf7, 0xc8, 
  0x9b, 0x44, 0x41, 0x08, 0x95, 0x44, 0x41, 0x08, 0x91, 0x44, 0x41, 0x08, 0x91, 0x44, 0x41, 0x08, 
  0x91, 0x48, 0x41, 0x08, 0x91, 0x34, 0x41, 0x08, 0x80, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00, 0x08, 
  0xff, 0xff, 0xff, 0xf8
};
// 'mqtt-off', 29x13px
const unsigned char mqttOff [] PROGMEM = {
  0xff, 0xff, 0xff, 0xf8, 0xb0, 0x00, 0x00, 0x08, 0x8c, 0x00, 0x00, 0x08, 0x93, 0xb9, 0xf7, 0xc8, 
  0x9b, 0x64, 0x41, 0x08, 0x95, 0x5c, 0x41, 0x08, 0x91, 0x47, 0x41, 0x08, 0x91, 0x44, 0xc1, 0x08, 
  0x91, 0x48, 0x71, 0x08, 0x91, 0x34, 0x4f, 0x08, 0x80, 0x00, 0x01, 0x88, 0x80, 0x00, 0x00, 0x68, 
  0xff, 0xff, 0xff, 0xf8
};
