#include <mcp_can.h>
#include <ESP8266WiFi.h>
#include <InfluxDbClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include "Adafruit_SSD1306.h"

extern "C" {
  #include <user_interface.h>
}

// WiFi: ---------------------------------------------------------------------------
const char* ssid = "";
const char* password = "";

//Influx DB: -----------------------------------------------------------------------
#define INFLUXDB_URL ""
#define INFLUXDB_DB_NAME ""
#define INFLUXDB_USER ""
#define INFLUXDB_PASSWORD ""
InfluxDBClient client(INFLUXDB_URL, INFLUXDB_DB_NAME);

// Variables used to store the results: --------------------------------------------

// The following variables may have strange units because converting them to more suitable
// units (maybe volts or amperes would result in working with floating point numbers, which
// is undesirable in general.

// Array containing the voltage of each battery cell in millivolts mV.
int cellVoltages[96];
// Battery voltage in centivolts cV.
long batVoltage;
// Battery instensity in miliamps mA.
long batIntensity;
// Battery packs temperatures in °C
byte batTemperatures[3];

// CAN Variables: ------------------------------------------------------------------

// All the information requests on the Nissan Leaf need to be sent with this identifier.
#define requestID 0x079B

// These are the messages that need to be sent to the Nissan Leaf in order to get different
// data:

// This requests the temperatures of the battery packs 
byte requestBattTempBytes[] = {0x02, 0x21, 0x04, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// This request some general parameters of the battery, such as state of charge, voltage or
// current.
byte requestBattInfoBytes[] = {0x02, 0x21, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// This requests the voltage of each battery cell.
byte requestVoltageCellsBytes[] = {0x02, 0x21, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// This request additional data. Because the Nissan Leaf only sends 8 bytes long CAN
// dataframes, it splits the battery inoformation across multiple dataframes. Once the first
// of these dataframes has been delivered, these bytes can be used to request the following
// ones.
byte addionalDataBytes[] = {0x30, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff};
// The following bytes can be used to ask for all the following data, but are not used in this 
// script.
byte allAdditionalDataBytes[] = {0x30, 0x00, 0x18, 0xff, 0xff, 0xff, 0xff, 0xff};

// The following variables are used when sending and receiving CAN dataframes:

// Used to store the sending ID of the incoming CAN dataframes.
unsigned long IDIncoming;
// Used to store the length of the CAN dataframes. The ones used by the Nissan Leaf are always
// 8 bytes long.
byte dlc;
// Used to store the 8 bytes that form the CAN dataframes returned by the Nissan Leaf.
byte canBytes[8];
// An array used to print information on the serial monitor.
char msgString[128];

// PINs: ---------------------------------------------------------------------------

// The INT is on pin 0, which corresponds to D3.
const int CAN0_INT = 0;
// Set CS to pin 15, which corresponds to D8.
MCP_CAN CAN0(15);

// Other variables: ----------------------------------------------------------------

// Number of times a CAN message requesting information has been sent
unsigned int numTest = 0;
// Number of errors that have occurred in the data retrieving process. These errors can
// be CAN messages not being send properly, not receiving an answer in time etc.
// A small number of errors represents some points missing in the final dataset and
// should not be a big concern.
unsigned int numErr = 0;
// Delay (in miliseconds) that the programs wait when it encounters an error before
// continuing.
unsigned int errDelay = 2000;
// Debugging. If set to true the functions that read the CAN data print the raw hex
// values on the serial monitor
bool printHex = false;
// Variables used to measure the time the test has been runing for.
unsigned int timeMinutes;
unsigned int timeHours;

// Display configuration: ----------------------------------------------------------

const int oledReset = -1;
Adafruit_SSD1306 display(oledReset);

void setup(){

  Serial.begin(9600);
  while (!Serial);

  // Display setup: ----------------------------------------------------------------

  // Initialize the display and set the text properties
  Serial.println("Initializing display");
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.display();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.clearDisplay();
  display.setCursor(0,0);

  // WiFi connection setup: --------------------------------------------------------

  Serial.print("MAC: ");
  Serial.print(WiFi.macAddress());
  
  // Printing the MAC adress on the screen
  display.print("Connecting");
  display.setCursor(0,10);  
  display.print("MAC:");
  display.setCursor(0,20);
  for (byte i =0; i<=8; i++){
    display.print(WiFi.macAddress()[i]);
  }
  display.setCursor(0,30);
  for (byte i =9; i<=16; i++){
    display.print(WiFi.macAddress()[i]);
  }
  display.display();
  
  Serial.print(WiFi.macAddress());
  Serial.print('\n');
  Serial.println("CONNECTING!!!!!!");
  WiFi.persistent(false);
  // Connect to WiFi
  WiFi.begin(ssid, password);
  //Removes unwanted wireless network
  WiFi.mode(WIFI_STA); 
  display.setCursor(0,38);
  int i = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    // Loading bar
    display.print("=");
    display.display();
    Serial.print(++i); Serial.print(' ');     
    if (i > 10) {
      WiFi.disconnect();
      Serial.println("WiFi error! Resetting chip!");
      display.clearDisplay();
      display.setCursor(0,0);
      display.print("Unable to connect:  restartingchip");
      display.display();
      delay(1500);
      ESP.restart();
      break;
    }
  }
  if(WiFi.status()== WL_CONNECTED){
    display.clearDisplay();
    display.setCursor(0,0);
    display.print("WiFi....ok");
    display.display();  
    Serial.println("OK!");  
    // Access Point (SSID).
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());  
    // IP address
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    // Signal strength
    long rssi = WiFi.RSSI();
    Serial.print("Signal strength (RSSI): ");
    Serial.print(rssi);
    Serial.println(" dBm");
    Serial.println("*****************************");  
  }else {
      WiFi.disconnect();
      Serial.println("WiFi error! Resetting chip!");
      display.clearDisplay();
      display.setCursor(0,0);
      display.print("Unable to connect:  restartingchip");
      display.display();
      delay(1500);
      ESP.restart();
  }

  // Influx connection setup: ------------------------------------------------------
  
  // Configure Influx client authentication
  client.setConnectionParamsV1(INFLUXDB_URL, INFLUXDB_DB_NAME, INFLUXDB_USER, INFLUXDB_PASSWORD);
  // Configure the Influx library so it doesn't close the http connection:
  client.setHTTPOptions(HTTPOptions().connectionReuse(true));
  // Check db connection
  if (client.validateConnection()) {
    Serial.print("Connected to InfluxDB: ");
    Serial.println(client.getServerUrl());
    display.print("influx..ok");
    display.display();
  } else {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(client.getLastErrorMessage());
    display.print("influx..Er");
    display.display();
  }
  Serial.println("*****************************");

  // MCP2515 connection setup: -----------------------------------------------------

  // Initialize MCP2515 running at 16MHz with a baudrate of 500kb/s and the masks and
  // filters disabled.
  if (CAN0.begin(MCP_STDEXT, CAN_500KBPS, MCP_8MHZ) == CAN_OK){
    Serial.println("MCP2515 Initialized Successfully!");
    display.print("mcp.....ok");
    display.display();
  }  
  else {
    Serial.println("Error Initializing MCP2515... Permanent failure!  Check your code &
    Con ections");
    display.print("mcp.....Er");
    display.print("Restartingchip");
    display.display();
    delay(1500);
    ESP.restart();
  }

  // Set operation mode to normal so the MCP2515 sends acks to received data.
  CAN0.setMode(MCP_NORMAL);                      

  // Debugging:
  // If no messages messages are being received, uncomment the setMode line below
  // to test the wiring between the Ardunio and the protocol controller.
  // The message that this sketch sends should be instantly received.
  //CAN0.setMode(MCP_LOOPBACK);
  
  // Configuring pin for /INT input.
  pinMode(CAN0_INT, INPUT);
  delay(1500);
  
}

void loop(){
  timeMinutes = millis()/60000 % 60;
  timeHours = millis()/3600000 % 24;
  // The data requests are only sent if the microcontroller is properly connected
  // to the WiFi
  if(WiFi.status()== WL_CONNECTED){
    // After checking the WiFi connection, the connection to the databse is
    // checked.
    if (!client.validateConnection()){
      display.clearDisplay();
      display.setCursor(0,0);
      display.print("Influx    error     restartingchip");
      display.display();
      delay(1500);
      ESP.restart();
    }
    numTest++;
    // The all the functions defined to retrieve data from the CAN bus return a boolean
    // indicating if all the were successfully obtained. If this is the case the function
    // returns true and the information is sent to the database.
    if (readCellVoltages()){
      displayMsg("receiving");
      for (byte i=0; i<=95; i++){
        // The names and tags given to the values are chosen to follow the existing convention
        // for storing data in this database.
        
        // Unit of measurement. The values sent to the database are expressed in volts.
        Point measurement1("V");
        // Tags identifying the data.
        measurement1.addTag("entity_id", "nissan_canbus_cells");
        measurement1.addTag("cellPair", String(i));
        measurement1.addField("value", cellVoltages[i]/1000.);
        client.writePoint(measurement1);
        // The following function is used to display the current status of the tests
        // on the lcd display. An argument is used to indicate what type of information
        // should be displayed.
        displayMsg("sending");
        // Print the data being sent on the serial monitor.
        Serial.print(client.getLastErrorMessage());
        Serial.print(" writing cell voltage: ");
        Serial.println(client.pointToLineProtocol(measurement1));             
      }
    }
    // In case some error occurred during the acquisition of the data, the corresponding
    // function returns false, an error message is displayed and nothing is sent to
    // the database.
    else{
      numErr++;
      displayMsg("receivingError");
      delay(errDelay);
    }
    numTest++;
    // The battery temperature is requested in a similar manner to the cell pair voltages
    if (readBattTemps()){
      displayMsg("receiving");
      for (byte i=0; i<=2; i++){
        Point measurement1("C");
        measurement1.addTag("entity_id", "nissan_canbus_all_battery");
        measurement1.addTag("battery_pack", String(i));
        measurement1.addField("value", float(batTemperatures[i]));
        client.writePoint(measurement1);
        displayMsg("sending");
        Serial.print(client.getLastErrorMessage());
        Serial.print(" writing temperature: ");
        Serial.println(client.pointToLineProtocol(measurement1));             
      }
    }
    else{
      numErr++;
      displayMsg("receivingError");
      delay(errDelay);
    }
    numTest++;
    // Finally the battery voltage and current are obtained and stored.
    if (readBattInfo()){
      displayMsg("receiving");
      Point measurement1("V");
      measurement1.addTag("entity_id", "nissan_canbus_all_battery");
      measurement1.addField("value", batVoltage/100.);
      client.writePoint(measurement1);
      displayMsg("sending");
      Serial.print(client.getLastErrorMessage());
      Serial.print(" writing battery voltage to DB: ");
      Serial.println(client.pointToLineProtocol(measurement1));

      Point measurement2("A");
      measurement2.addTag("entity_id", "nissan_canbus_all_battery");
      measurement2.addField("value", batIntensity/1000.);
      client.writePoint(measurement2);
      // Print what are we exactly writing
      Serial.print(client.getLastErrorMessage());
      Serial.print(" writing battery intensity to DB: ");
      Serial.println(client.pointToLineProtocol(measurement2));
    }
    else{
      numErr++;
      displayMsg("receivingError");
      delay(errDelay);
    }
    delay(500);
  }
  else {
    displayMsg("wifiError");
    Serial.println("Lost WiFi connection");
    reconnect();
  }
  
}

// Some small functions that used throughout the code are defined
// next:

/**
 * Most values are returned as 2 bytes that need to be concatenated.
 * This function is implemented to do that. Note that it only returns
 * unsigned values.
 */
unsigned long concatBytesU(byte leftByte, byte rightByte){
  return (leftByte<<8)+rightByte;
}

/**
 * The intensity is the only variable that can be positive (charging) or
 * negative (discharging) because of that, it is returned as 4 bytes
 * that need to be converted into a signed decimal value. This function
 * does that by first checking if the decimal number is positive or
 * negative (fist bit) and applying the Two's complement method 
 * accordingly.
 */
long convertIntensity(byte byte1, byte byte2, byte byte3, byte byte4){
  long decimalIntensity = (byte2<<16) + (byte3<<8) + byte4;
    
  // Negative number:
  if(bitRead(byte1,7)){
    bitClear(byte1, 7);
    decimalIntensity = decimalIntensity + byte1 * 16777216 -2147483648;
  }
  // Positive number
  else {    
    decimalIntensity += byte1 * 16777216;
  }
  return decimalIntensity;  
}

/**
 * This function is used when the WiFi connection fails to reestablish it.
 */
void reconnect() {  
    Serial.print("Reconnecting");
    WiFi.begin(ssid, password);
    //Removes unwanted wireless network  
    WiFi.mode(WIFI_STA);
    int i = 0;
    while (WiFi.status() != WL_CONNECTED) {  
        digitalWrite(BUILTIN_LED, HIGH);
        delay(500);  
        Serial.print(".");
        digitalWrite(BUILTIN_LED, LOW);
        if (i > 20) {
         ESP.restart();
         break;
        }        
    }  
    Serial.println("WiFi reConnected!");
}

/**
 * This function is used to display a variety of messages during
 * the operation of the test. It is always showing the the total number
 * of CAN requests, the number of faulty CAN requests and the time the
 * test has been running for.
 * 
 * Additionally the display can show the current status
 * of the test and different kinds of error messages.
 */
void displayMsg(String msgMode){
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("CAN...");
  if (numTest<1000) {display.print("0");}
  if (numTest<100) {display.print("0");}
  if (numTest<10) {display.print("0");}
  display.print(numTest);  
  display.print("ERR...");
  if (numErr<1000) {display.print("0");}
  if (numErr<100) {display.print("0");}
  if (numErr<10) {display.print("0");}
  display.print(numErr);  
  display.print("t....");
  if (timeHours<10) {display.print("0");}
  display.print(timeHours);
  display.print(":");
  if (timeMinutes<10) {display.print("0");}
  display.print(timeMinutes);
  display.print("----------");
  
  if (msgMode=="receiving"){
    display.print("Receiving data");
  } 
  if (msgMode=="sending"){
    display.print("Sending toINFLUX");    
  }
  if (msgMode=="wifiError"){
    display.print("Connectingto WiFI");    
  }
  if (msgMode=="receivingError"){
    display.print("Data not  received");    
  }
  display.display();
}

/**
 * This function requests additional data to the CAN BUS. Because the Nissan Leaf only
 * sends 8 bytes long CAN dataframes, it is necessary to split the battery information
 * across multiple dataframes. Once the first of these dataframes has been delivered,
 * this function can be used to request the following ones.
 * 
 * The function returns true if it executed properly and false if it encounters any errors.
 */
bool readAdditionalData(){
  if (CAN0.sendMsgBuf(requestID, 8, addionalDataBytes) != CAN_OK) {
    Serial.println("An error ocurred during the request for additional data");
    return false;
  }
  
  delay(500);

  // Variable that indicates if a response to the request has been received.
  bool msgReceived = false;
  // Maximum number of times the wile loop is runned. If no value is receive before
  // this maximum is reached, the function is aborted and returns false.
  byte maxIterations = 25;
  byte numIterations = 0;
  
  while (!msgReceived){
    
    if (numIterations==maxIterations){
      Serial.println("No response to the initial request received before time expired");
      return false;
    }
    msgReceived = !digitalRead(CAN0_INT);
    
    // If CAN0_INT pin is low, read receive buffer.
    if (msgReceived){
      CAN0.readMsgBuf(&IDIncoming, &dlc, canBytes);
      
      // Determines if message is a remote request frame.
      if ((IDIncoming & 0x40000000) == 0x40000000) {
        Serial.println("The request for additional data returned a remote request frame");
        return false;
      } 
      // Checks if the ID of the response is 0x7BB
      else if (IDIncoming != 1979ul) {
        sprintf(msgString, "Response received from an unexpected ID: %.3lX", IDIncoming);
        Serial.println(msgString);
        return false;
      }    
      else {        
        return true;
     }
    }
    else{
      numIterations++;
      delay(300);      
    }

  }
}

/**
 * This function is defined to read Nissan Leaf battery voltage and current. It
 * checks that the received dataframes correspond to the desired information and the
 * results are stored on the variables batIntensity and batVoltage.
 * 
 * The function returns true if it executed properly and false if it encounters any errors.
*/
bool readBattInfo(){
  if (CAN0.sendMsgBuf(requestID, 8, requestBattInfoBytes) != CAN_OK) {
    Serial.println("Battery information error: An error ocurred during the initial request");
    return false;
  }
  else{
    Serial.println("Battery information: Initial request sent");
  }
  
  delay(100);

  // Variable that indicates if a response to the request has been received.
  bool msgReceived = false;
  // Maximum number of times the wile loop is run. If no value is received before this maximum
  // is reached, the function is aborted and returns false.
  byte maxIterations = 25;
  byte numIterations = 0;
  
  while (!msgReceived){
    
    if (numIterations==maxIterations){
      Serial.println("Battery information error: No response to the initial request received
      before time expired");
      return false;
    }
    msgReceived = !digitalRead(CAN0_INT);
    
    // If CAN0_INT pin is low, read receive buffer    
    if (msgReceived){
      CAN0.readMsgBuf(&IDIncoming, &dlc, canBytes);

      // Determines if message is a remote request frame.
      if ((IDIncoming & 0x40000000) == 0x40000000) {
        Serial.println("Battery information error: The initial request returned a remote
        request frame");
        return false;
      }
      // Checks if the ID of the response is 0x7BB.
      else if (IDIncoming != 1979ul) {
        sprintf(msgString, "Battery information error: Response received from an unexpected
        ID: %.3lX", IDIncoming);
        Serial.println(msgString);
        return false;
      }
      // Checks the header information.
      else if (canBytes[0]!=0x10 || canBytes[1]!=0x35 || canBytes[2]!=0x61||
      canBytes[3]!=0x01) {
        Serial.print("Battery information error: The initial frame header is different from
        expected: ");
        for (byte i = 0; i <= 3; i++) {
          sprintf(msgString, " 0x%.2X", canBytes[i]);
          Serial.print(msgString);
        }
        Serial.println();
        return false;
      }
      else{
        Serial.println("Battery information: An adequate response to the initial request was
        received");
        if (printHex){
          Serial.println("Battery info hex");
          for (byte i = 0; i <= 7; i++) {
            sprintf(msgString, " 0x%.2X", canBytes[i]);
            Serial.print(msgString);
          }
          Serial.println();  
        }
      }
    }
    numIterations++;
    delay(200);
  }
  
  // Depending of the variable being measured, the information is stored in different ways. 
  for (byte additionalInfoIndex=0; additionalInfoIndex<=4; additionalInfoIndex++){
    if (!readAdditionalData()){
      sprintf(msgString, "Battery information error: An error arose when requesting additional
      package number: %u", additionalInfoIndex);
      Serial.print(msgString);
      return false;
    }
    // If debuging is enable, print the raw data on the serial monitor.
    if (printHex){
      for (byte i = 0; i <= 7; i++) {
        sprintf(msgString, " 0x%.2X", canBytes[i]);
        Serial.print(msgString);
      }
      Serial.println();  
    }   
    // Battery intensity:
    if (canBytes[0] == 0x21){
      // The intensity is the only variable that can be positive (charging) or negative
      //(discharging) because of that, it is returned as 4 bytes that need to be converted
      // into a signed decimal value. This is done using the convertIntensity() function.
      batIntensity = convertIntensity(canBytes[3],canBytes[4],canBytes[5],canBytes[6]);
    }
    // Battery voltage;
    else if (canBytes[0] == 0x23){
      batVoltage = concatBytesU(canBytes[1], canBytes[2]); 
    }
  }
  Serial.println("Battery temperatures: All additional values were received");
  return true;
}

/**
 * This function is defined to read three values of the Nissan Leaf battery temperature.
 * It checks that the received dataframes correspond to the desired information and the
 * results are stored on the array batTemperatures.
 * 
 * The function returns true if it executed properly and false if it encounters any errors. 
*/
bool readBattTemps(){
  if (CAN0.sendMsgBuf(requestID, 8, requestBattTempBytes) != CAN_OK) {
    Serial.println("Battery temperatures error: An error occurred during the initial
    request");
    return false;
  }
  
  delay(100);

  // Variable that indicates if a response to the request has been received.
  bool msgReceived = false;
  // Maximum number of times the wile loop is run. If no value is received before this maximum
  // is reached, the function is aborted and returns false.
  byte maxIterations = 100;
  byte numIterations = 0;
  
  while (!msgReceived){
    
    if (numIterations==maxIterations){
      Serial.println("Battery temperatures error: No response to the initial request received
      before time expired");
      return false;
    }
    msgReceived = !digitalRead(CAN0_INT);
    
    // If CAN0_INT pin is low, read receive buffer    
    if (msgReceived){
      CAN0.readMsgBuf(&IDIncoming, &dlc, canBytes);

      // Determines if message is a remote request frame.
      if ((IDIncoming & 0x40000000) == 0x40000000) {
        Serial.println("Battery temperatures error: The initial request returned a remote
        request frame");
        return false;
      }
      // Checks if the ID of the response is 0x7BB.
      else if (IDIncoming != 1979ul) {
        sprintf(msgString, "Battery temperatures error: Response received from an unexpected
        ID: %.3lX", IDIncoming);
        Serial.println(msgString);
        return false;
      }
      // Checks the header information.
      else if (canBytes[0]!=0x10 || canBytes[1]!=0x1F || canBytes[2]!=0x61|| 
      canBytes[3]!=0x04) {
        Serial.print("Battery temperatures error: The initial frame header is different from 
        expected: ");
        for (byte i = 0; i <= 3; i++) {
          sprintf(msgString, " 0x%.2X", canBytes[i]);
          Serial.print(msgString);
        }
        Serial.println();
        return false;
      }
      else{
        batTemperatures[0] = canBytes[6];
        if (printHex){
          Serial.println("Temps hex");
          for (byte i = 0; i <= 7; i++) {
           sprintf(msgString, " 0x%.2X", canBytes[i]);
           Serial.print(msgString);
          }
          Serial.println();        
        }
      }
    }
    numIterations++;
    delay(200);
  }
   
  for (byte additionalInfoIndex=1; additionalInfoIndex<=4; additionalInfoIndex++){
    if (!readAdditionalData()){
      sprintf(msgString, "Battery temperatures error: An error arose when requesting 
      additional package number: %u", additionalInfoIndex);
      Serial.print(msgString);
      return false;
    }
    if (canBytes[0] == 0x21){
      batTemperatures[1] = canBytes[2];
    }
    else if (canBytes[0] == 0x22){
      batTemperatures[2] = canBytes[1];
    }
    // If debuging is enable, print the raw data on the serial monitor.
    if (printHex){
      for (byte i = 0; i <= 7; i++) {
        sprintf(msgString, " 0x%.2X", canBytes[i]);
        Serial.print(msgString);
      }
      Serial.println();  
    }
  }
  return true;
}

/**
 * This function is defined to read the voltage values of each of the 96 cell pairs of
 * the Nissan Leaf battery. It checks that the received dataframes correspond to the
 * desired information and the results are stored on the array cellVoltages.
 *
 * The function returns true if it executed properly and false if it encounters any errors.
*/
bool readCellVoltages(){
  
   // Initial voltage value request.
  if (CAN0.sendMsgBuf(requestID, 8, requestVoltageCellsBytes) != CAN_OK) {
    Serial.println("Cell voltages error: An error occurred during the initial request");
    return false;
  }
  else{
    Serial.println("Cell voltages: Initial request sent");
  }
  delay(100);

  // Variable that indicates if a response to the request has been received.
  bool msgReceived = false;
  // Maximum number of times the wile loop is run. If no value is received before
  // this maximum is reached, the function is aborted and returns false.
  byte maxIterations = 25;
  byte numIterations = 0;

  // Once the request has been sent the program checks for a response with the appropriate
  // header continuously until a maximum number of iterations is reached.
  while (!msgReceived){
    
    if (numIterations==maxIterations){
      Serial.println("Cell voltages error: No response to the initial request received before
      time expired");
      return false;
    }
    msgReceived = !digitalRead(CAN0_INT);

    // If CAN0_INT pin is low, read receive buffer.
    if (msgReceived){
      CAN0.readMsgBuf(&IDIncoming, &dlc, canBytes);

      // Determines if message is a remote request frame.
      if ((IDIncoming & 0x40000000) == 0x40000000) {
        Serial.println("Cell voltages error: The inital request returned a remote request
        frame");
        return false;
      } 
      // Checks that the ID of the response is 0x7BB.
      else if (IDIncoming != 1979ul) {
        sprintf(msgString, "Cell voltages error: Response received from an unexpected ID:
        %.3lX", IDIncoming);
        Serial.println(msgString);
        return false;
      }
      // Checks the header information.
      else if (canBytes[0]!=0x10 || canBytes[1]!=0xC6 || canBytes[2]!=0x61||
      canBytes[3]!=0x02) {
        Serial.print("Cell voltages error: The initial frame header is different from
        expected: ");
        for (byte i = 0; i < 3; i++) {
          sprintf(msgString, " 0x%.2X", canBytes[i]);
          Serial.print(msgString);
        }
        Serial.println();
        return false;
      }
      // If everything worked the received information is copied to the array.
      else {
        cellVoltages[0] = concatBytesU(canBytes[4],canBytes[5]);
        cellVoltages[1] = concatBytesU(canBytes[6],canBytes[7]);
        Serial.println("Cell voltages: An adequate response to the initial request was
        received");
        if (printHex){
          Serial.println("Cell voltages hex");
          for (byte i = 0; i <= 7; i++) {
           sprintf(msgString, " 0x%.2X", canBytes[i]);
           Serial.print(msgString);
          }
          Serial.println();  
        }
      }
    }
    numIterations++;
    delay(200);
  }  

  // Because each of the additional information frames contains 3.5 voltage values, every two
  // iterations the last byte needs to be stored to be concatenated with the first byte
  // of the following iteration. This byte is stored in the bufferValue variable.
  byte bufferValue;
  // Additionally, because of the bufferValue, on some additional data requests 3 voltages are
  // added to the cellVoltages array and on some others 4 values are added. This variable
  // keeps track of the position of the last voltage value in the array. It is initialized
  // as 1 because the first two voltages are already stored from the initial request.
  byte lastIndexUsed = 1;
  
  for (byte additionalVoltIndex=0; additionalVoltIndex<27; additionalVoltIndex++){
    if (!readAdditionalData()){
      sprintf(msgString, "Cell voltages error: An error arose when requesting additional
      package number: %u", additionalVoltIndex);
      Serial.print(msgString);
      return false;
    }
    // If debuging is enable, print the raw data on the serial monitor.
    if (printHex){
      for (byte i = 0; i <= 7; i++) {
        sprintf(msgString, " 0x%.2X", canBytes[i]);
        Serial.print(msgString);
      }
      Serial.println();  
    }
    if (!(additionalVoltIndex%2)){
      for (byte i = 1; i <= 5; i+=2){
        cellVoltages[++lastIndexUsed] = concatBytesU(canBytes[i],canBytes[i+1]);
      }
      bufferValue = canBytes[7];
    }
    else {
      cellVoltages[++lastIndexUsed] = concatBytesU(bufferValue,canBytes[1]);
      for (byte i = 2; i <= 6; i+=2){
        cellVoltages[++lastIndexUsed] = concatBytesU(canBytes[i],canBytes[i+1]);
      }
    }     
  }
  Serial.println("Cell voltages: All additional values were received");
  return true;
}
