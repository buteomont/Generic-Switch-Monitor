/* A simple, generic program to monitor a switch and report its status via MQTT when it changes.
 * 
 * This processor will sleep until one of two things occur:
 *  1. The processor is reset by momentarily pulling the reset pin low.
 *  2. The number of seconds in reportInterval has passed since the last wakeup.
 * When it wakes, it will connect to the specified router, subscribe to the command
 * topic (<topicRoot/command>) on the specified broker, and publish a set of values.
 * 
 * Configuration is done via serial connection, web page, or MQTT topic.  Enter:
 *  broker=<broker name or address>
 *  port=<port number>   (defaults to 1883)
 *  topicroot=<topic root> (something like buteomont/gate/package/ - must end with / and 
 *  suffixes like "status", "distance", "analog", or "voltage" will be added)
 *  user=<mqtt user>
 *  pass=<mqtt password>
 *  ssid=<wifi ssid>
 *  wifipass=<wifi password>  
 *  reportinterval=<seconds>; //How long to wait between status reports
 *  switchPort=<GPIO number>; // GPIO port to which the monitored switch is connected

 * 
 * Once connected to an MQTT broker, configuration can be done similarly via the 
 * <topicroot>/command topic. Because this program sleeps most of the time, you will need
 * to send a <topicroot>/command with the RETAIN bit set and a message of "reportinterval=0"
 * to keep it awake while you make changes. Reset the reportinterval when you are finished
 * and don't forget to remove the retained MQTT message.
 * 
 * NOTE1: If you're using an ESP8266-01s, don't forget to bodge GPIO16 to the reset pin! 
 * 
 * NOTE2: to upload a new web page from LittleFS, use "pio run --target uploadfs" in a 
 * terminal window.
 *
  */

#include <Arduino.h>
#include <math.h>    
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <EEPROM.h>
#include "switchMonitor.h"

#define VERSION "25.05.17.0"  //remember to update this after every change! YY.MM.DD.REV

ADC_MODE(ADC_VCC); //use the ADC to measure battery voltage

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
AsyncWebServer server(80);

String commandString = "";     // a String to hold incoming commands from serial
bool commandComplete = false;  // goes true when enter is pressed

typedef struct //these are the ports to monitor
  {
  bool isActive; //this entry is being used if this flag is true
  uint8_t gpioNumber;
  char highMessage[MQTT_TOPIC_SUFFIX_SIZE];
  char lowMessage[MQTT_TOPIC_SUFFIX_SIZE];
  bool usePullup;
  } port;

// These are the settings that get stored in EEPROM.  They are all in one struct which
// makes it easier to store and retrieve.
typedef struct 
  {
  unsigned int validConfig=0; 
  char ssid[SSID_SIZE] = "";
  char wifiPassword[PASSWORD_SIZE] = "";
  char mqttBrokerAddress[ADDRESS_SIZE]=""; //default
  int mqttBrokerPort=1883;
  char mqttUsername[USERNAME_SIZE]="";
  char mqttPassword[PASSWORD_SIZE]="";
  char mqttTopicRoot[MQTT_TOPIC_SIZE]="";
  char mqttClientId[MQTT_CLIENTID_SIZE]=""; //will be the same across reboots
  bool debug=true;
  char address[ADDRESS_SIZE]=""; //static address for this device
  char netmask[ADDRESS_SIZE]=""; //size of network
  ulong reportInterval=DEFAULT_REPORT_INTERVAL; //How long to wait between checks
  char mdnsName[ADDRESS_SIZE]=""; //Name to use for MDNS (without .local suffix)
  port ports[PORT_COUNT];
  } conf;
conf settings; //all settings in one struct makes it easier to store in EEPROM
boolean settingsAreValid=false;

IPAddress ip;
IPAddress mask;

ulong keepAwake=0; //this will be updated to allow more time to change settings on the web page

String webMessage="";
bool apModeActive=false;

// These are handy ESP metrics that can be used to measure performance and status.
// --- System/Resource Metrics ---
// uint32_t freeHeap = ESP.getFreeHeap();
// uint8_t heapFragmentation = ESP.getHeapFragmentation(); // Returns a percentage (0-100)
// uint32_t maxFreeBlockSize = ESP.getMaxFreeBlockSize();
// uint32_t chipId = ESP.getChipId();
// String resetReason = ESP.getResetReason();
// uint32_t cpuFreqMHz = ESP.getCpuFreqMHz();
// uint32_t flashChipId = ESP.getFlashChipId();
// uint32_t flashChipSize = ESP.getFlashChipSize(); // Size as seen by SDK
// uint32_t flashChipRealSize = ESP.getFlashChipRealSize(); // Actual physical size
// uint32_t sketchSize = ESP.getSketchSize();
// uint32_t freeSketchSpace = ESP.getFreeSketchSpace(); // Note: Often means space for OTA update

// --- Power/Voltage Metrics ---
// uint32_t vccMilliVolts = ESP.getVcc(); // Requires ADC_MODE(ADC_VCC) in global scope

// --- Network-Related Metrics ---
// long rssi = WiFi.RSSI();
// int wifiStatus = WiFi.status(); // Returns WL_CONNECTED, WL_IDLE, etc.
// IPAddress localIp = WiFi.localIP();
// IPAddress gatewayIp = WiFi.gatewayIP();
// IPAddress subnetMask = WiFi.subnetMask();
// IPAddress dns1Ip = WiFi.dnsIP(); // Primary DNS server
// IPAddress dns2Ip = WiFi.dnsIP(1); // Secondary DNS server (if configured)
// String macAddressStr = WiFi.macAddress(); // Returns MAC as a String (e.g., "AA:BB:CC:DD:EE:FF")
// uint8_t connectedApClients = WiFi.softAPgetStationNum(); // Only if in AP mode


// Accept a port number, and return the index into the settings.ports[] for that port
// Only works for ports 0-5 and 12-16.  Returns -1 otherwise.
int8_t portIndex(int8_t portNumber)
  {
  if (portNumber>16 || (portNumber>5 && portNumber<12))
    {
    Serial.print("Port ");
    Serial.print(portNumber);
    Serial.println(" is invalid.");
    return -1;
    }
  if (portNumber<=5)
    return portNumber;
  else
    return portNumber-6;
  }

// Accept an index into the ports[] array, and return the GPIO number for that port
// Index must be 0-10. This function is the opposite of portIndex()
int8_t indexPort(uint8_t index)
  {
  if (index>10)
    {
    Serial.print("Index ");
    Serial.print(index);
    Serial.println(" is invalid.");
    return -1;
    }
  if (index<=5)
    return index;
  else
    return index+6;
  }

// This will replace placeholders in the HTML with actual settings
String processor(const String& var) 
  {
  char buf[max(SSID_SIZE,max(PASSWORD_SIZE,max(USERNAME_SIZE,MQTT_TOPIC_SIZE)))];
  if (var =="broker")           return settings.mqttBrokerAddress;
  if (var =="port")             return itoa(settings.mqttBrokerPort,buf,10);
  if (var =="topicroot")        return settings.mqttTopicRoot;
  if (var =="user")             return settings.mqttUsername;
  if (var =="pass")             return settings.mqttPassword; 
  if (var =="ssid")             return settings.ssid        ;
  if (var =="wifipass")         return settings.wifiPassword;
  if (var =="address")          return settings.address     ;
  if (var =="netmask")          return settings.netmask     ;
  if (var =="debugChecked")     return settings.debug?" checked":"";
  if (var =="reportinterval")   return itoa(settings.reportInterval,buf,10);
  if (var =="mdnsname")         return settings.mdnsName     ;
  if (var =="gpio0Checked")     return settings.ports[0].isActive?" checked":"";
  if (var =="gpio0highval")     return settings.ports[0].highMessage;
  if (var =="gpio0lowval")      return settings.ports[0].lowMessage;
  if (var =="pullup0Checked")   return settings.ports[0].usePullup?" checked":"";
 
  if (var =="gpio1Checked")     return settings.ports[1].isActive?" checked":"";
  if (var =="gpio1highval")     return settings.ports[1].highMessage;
  if (var =="gpio1lowval")      return settings.ports[1].lowMessage;
  if (var =="pullup1Checked")   return settings.ports[1].usePullup?" checked":"";
  
  if (var =="gpio2Checked")     return settings.ports[2].isActive?" checked":"";
  if (var =="gpio2highval")     return settings.ports[2].highMessage;
  if (var =="gpio2lowval")      return settings.ports[2].lowMessage;
  if (var =="pullup2Checked")   return settings.ports[2].usePullup?" checked":"";
  
  if (var =="gpio3Checked")     return settings.ports[3].isActive?" checked":"";
  if (var =="gpio3highval")     return settings.ports[3].highMessage;
  if (var =="gpio3lowval")      return settings.ports[3].lowMessage;
  if (var =="pullup3Checked")   return settings.ports[3].usePullup?" checked":"";
  
  if (var =="gpio4Checked")     return settings.ports[4].isActive?" checked":"";
  if (var =="gpio4highval")     return settings.ports[4].highMessage;
  if (var =="gpio4lowval")      return settings.ports[4].lowMessage;
  if (var =="pullup4Checked")   return settings.ports[4].usePullup?" checked":"";
  
  if (var =="gpio5Checked")     return settings.ports[5].isActive?" checked":"";
  if (var =="gpio5highval")     return settings.ports[5].highMessage;
  if (var =="gpio5lowval")      return settings.ports[5].lowMessage;
  if (var =="pullup5Checked")   return settings.ports[5].usePullup?" checked":"";
  
  if (var =="gpio12Checked")     return settings.ports[6].isActive?" checked":"";
  if (var =="gpio12highval")     return settings.ports[6].highMessage;
  if (var =="gpio12lowval")      return settings.ports[6].lowMessage;
  if (var =="pullup12Checked")   return settings.ports[6].usePullup?" checked":"";
  
  if (var =="gpio13Checked")     return settings.ports[7].isActive?" checked":"";
  if (var =="gpio13highval")     return settings.ports[7].highMessage;
  if (var =="gpio13lowval")      return settings.ports[7].lowMessage;
  if (var =="pullup13Checked")   return settings.ports[7].usePullup?" checked":"";
  
  if (var =="gpio14Checked")     return settings.ports[8].isActive?" checked":"";
  if (var =="gpio14highval")     return settings.ports[8].highMessage;
  if (var =="gpio14lowval")      return settings.ports[8].lowMessage;
  if (var =="pullup14Checked")   return settings.ports[8].usePullup?" checked":"";
  
  if (var =="gpio15Checked")     return settings.ports[9].isActive?" checked":"";
  if (var =="gpio15highval")     return settings.ports[9].highMessage;
  if (var =="gpio15lowval")      return settings.ports[9].lowMessage;
  if (var =="pullup15Checked")   return settings.ports[9].usePullup?" checked":"";
  
  if (var =="gpio16Checked")     return settings.ports[10].isActive?" checked":"";
  if (var =="gpio16highval")     return settings.ports[10].highMessage;
  if (var =="gpio16lowval")      return settings.ports[10].lowMessage;
  if (var =="pullup16Checked")   return settings.ports[10].usePullup?" checked":"";
  
  if (var =="message")       
    {
    String msg=webMessage;
    webMessage="";    //only display the message once
    Serial.println(msg);
    return msg; 
    }
  return String();
  }

void showSettings()
  {
  Serial.print("broker=<MQTT broker host name or address> (");
  Serial.print(settings.mqttBrokerAddress);
  Serial.println(")");
  Serial.print("port=<port number>   (");
  Serial.print(settings.mqttBrokerPort);
  Serial.println(")");
  Serial.print("topicroot=<topic root> (");
  Serial.print(settings.mqttTopicRoot);
  Serial.println(")  Note: must end with \"/\"");  
  Serial.print("user=<mqtt user> (");
  Serial.print(settings.mqttUsername);
  Serial.println(")");
  Serial.print("pass=<mqtt password> (");
  Serial.print(settings.mqttPassword);
  Serial.println(")");
  Serial.print("ssid=<wifi ssid> (");
  Serial.print(settings.ssid);
  Serial.println(")");
  Serial.print("wifipass=<wifi password> (");
  Serial.print(settings.wifiPassword);
  Serial.println(")");
  Serial.print("address=<Static IP address if so desired> (");
  Serial.print(settings.address);
  Serial.println(")");
  Serial.print("netmask=<Network mask to be used with static IP> (");
  Serial.print(settings.netmask);
  Serial.println(")");
  Serial.print("mdnsname=<Name to use (without .local) for MDNS> (");
  Serial.print(settings.mdnsName);
  Serial.println(")");
  Serial.print("debug=1|0 (");
  Serial.print(settings.debug);
  Serial.println(")");
  Serial.print("reportinterval=<seconds>   (");
  Serial.print(settings.reportInterval);
  Serial.println(")");
  
  Serial.println("Ports:");
  bool noActivePorts=true;
  for (int i=0;i<PORT_COUNT;i++)
    {
    port& iport=settings.ports[i];
    if (iport.isActive)
      {
      Serial.printf("GPIO=%d\tHigh Topic=%s\tLow Topic=%s\n",
                    iport.gpioNumber,
                    iport.highMessage,
                    iport.lowMessage);
      noActivePorts=false;
      }
    yield();
    }
  if (noActivePorts)
    Serial.println("No ports configured.");

  Serial.print("MQTT Client ID is ");
  Serial.println(settings.mqttClientId);
  Serial.print("Address is ");
  Serial.println(wifiClient.localIP());
  Serial.println("To assign ports, use \"portadd=gpio,highmessage,lowmessage,usepullup\"");
  Serial.println("To remove a port, use \"portremove=gpio\"");
  Serial.println("\n*** Use NULL to reset a setting to its default value ***");
  Serial.println("*** Use \"resetmqttid=yes\" to reset all settings  ***");
  Serial.println("*** Use \"factorydefaults=yes\" to reset all settings  ***\n");
  
  Serial.print("\nSettings are ");
  Serial.println(settingsAreValid?"valid.":"incomplete.");
  }

  
/*
 * Check for configuration input via the serial port.  Return a null string 
 * if no input is available or return the complete line otherwise.
 */
String getConfigCommand()
  {
  if (commandComplete) 
    {
    Serial.println(commandString);
    String newCommand=commandString;
    if (newCommand.length()==0)
      newCommand='\n'; //to show available commands

    commandString = "";
    commandComplete = false;
    return newCommand;
    }
  else return "";
  }

bool processCommand(String cmd)
  {
  bool commandFound=true; //saves a lot of code
  char nme[30]; //shouldn't get any commands larger than this
  const char *str=cmd.c_str();
  char *val=NULL;
  char *nme_t=strtok((char *)str,"=");
  strcpy(nme,nme_t);//Don't modify c_str() pointers
  if (nme_t!=NULL)
    val=strtok(NULL,"=");
  else
    strcpy(nme,"\n"); 
  
  if (nme[0]=='\n' || nme[0]=='\r' || nme[0]=='\0') //a single cr means show current settings
    {
    showSettings();
    commandFound=false; //command not found
    }
  else
    {
    //Get rid of the carriage return
    if (val!=NULL && strlen(val)>0 && val[strlen(val)-1]==13)
      val[strlen(val)-1]=0; 

    if (val!=NULL)
      {
      if (strcmp(val,"NULL")==0) //to nullify a value, you have to really mean it
        {
        strcpy(val,"");
        }
      
      if (strcmp(nme,"broker")==0)
        {
        strcpy(settings.mqttBrokerAddress,val);
        saveSettings();
        }
      else if (strcmp(nme,"port")==0)
        {
        if (!val)
          strcpy(val,"0");
        settings.mqttBrokerPort=atoi(val);
        saveSettings();
        }
      else if (strcmp(nme,"topicroot")==0)
        {
        strcpy(settings.mqttTopicRoot,val);
        if (val[strlen(val)-1] !='/') // must end with a /
          {
          strcat(settings.mqttTopicRoot,"/");
          }
        saveSettings();
        }
      else if (strcmp(nme,"user")==0)
        {
        strcpy(settings.mqttUsername,val);
        saveSettings();
        }
      else if (strcmp(nme,"pass")==0)
        {
        strcpy(settings.mqttPassword,val);
        saveSettings();
        }
      else if (strcmp(nme,"ssid")==0)
        {
        strcpy(settings.ssid,val);
        saveSettings();
        }
      else if (strcmp(nme,"wifipass")==0)
        {
        strcpy(settings.wifiPassword,val);
        saveSettings();
        }
      else if (strcmp(nme,"address")==0)
        {
        strcpy(settings.address,val);
        saveSettings();
        }
      else if (strcmp(nme,"mdnsname")==0)
        {
        strcpy(settings.mdnsName,val);
        saveSettings();
        }
      else if (strcmp(nme,"netmask")==0)
        {
        strcpy(settings.netmask,val);
        saveSettings();
        }
      else if (strcmp(nme,"debug")==0)
        {
        if (!val)
          strcpy(val,"0");
        settings.debug=atoi(val)==1?true:false;
        saveSettings();
        }
      else if (strcmp(nme,"reportinterval")==0)
        {
        if (!val)
          strcpy(val,"0");
        settings.reportInterval=atoi(val);
        saveSettings();
        }

      // "portadd=gpio,highmessage,lowmessage,usePullup" should add a port
      else if (strcmp(nme,"portadd")==0)
        {
        if (val)
          {
          char *portnum=strtok(val,",");
          char *hitopic=strtok(NULL,",");
          char *lotopic=strtok(NULL,",");
          char *usePullup=strtok(NULL,",");
          uint8_t port=atoi(portnum);
          int8_t index=portIndex(port);
          if (index>=0)
            {
            settings.ports[index].isActive=true;
            settings.ports[index].gpioNumber=port;
            if (hitopic)
              {  
              strncpy(settings.ports[index].highMessage,hitopic,MQTT_TOPIC_SUFFIX_SIZE-1);
              settings.ports[index].highMessage[MQTT_TOPIC_SUFFIX_SIZE-1]='\0'; //ensure null termination
              }
            else
              strcpy(settings.ports[index].highMessage,"high");

            if (lotopic)
              {  
              strncpy(settings.ports[index].lowMessage,lotopic,MQTT_TOPIC_SUFFIX_SIZE-1);
              settings.ports[index].lowMessage[MQTT_TOPIC_SUFFIX_SIZE-1]='\0'; //ensure null termination
              }
            else
              strcpy(settings.ports[index].lowMessage,"low");

            if (usePullup)
              settings.ports[index].usePullup=true;
            else
              settings.ports[index].usePullup=false;

            saveSettings();
            }
          else
            commandFound=false;
          }
        }

      // "portremove=gpio" should remove a port
     else if (strcmp(nme,"portremove")==0)
        {
        if (val)
          {
          uint8_t port=atoi(val);
          int8_t index=portIndex(port);
          if (index>=0)
            {
            settings.ports[index].isActive=false;
            saveSettings();
            }
          else
            commandFound=false;
          }
        }

      else if ((strcmp(nme,"resetmqttid")==0)&& (strcmp(val,"yes")==0))
        {
        generateMqttClientId(settings.mqttClientId);
        saveSettings();
        }
      else if ((strcmp(nme,"factorydefaults")==0) && (strcmp(val,"yes")==0)) //reset all eeprom settings
        {
        Serial.println("\n*********************** Resetting EEPROM Values ************************");
        initializeSettings();
        saveSettings();
        delay(2000);
        ESP.restart();
        }
      else
        {
        showSettings();
        commandFound=false; //command not found
        }
      }
    }
  keepAwake=millis()+STAY_AWAKE_INCREMENT; //stay awake a little longer for more web changes
  return commandFound;
  }

void initializeSettings()
  {
  settings.validConfig=0; 
  strcpy(settings.ssid,"");
  strcpy(settings.wifiPassword,"");
  strcpy(settings.mqttBrokerAddress,""); //default
  settings.mqttBrokerPort=1883;
  strcpy(settings.mqttUsername,"");
  strcpy(settings.mqttPassword,"");
  strcpy(settings.mqttTopicRoot,"");
  strcpy(settings.mdnsName,"");
  strcpy(settings.address,"");
  strcpy(settings.netmask,"255.255.255.0");
  settings.reportInterval=DEFAULT_REPORT_INTERVAL;
  generateMqttClientId(settings.mqttClientId);
  for (int i=0;i<PORT_COUNT;i++)
    settings.ports[i].isActive=false;
  }

void checkForCommand()
  {
  if (Serial.available())
    {
    incomingSerialData();
    String cmd=getConfigCommand();
    if (cmd.length()>0)
      {
      yield();
      processCommand(cmd);
      }
    }
  }


/************************
 * Do the MQTT thing
 ************************/
bool report()
  {
  char topic[MQTT_TOPIC_SIZE+9];
  char reading[18];
  bool ok=true;

  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,MQTT_PAYLOAD_STATUS_COMMAND);
  // bool switchStatus=digitalRead(settings.switchPort);
  // if (switchStatus!=settings.activeLow) //means the device has triggered (activeLow=1)
  //   publish(topic,MQTT_PAYLOAD_TRIPPED_STATUS,true);
  // else
  //   publish(topic,MQTT_PAYLOAD_ARMED_STATUS,true);
  for (int i=0;i<PORT_COUNT;i++)
    {
    if (settings.ports[i].isActive)
      {
      bool switchStatus=digitalRead(settings.ports[i].gpioNumber);
      publish(topic,switchStatus?settings.ports[i].highMessage:settings.ports[i].lowMessage,false);
      }
    }

  //publish the radio strength reading while we're at it
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,MQTT_TOPIC_RSSI);
  sprintf(reading,"%d",WiFi.RSSI()); 
  ok=ok & publish(topic,reading,true); //retain

  //publish the battery voltage
  uint32_t vccMilliVolts = ESP.getVcc(); // millivolts
  float vccVolts = (float)vccMilliVolts / 1000.0;// Convert to Volts
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,MQTT_TOPIC_BATTERY);
  sprintf(reading,"%.2f",vccVolts); 
  ok=ok & publish(topic,reading,true); //retain

  // Publish some memory usage info
  uint32_t freeHeap = ESP.getFreeHeap();
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,MQTT_TOPIC_FREE_HEAP);
  sprintf(reading,"%d",freeHeap); 
  ok=ok & publish(topic,reading,true); //retain

  uint8_t heapFragmentation = ESP.getHeapFragmentation(); // Returns a percentage (0-100)
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,MQTT_TOPIC_HEAP_FRAGMENTATION);
  sprintf(reading,"%d%%",heapFragmentation); 
  ok=ok & publish(topic,reading,true); //retain

  uint32_t maxFreeBlockSize = ESP.getMaxFreeBlockSize();
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,MQTT_TOPIC_MAX_FREE_BLOCK_SIZE);
  sprintf(reading,"%d",maxFreeBlockSize); 
  ok=ok & publish(topic,reading,true); //retain
  
  if (settings.debug)
    {
    Serial.print("Publish ");
    Serial.println(ok?"OK":"Failed");
    }
  return ok;
  }


boolean publish(char* topic, const char* reading, boolean retain)
  {
  if (settings.debug)
    {
    Serial.print(topic);
    Serial.print(" ");
    Serial.println(reading);
    }
  boolean ok=false;
  connectToWiFi(); //just in case we're disconnected from WiFi
  reconnectToBroker(); //also just in case we're disconnected from the broker

  if (mqttClient.connected() && 
      settings.mqttTopicRoot &&
      WiFi.status()==WL_CONNECTED)
    {
    ok=mqttClient.publish(topic,reading,retain); 
    }
  else
    {
    Serial.print("Can't publish due to ");
    if (WiFi.status()!=WL_CONNECTED)
      Serial.println("no WiFi connection.");
    else if (!mqttClient.connected())
      Serial.println("not connected to broker.");
    }
  return ok;
  }



/**
 * Handler for incoming MQTT messages.  The payload is the command to perform. 
 * The MQTT message topic sent is the topic root plus the command.
 * Implemented commands are: 
 * MQTT_PAYLOAD_SETTINGS_COMMAND: sends a JSON payload of all user-specified settings
 * MQTT_PAYLOAD_REBOOT_COMMAND: Reboot the controller
 * MQTT_PAYLOAD_VERSION_COMMAND Show the version number
 * MQTT_PAYLOAD_STATUS_COMMAND Show the most recent flow values
 */
void incomingMqttHandler(char* reqTopic, byte* payload, unsigned int length) 
  {
  if (settings.debug)
    {
    Serial.println("====================================> Callback works.");
    }
  payload[length]='\0'; //this should have been done in the calling code, shouldn't have to do it here
  boolean rebootScheduled=false; //so we can reboot after sending the reboot response
  char charbuf[100];
  sprintf(charbuf,"%s",payload);
  const char* response;
  
  
  //if the command is MQTT_PAYLOAD_SETTINGS_COMMAND, send all of the settings
  if (strcmp(charbuf,MQTT_PAYLOAD_SETTINGS_COMMAND)==0)
    {
    char tempbuf[35]; //for converting numbers to strings
    char jsonStatus[JSON_STATUS_SIZE];
    
    strcpy(jsonStatus,"{");
    strcat(jsonStatus,"\"broker\":\"");
    strcat(jsonStatus,settings.mqttBrokerAddress);
    strcat(jsonStatus,"\", \"port\":");
    sprintf(tempbuf,"%d",settings.mqttBrokerPort);
    strcat(jsonStatus,tempbuf);
    strcat(jsonStatus,", \"topicroot\":\"");
    strcat(jsonStatus,settings.mqttTopicRoot);
    strcat(jsonStatus,"\", \"user\":\"");
    strcat(jsonStatus,settings.mqttUsername);
    strcat(jsonStatus,"\", \"pass\":\"");
    strcat(jsonStatus,settings.mqttPassword);
    strcat(jsonStatus,"\", \"ssid\":\"");
    strcat(jsonStatus,settings.ssid);
    strcat(jsonStatus,"\", \"wifipass\":\"");
    strcat(jsonStatus,settings.wifiPassword);
    strcat(jsonStatus,"\", \"mqttClientId\":\"");
    strcat(jsonStatus,settings.mqttClientId);
    strcat(jsonStatus,"\", \"address\":\"");
    strcat(jsonStatus,settings.address);
    strcat(jsonStatus,"\", \"netmask\":\"");
    strcat(jsonStatus,settings.netmask);
    strcat(jsonStatus,"\", \"mdnsname\":\"");
    strcat(jsonStatus,settings.mdnsName);
    strcat(jsonStatus,"\", \"debug\":\"");
    strcat(jsonStatus,settings.debug?"true":"false");
    strcat(jsonStatus,"\", \"reportinterval\":");
    sprintf(tempbuf,"%lu",settings.reportInterval);
    strcat(jsonStatus,tempbuf);
    strcat(jsonStatus,", \"IPAddress\":\"");
    strcat(jsonStatus,wifiClient.localIP().toString().c_str());
    strcat(jsonStatus,"\",");
    strcat(jsonStatus,"\"ports\":[");
    for (int i=0;i<PORT_COUNT;i++)
      {
      if (settings.ports[i].isActive)
        {
        strcat(jsonStatus,"{\"GPIO\":");
        sprintf(tempbuf,"%d",settings.ports[i].gpioNumber);
        strcat(jsonStatus,tempbuf);
        strcat(jsonStatus,", \"highmessage\":\"");
        strcat(jsonStatus,settings.ports[i].highMessage);
        strcat(jsonStatus,"\", \"lowmessage\":\"");
        strcat(jsonStatus,settings.ports[i].lowMessage);
        strcat(jsonStatus,"\", \"usePullup\":\"");
        strcat(jsonStatus,settings.ports[i].usePullup?"true":"false");
        strcat(jsonStatus,"\"},");
        }
      yield();
      }
    size_t len = strlen(jsonStatus);
    if (jsonStatus[len - 1] == ',')
      jsonStatus[len - 1] = ']';   //replace the last comma to close the array
    else
      strcat(jsonStatus,"]"); //happens when port array is empty
    
    strcat(jsonStatus,"}");
    response=jsonStatus;
    }
  else if (strcmp(charbuf,MQTT_PAYLOAD_VERSION_COMMAND)==0) //show the version number
    {
    char tmp[15];
    strcpy(tmp,VERSION);
    response=tmp;
    }
  else if (strcmp(charbuf,MQTT_PAYLOAD_STATUS_COMMAND)==0) //show the latest value
    {
    report();
    char tmp[25];
    strcpy(tmp,"Status report complete");
    response=tmp;
    }
  else if (strcmp(charbuf,MQTT_PAYLOAD_REBOOT_COMMAND)==0) //reboot the controller
    {
    char tmp[10];
    strcpy(tmp,"REBOOTING");
    response=tmp;
    rebootScheduled=true;
    }
  else if (processCommand(charbuf))
    {
    response="OK";
    }
  else
    {
    char badCmd[18];
    strcpy(badCmd,"(empty)");
    response=badCmd;
    }
    
  char topic[MQTT_TOPIC_SIZE];
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,charbuf); //the incoming command becomes the topic suffix

  if (!publish(topic,response,false)) //do not retain
    Serial.println("************ Failure when publishing status response!");
    
  delay(2000); //give publish time to complete
  
  if (rebootScheduled)
    {
    ESP.restart();
    }
  }


//Generate an MQTT client ID.  This should not be necessary very often
char* generateMqttClientId(char* mqttId)
  {
  strcpy(mqttId,MQTT_CLIENT_ID_ROOT);
  strcat(mqttId, String(random(0xffff), HEX).c_str());
  if (settings.debug)
    {
    Serial.print("New MQTT userid is ");
    Serial.println(mqttId);
    }
  return mqttId;
  }

/*
 * Reconnect to the MQTT broker
 */
void reconnectToBroker() 
  {
  if (strlen(settings.mqttBrokerAddress)>0)
    {
    if (WiFi.status() != WL_CONNECTED)
      {
      Serial.println("WiFi not ready, skipping MQTT connection");
      }
    else
      {
      // Loop until we're reconnected
      while (!mqttClient.connected()) 
        {
        Serial.print("Attempting MQTT connection...");

        mqttClient.setBufferSize(JSON_STATUS_SIZE); //default (256) isn't big enough
        mqttClient.setKeepAlive(120); //seconds
        mqttClient.setServer(settings.mqttBrokerAddress, settings.mqttBrokerPort);
        mqttClient.setCallback(incomingMqttHandler);
        yield();

        // Attempt to connect
        if (mqttClient.connect(settings.mqttClientId,settings.mqttUsername,settings.mqttPassword))
          {
          Serial.println("connected to MQTT broker.");

          //resubscribe to the incoming message topic
          char topic[MQTT_TOPIC_SIZE];
          strcpy(topic,settings.mqttTopicRoot);
          strcat(topic,MQTT_TOPIC_COMMAND_REQUEST);
          bool subgood=mqttClient.subscribe(topic);
          showSub(topic,subgood);
          }
        else 
          {
          Serial.print("failed, rc=");
          Serial.println(mqttClient.state());
          Serial.println("Will try again in a second");
          
          // Wait a second before retrying
          // In the meantime check for input in case something needs to be changed to make it work
        //  checkForCommand(); 
          yield();
          delay(1000);
          yield();
          }
        checkForCommand();
        }
      mqttClient.loop(); //This has to happen every so often or we get disconnected for some reason
      }
    }
  else if (settings.debug)
    {
    Serial.println("Broker address not set, ignoring MQTT");
    }
  }

void showSub(char* topic, bool subgood)
  {
  if (settings.debug)
    {
    Serial.print("++++++Subscribing to ");
    Serial.print(topic);
    Serial.print(":");
    Serial.println(subgood);
    }
  }

/**
 * @brief Checks if a C-style string (char array) contains only
 * alphanumeric characters (a-z, A-Z, 0-9).
 *
 * @param str The null-terminated char array to check.
 * @return true if all characters are alphanumeric or the string is empty.
 * @return false if any character is not alphanumeric.
 */
bool checkString(const char* str) 
  {
    if (str == nullptr) 
      {
      return false; // A null pointer is not a valid string
      }

    // Iterate through the string until the null terminator
    for (size_t i = 0; str[i] != '\0'; ++i) 
      {
      // Cast char to unsigned char to avoid potential issues with 
      // negative char values when passed to ctype functions.
      unsigned char ch = static_cast<unsigned char>(str[i]); // Cast once
      if (!isalnum(ch) && ch != '/' && ch != '.')
        {
        return false; // Found a non-alphanumeric character
        }
      }

  // If the loop completes, all characters were alphanumeric (or the string was empty)
  return true;
  }

// Check all of the text in all active ports for sanity
bool checkPorts()
  {
  bool hasOne=false; //settings are incomplete unless we have at least one port activated
  for (int i=0;i<PORT_COUNT;i++)
    {
    if (settings.ports[i].isActive)
      {
      hasOne=true;
      if (!(checkString(settings.ports[i].highMessage) &&
             checkString(settings.ports[i].lowMessage)))
        return false;
      }
    else //clear out any that are inactive
      {
      settings.ports[i].gpioNumber=0;
      settings.ports[i].highMessage[0]='\0';
      settings.ports[i].lowMessage[0]='\0';
      settings.ports[i].usePullup=false;
      }
//    yield();
    }
  return true && hasOne;
  }

/*
 * Check all of the strings in the settings.   If any of the
 * character strings in the settings fail the test, it's 
 * likely that the settings are corrupt. 
*/  
bool settingsSanityCheck()
  {
  return checkString(settings.ssid)
      && checkString(settings.wifiPassword)
      && checkString(settings.mqttBrokerAddress)
      && checkString(settings.mqttUsername)
      && checkString(settings.mqttPassword)
      && checkString(settings.mqttTopicRoot)
      && checkString(settings.mqttClientId)
      && checkString(settings.address)
      && checkString(settings.mdnsName)
      && checkString(settings.netmask)
      && checkPorts()
      ;
  }

/*
 * Save the settings to EEPROM. Set the valid flag if everything is filled in.
 */
boolean saveSettings()
  {
  if (strlen(settings.ssid)>0 &&
      strlen(settings.wifiPassword)>0 &&
      // strlen(settings.mqttBrokerAddress)>0 &&
      // settings.mqttBrokerPort!=0 &&
      strlen(settings.mqttTopicRoot)>0 &&
      strlen(settings.mqttClientId)>0 &&
      settingsSanityCheck())
    {
    Serial.println("Settings deemed complete");
    settings.validConfig=VALID_SETTINGS_FLAG;
    settingsAreValid=true;
    }
  else
    {
    Serial.println("Settings still incomplete");
    settings.validConfig=0;
    settingsAreValid=false;
    }
    
  //The mqttClientId is not set by the user, but we need to make sure it's set  
  if (strlen(settings.mqttClientId)==0)
    {
    generateMqttClientId(settings.mqttClientId);
    }
    
  EEPROM.put(0,settings);
  if (settings.debug)
    Serial.println("Committing settings to eeprom");
  return EEPROM.commit();
  }

  //This is a special initialization function. Do not call except when necessary.
void initTopicForTesting()
  {
  strcpy(settings.mqttTopicRoot,"buteomont/mousetest/"); // To keep from messing up the real mousetrap
  saveSettings();
  }

void erasePortsForTesting()
  {
  for (int i=0;i<PORT_COUNT;i++)
    {
    settings.ports[i].isActive=false;
    settings.ports[i].highMessage[0]='\0';
    settings.ports[i].lowMessage[0]='\0';
    }
  saveSettings();
  }

void initSerial()
  {
  Serial.begin(115200);
  Serial.setTimeout(10000);
  
  while (!Serial); // wait here for serial port to connect.
  Serial.println();
  Serial.println("Serial communications established.");
  delay(5000);
  commandString.reserve(200); // reserve 200 bytes of serial buffer space for incoming command string
  }

void initFS()
  {
  if (!LittleFS.begin()) 
    Serial.println("Failed to mount FS");
  else
    {
    Serial.println("File system started.");
    //   Serial.println("Listing LittleFS contents:");
    // Dir dir = LittleFS.openDir("/"); // Open the root directory
    // while (dir.next())
    //   {
    //   Serial.print("  FILE: ");
    //   Serial.print(dir.fileName());
    //   Serial.print("  SIZE: ");
    //   Serial.println(dir.fileSize());
    //   }
    // Serial.println("-------------------------");
    }

  }

/*
*  Initialize the settings from eeprom and determine if they are valid
*/
void loadSettings()
  {
  EEPROM.get(0,settings);

  if (!settingsSanityCheck()) //if something is wildly off then don't run, allow setup
    {
    settings.validConfig=0;
    settingsAreValid=false;
    Serial.println(F("Settings are corrupt, marking invalid."));
    }
  else if (settings.validConfig==VALID_SETTINGS_FLAG)    //skip loading stuff if it's never been written
    {
    settingsAreValid=true;
    if (settings.debug)
      {
      Serial.println("\nLoaded configuration values from EEPROM");
      }
    }
  else
    {
    Serial.println("Skipping load from EEPROM, device not configured.");    
    settingsAreValid=false;
    }
    showSettings();
  }

void reconfigSerial()
  {
  if (settings.ports[TX_PIN].isActive && settings.ports[RX_PIN].isActive)
    {
    Serial.println("*******************************************");
    Serial.println("* Both TX and RX are being used for GPIO. *");
    Serial.println("* Serial UART is being deactivated!       *");
    Serial.println("*******************************************");
    Serial.flush();
    Serial.end();
    }
  else if (settings.ports[RX_PIN].isActive)
    {
    Serial.println("****************************************");
    Serial.println("* The RX port is being used for GPIO.  *");
    Serial.println("* Serial receive is being deactivated! *");
    Serial.println("****************************************");
    Serial.flush();
    Serial.begin(115200,SERIAL_8N1,SERIAL_TX_ONLY); 
    }    
  else if (settings.ports[RX_PIN].isActive)
    {
    Serial.println("***************************************");
    Serial.println("* The TX port is being used for GPIO. *");
    Serial.println("* Serial.print is being deactivated!  *");
    Serial.println("***************************************");
    Serial.flush();
    Serial.begin(115200,SERIAL_8N1,SERIAL_TX_ONLY); 
    }
  else
    Serial.println("No port adjustments necessary.");  
  }

void initPorts()
  {
  for (int i=0;i<PORT_COUNT;i++)
    {
    if (settings.ports[i].isActive)
      {
      int8_t port=indexPort(i);
      if (port>=0)
        {
        pinMode(port,settings.ports[i].usePullup?INPUT_PULLUP:INPUT);
        }
      }
    }
  }

void initSettings()
  {
  EEPROM.begin(sizeof(settings)); //fire up the eeprom section of flash
  loadSettings(); //set the values from eeprom 

  //show the MAC address
  Serial.print("ESP8266 MAC Address: ");
  Serial.println(WiFi.macAddress());

  if (settings.mqttBrokerPort < 0) //then this must be the first powerup
    {
    Serial.println("\n*********************** Resetting All EEPROM Values ************************");
    initializeSettings();
    saveSettings();
    delay(2000);
    ESP.restart();
    }
  }

void startAPMode() 
  {
  Serial.println("\nStarting SoftAP Mode...");
  WiFi.mode(WIFI_AP); // Set mode to AP
  // Optional: Set a static IP for the AP (defaults to 192.168.4.1)
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  bool result = WiFi.softAP(STANDALONE_SSID, "password");
  apModeActive=true;

  if (result) 
    {
    Serial.print("SoftAP '" + String(STANDALONE_SSID) + "' started. IP: ");
    Serial.println(WiFi.softAPIP());
    } 
  else 
    {
    Serial.println("Failed to start SoftAP!");
    }
  keepAwake=millis()+STAY_AWAKE_INCREMENT; //stay awake a while for changes via web page
  }

/**
 * @brief Checks if the ESP8266 SoftAP is actively running.
 *
 * This function verifies if the current WiFi mode includes AP functionality
 * and that the SoftAP interface has a valid IP address.
 *
 * @return true if SoftAP is running and initialized, false otherwise.
 */
// bool isSoftAPRunning()
//   {
//   // Check if the current Wi-Fi mode is AP or AP_STA
//   if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA)
//     {
//     // Additionally, check if the SoftAP has successfully obtained an IP address.
//     // If the IP is 0.0.0.0, it usually means the AP failed to start correctly.
//     if (WiFi.softAPIP()[0] != 0)
//       {
//       return true; // AP mode is active and has an IP
//       }
//     }
//   return false; // AP mode is not active or hasn't got an IP
//   }

/*
 * If not connected to wifi, connect.
 */
void connectToWiFi()
  {
  if (settingsAreValid && WiFi.status() != WL_CONNECTED && !apModeActive)
    {
    Serial.print("Attempting to connect to WPA SSID \"");
    Serial.print(settings.ssid);
    Serial.println("\"");

    WiFi.disconnect(true); // Completely reset Wi-Fi stack
    delay(100); // Small delay to ensure reset is applied
    WiFi.persistent(false); // Prevent saving to flash
    WiFi.mode(WIFI_STA); //station mode, we are only a client in the wifi world

    if (ip.isSet()) //Go with a dynamic address if no valid IP has been entered
      {
      if (!WiFi.config(ip,ip,mask))
        {
        Serial.println("STA Failed to configure");
        }
      }

    unsigned long connectTimeout = millis() + WIFI_TIMEOUT_SECONDS*1000; // 30 second timeout
    WiFi.begin(settings.ssid, settings.wifiPassword);
    //delay(1000);
    unsigned long lastDotTime = millis(); // For printing dots without blocking
    while (WiFi.status() != WL_CONNECTED && millis() < connectTimeout) 
      {
      // Not yet connected
      if (millis() - lastDotTime > 500) // Print dot every 500ms, but don't block
        {
        Serial.print(".");
        lastDotTime = millis();
        yield();
        }
      checkForCommand(); // Check for input in case something needs to be changed to work
      yield();
      }

    if (WiFi.status() != WL_CONNECTED)
      {
      Serial.println("\nConnection to network failed. Opening AP mode.");
      startAPMode();  //fire up our own network
      }
    else 
      {
      Serial.print("\nConnected to network with address ");
      Serial.println(WiFi.localIP());
      Serial.println();
      }
    // server.begin();
    }
  }

void initServer()
  {
  connectToWiFi(); //will either connect to wifo or set up AP mode
  server.begin();

  Serial.print("Setting MDNS name to ");
  Serial.print(settings.mdnsName);
  Serial.println(".local");

  if (!MDNS.begin(settings.mdnsName)) // Always check return value!
    {
    Serial.println("Error setting up MDNS responder!");
    }
    else
    {
    Serial.println("mDNS responder started successfully.");
    MDNS.addService("http", "tcp", 80); // Add service after mDNS is running
    Serial.println("HTTP service added to mDNS.");
    }  
  }

void notFound(AsyncWebServerRequest *request) 
  {
  request->send(404, "text/plain", "Not found");
  }

void setup()
  {
  initSerial();
  
  initSettings();

  initFS();
 
  if (!settingsAreValid) //we need more settings, allow it via the web page
    startAPMode();

  initServer();

  if (settingsAreValid)
    {      
    reconfigSerial(); //settings are valid, reconfigure the serial port if necessary
    initPorts();  // Initialize the I/O ports based on settings

    if (settings.debug)
      {
      if (!ip.fromString(settings.address)&& !apModeActive)
        {
        Serial.println("Static IP Address '"+String(settings.address)+"' is blank or not valid. Using dynamic addressing.");
        // settingsAreValid=false;
        // settings.validConfig=false;
        }
      else if (!mask.fromString(settings.netmask)&& !apModeActive)
        {
        Serial.println("Static network mask "+String(settings.netmask)+" is not valid.");
        // settingsAreValid=false;
        // settings.validConfig=false;
        }
      }
    }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) 
    {
    
    // if (settings.debug) //prove that the request and file are ok
    //   {
    //   Serial.println("Got request for index page.");

    //   File file = LittleFS.open("index.html", "r"); // Open the file for reading
    //   if (file)
    //     {
    //     // Read and print each character until the end of the file
    //     while (file.available())
    //       {
    //       Serial.write(file.read()); // Use Serial.write for raw bytes, good for large files
    //       }
    //     file.close(); 
    //     }
    //   else
    //     {
    //     Serial.println("Can't open file.");
    //     }
    //   }
    Serial.println("*********** Got web request ****************");
    request->send(LittleFS, "/index.html", "text/html", false, processor);
    keepAwake=millis()+STAY_AWAKE_INCREMENT; //stay awake a little longer for more web changes
    });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) 
    {
    // if (settings.debug)
    //   {
    //   int params = request->params(); // total number of parameters in this request
    //   for (int i = 0; i < params; i++)
    //     {
    //     AsyncWebParameter* p = request->getParam(i);
    //     Serial.printf("Form Data - Name: '%s', Value: '%s'\n", p->name().c_str(), p->value().c_str());
    //     }
    //   }
    Serial.println("******************** Saving form **********************");
    bool changed=false; //starts out with nothing changed

    if (request->hasParam("ssid", true))
      {
      const char* val = request->getParam("ssid", true)->value().c_str();
      if (strcmp(val, settings.ssid) != 0)  
        {
        snprintf(settings.ssid,sizeof(settings.ssid),"%s",val);
        changed=true;
        }
      }
    if (request->hasParam("wifipass", true))
      {
      const char* val = request->getParam("wifipass", true)->value().c_str();
      if (strcmp(val, settings.wifiPassword) != 0)  
        {
        snprintf(settings.wifiPassword,sizeof(settings.wifiPassword),"%s",val);
        changed=true;
        }
      }
    if (request->hasParam("address", true))
      {
      const char* val = request->getParam("address", true)->value().c_str();
      if (strcmp(val, settings.address) != 0)  
        {
        snprintf(settings.address,sizeof(settings.address),"%s",val);
        changed=true;
        }
      }
    if (request->hasParam("netmask", true))
      {
      const char* val = request->getParam("netmask", true)->value().c_str();
      if (strcmp(val, settings.netmask) != 0)  
        {
        snprintf(settings.netmask,sizeof(settings.netmask),"%s",val);
        changed=true;
        }
      }
    if (request->hasParam("broker", true))
      {
      const char* val = request->getParam("broker", true)->value().c_str();
      if (strcmp(val, settings.mqttBrokerAddress) != 0)  
        {
        snprintf(settings.mqttBrokerAddress,sizeof(settings.mqttBrokerAddress),"%s",val);
        changed=true;
        }
      }
    if (request->hasParam("port", true))
      {
      int val = atoi(request->getParam("port", true)->value().c_str());
      if (val != settings.mqttBrokerPort)  
        {
        settings.mqttBrokerPort=val;
        changed=true;
        }
      }
    if (request->hasParam("topicroot", true))
      {
      String vals = request->getParam("topicroot", true)->value();
      if (!vals.endsWith("/"))  //enforce last slash rule
        vals+="/";
      const char* val = vals.c_str();

      if (strcmp(val, settings.mqttTopicRoot) != 0)  
        {
        snprintf(settings.mqttTopicRoot,sizeof(settings.mqttTopicRoot),"%s",val);
        changed=true;
        }
      }
    if (request->hasParam("user", true))
      {
      const char* val = request->getParam("user", true)->value().c_str();
      if (strcmp(val, settings.mqttUsername) != 0)  
        {
        snprintf(settings.mqttUsername,sizeof(settings.mqttUsername),"%s",val);
        changed=true;
        }
      }
    if (request->hasParam("pass", true))
      {
      const char* val = request->getParam("pass", true)->value().c_str();
      if (strcmp(val, settings.mqttPassword) != 0)  
        {
        snprintf(settings.mqttPassword,sizeof(settings.mqttPassword),"%s",val);
        changed=true;
        }
      }
    if (request->hasParam("mdnsname", true))
      {
      const char* val = request->getParam("mdnsname", true)->value().c_str();
      if (strcmp(val, settings.mdnsName) != 0)  
        {
        snprintf(settings.mdnsName,sizeof(settings.mdnsName),"%s",val);
        changed=true;
        }
      }
    if (request->hasParam("debug", true)) //Maybe not - debug doesn't show up if not checked.
      {
      String val = request->getParam("debug", true)->value();
      bool bval=(strcmp(val.c_str(), "1") == 0);
      if (bval != settings.debug)  
        {
        settings.debug=bval;
        changed=true;
        }
      }
    else if (settings.debug) //special case - checkbox not sent if not checked. Always true or false though.
      {
      settings.debug=false;
      changed=true;
      }

    if (request->hasParam("reportinterval", true))
      {
      ulong val = (ulong)atol(request->getParam("reportinterval", true)->value().c_str());
      if (val != settings.reportInterval)  
        {
        settings.reportInterval=val;
        changed=true;
        }
      }

    
    //This is where we set the ports to use. Have to clear all of them out first
    //because if the box is not checked, it won't show up in the request.
    changed=true; //I guess this is always going to be the case
    for (int i=0;i<PORT_COUNT;i++)
      settings.ports[i].isActive=false;

    //now process all of the ports in the request
    // ------Port 0
    port* thisPort=&settings.ports[0];
    if (request->hasParam("useGpio0", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=0;
      if (request->hasParam("gpio0highval",true))
        {
        const char* val = request->getParam("gpio0highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio0lowval",true))
        {
        const char* val = request->getParam("gpio0lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup0",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
      
    // ------Port 1
    thisPort=&settings.ports[1];
    if (request->hasParam("useGpio1", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=1;
      if (request->hasParam("gpio1highval",true))
        {
        const char* val = request->getParam("gpio1highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio1lowval",true))
        {
        const char* val = request->getParam("gpio1lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup1",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
      
    // ------Port 2
    thisPort=&settings.ports[2];
    if (request->hasParam("useGpio2", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=2;
      if (request->hasParam("gpio2highval",true))
        {
        const char* val = request->getParam("gpio2highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio2lowval",true))
        {
        const char* val = request->getParam("gpio2lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup2",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
       
    // ------Port 3
    thisPort=&settings.ports[3];
    if (request->hasParam("useGpio3", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=3;
      if (request->hasParam("gpio3highval",true))
        {
        const char* val = request->getParam("gpio3highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio3lowval",true))
        {
        const char* val = request->getParam("gpio3lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup3",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
       
    // ------Port 4
    thisPort=&settings.ports[4];
    if (request->hasParam("useGpio4", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=4;
      if (request->hasParam("gpio4highval",true))
        {
        const char* val = request->getParam("gpio4highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio4lowval",true))
        {
        const char* val = request->getParam("gpio4lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup4",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
       
    // ------Port 5
    thisPort=&settings.ports[5];
    if (request->hasParam("useGpio5", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=5;
      if (request->hasParam("gpio5highval",true))
        {
        const char* val = request->getParam("gpio5highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio5lowval",true))
        {
        const char* val = request->getParam("gpio5lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup5",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
       
    // ------Port 12
    thisPort=&settings.ports[6];
    if (request->hasParam("useGpio12", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=12;
      if (request->hasParam("gpio12highval",true))
        {
        const char* val = request->getParam("gpio12highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio12lowval",true))
        {
        const char* val = request->getParam("gpio12lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup12",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
       
    // ------Port 13
    thisPort=&settings.ports[7];
    if (request->hasParam("useGpio13", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=13;
      if (request->hasParam("gpio13highval",true))
        {
        const char* val = request->getParam("gpio13highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio13lowval",true))
        {
        const char* val = request->getParam("gpio13lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup13",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
       
    // ------Port 14
    thisPort=&settings.ports[8];
    if (request->hasParam("useGpio14", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=14;
      if (request->hasParam("gpio14highval",true))
        {
        const char* val = request->getParam("gpio14highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio14lowval",true))
        {
        const char* val = request->getParam("gpio14lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup14",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
       
    // ------Port 15
    thisPort=&settings.ports[9];
    if (request->hasParam("useGpio15", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=15;
      if (request->hasParam("gpio15highval",true))
        {
        const char* val = request->getParam("gpio15highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio15lowval",true))
        {
        const char* val = request->getParam("gpio15lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup15",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
        
    // ------Port 16
    thisPort=&settings.ports[10];
    if (request->hasParam("useGpio16", true))
      {
      thisPort->isActive=true;
      thisPort->gpioNumber=16;
      if (request->hasParam("gpio16highval",true))
        {
        const char* val = request->getParam("gpio16highval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->highMessage,sizeof(thisPort->highMessage),"%s",val);
        else
          strcpy(thisPort->highMessage,MQTT_DEFAULT_TOPIC_SUFFIX_HIGH);
        }
      if (request->hasParam("gpio16lowval",true))
        {
        const char* val = request->getParam("gpio16lowval", true)->value().c_str();
        if (strlen(val)>0)
          snprintf(thisPort->lowMessage,sizeof(thisPort->lowMessage),"%s",val);
        else
          strcpy(thisPort->lowMessage,MQTT_DEFAULT_TOPIC_SUFFIX_LOW);
        }
      if (request->hasParam("usePullup16",true))
        {
        thisPort->usePullup=true;
        }
      }
    else
      {
      thisPort->highMessage[0]='\0';
      thisPort->lowMessage[0]='\0';
      thisPort->usePullup=false;
      }
    
    if (changed)
      {
      saveSettings();
      webMessage="Settings saved";
      }

    keepAwake=millis()+STAY_AWAKE_INCREMENT; //stay awake a little longer for more web changes
    request->redirect("/");  // Go back to main page
    });
  
  server.onNotFound(notFound);

  server.begin();  
  }

void loop()
  {
  if (settings.debug)
    {
    static unsigned long lastLoopTime = 0;
    unsigned long now = millis();
    unsigned long duration = now - lastLoopTime;
    lastLoopTime = now;
    if (duration > 5)
      Serial.printf("loop() gap: %lu ms\n", duration);
    }
  
  
  yield();
  if (settingsAreValid)
    {      
    if (WiFi.status() != WL_CONNECTED && !apModeActive)
      {
      yield();
      connectToWiFi();
      }
    if (!mqttClient.connected() && WiFi.status() == WL_CONNECTED)
      {
      yield();
      reconnectToBroker();
      yield();
      mqttClient.loop();
      }  
    else 
      mqttClient.loop();
    }
  
  yield();
  MDNS.update();
  yield();
    
  checkForCommand();
  yield(); //Very important! Web page won't load without these yields.

  static unsigned long nextReport=0; //first report right away

  if (settingsAreValid && millis() >= nextReport && !apModeActive)
    {
    nextReport=millis()+STAY_AWAKE_MINIMUM_MS;
    report();
    yield();
    }

  // Give someone a chance to change a setting before sleeping
  if (settingsAreValid && 
      settings.reportInterval>0 && 
      millis() > STAY_AWAKE_MINIMUM_MS &&
      millis()>keepAwake)
    {
    Serial.print("Sleeping for ");
    Serial.print(settings.reportInterval);
    Serial.println(" seconds");
    Serial.flush();
    ESP.deepSleep(settings.reportInterval*1000000, WAKE_RF_DEFAULT); 
    }
  }

// Stack overflow hook to stop and let me know there's a crash.
extern "C" void vApplicationStackOverflowHook(void* xTask, char *pcTaskName)
  {
  Serial.println("***********************************************");
  Serial.print("Stack overflow in task: ");
  Serial.println(pcTaskName);
  Serial.println("***********************************************");
  while (true);  // Halt so you can see the error
  }

/*
  SerialEvent occurs whenever a new data comes in the hardware serial RX. This
  routine is run between each time loop() runs, so using delay inside loop can
  delay response. Multiple bytes of data may be available.
*/
void incomingSerialData() 
  {
  static bool lastCR = false; 
    {
    char inChar = (char)Serial.read(); // get the new byte
    Serial.print(inChar); //echo it back to the terminal

    // if the incoming character is a newline, set a flag so the main loop can
    // do something about it 
    if (inChar == '\n' || inChar == '\r') 
      {
      if (lastCR)     //some serial ports send both CR and LF, We want to ignore the second one
        lastCR=false;
      else
        {
        lastCR=true;
        commandComplete = true;
        }
      }
    else
      {
      lastCR=false; //in case only one of \r and \n is sent
      // add it to the inputString 
      commandString += inChar;
      }
    }
  }
