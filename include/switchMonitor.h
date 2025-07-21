#define MAX_HARDWARE_FAILURES 20
#define VALID_SETTINGS_FLAG 0xDAB0
#define LED_ON LOW
#define LED_OFF HIGH
#define SSID_SIZE 100
#define PASSWORD_SIZE 50
#define ADDRESS_SIZE 30
#define USERNAME_SIZE 50
#define MQTT_CLIENTID_SIZE 25
#define MQTT_TOPIC_SIZE 150
#define MQTT_TOPIC_DISTANCE "distance"
#define MQTT_TOPIC_BATTERY "battery"
#define MQTT_TOPIC_ANALOG "analog"
#define MQTT_TOPIC_RSSI "rssi"
#define MQTT_TOPIC_SNR "snr"
#define MQTT_TOPIC_FREE_HEAP "freeHeap"
#define MQTT_TOPIC_HEAP_FRAGMENTATION "heapFrag"
#define MQTT_TOPIC_MAX_FREE_BLOCK_SIZE "maxBlockSize"
#define MQTT_CLIENT_ID_ROOT "GenericMonitor"
#define MQTT_TOPIC_COMMAND_REQUEST "command"
#define MQTT_PAYLOAD_SETTINGS_COMMAND "settings" //show all user accessable settings
#define MQTT_PAYLOAD_RESET_PULSE_COMMAND "resetPulseCounter" //reset the pulse counter to zero
#define MQTT_PAYLOAD_REBOOT_COMMAND "reboot" //reboot the controller
#define MQTT_PAYLOAD_VERSION_COMMAND "version" //show the version number
#define MQTT_PAYLOAD_STATUS_COMMAND "status" //show the most recent flow values
#define MQTT_PAYLOAD_ARMED_STATUS "armed" //device has not triggered
#define MQTT_PAYLOAD_TRIPPED_STATUS "tripped" //device has triggered
#define JSON_STATUS_SIZE SSID_SIZE+PASSWORD_SIZE+USERNAME_SIZE+MQTT_TOPIC_SIZE+150 //+150 for associated field names, etc
#define PUBLISH_DELAY 400 //milliseconds to wait after publishing to MQTT to allow transaction to finish
#define WIFI_TIMEOUT_SECONDS 30 // give up on wifi after this long
#define FULL_BATTERY_COUNT 3686 //raw A0 count with a freshly charged 18650 lithium battery 
#define FULL_BATTERY_VOLTS 412 //4.12 volts for a fully charged 18650 lithium battery 
#define ONE_HOUR 3600000 //milliseconds
#define DEFAULT_CHECK_INTERVAL 60 //seconds to sleep between checks
#define SWITCH_PIN 14 //switch to monitor is on pin GPIO14 (D5) by default
#define STANDALONE_SSID "monitor" //SSID to use when in soft AP mode
#define STAY_AWAKE_MINIMUM_MS 30000 //When woken, it will wait at least this long before going back to sleep. Includes startup time.

void showSettings();
String getConfigCommand();
bool processCommand(String cmd);
void checkForCommand();
float read_pressure();
bool report();
boolean publish(char* topic, const char* reading, boolean retain);
void incomingMqttHandler(char* reqTopic, byte* payload, unsigned int length) ;
void setup_wifi();
void connectToWiFi();
void reconnectToBroker();
void showSub(char* topic, bool subgood);
void initializeSettings();
boolean saveSettings();
void setup();
void loop();
void incomingSerialData();
char* generateMqttClientId(char* mqttId);

//MQTT status for reference only
// MQTT_CONNECTION_TIMEOUT     -4
// MQTT_CONNECTION_LOST        -3
// MQTT_CONNECT_FAILED         -2
// MQTT_DISCONNECTED           -1
// MQTT_CONNECTED               0
// MQTT_CONNECT_BAD_PROTOCOL    1
// MQTT_CONNECT_BAD_CLIENT_ID   2
// MQTT_CONNECT_UNAVAILABLE     3
// MQTT_CONNECT_BAD_CREDENTIALS 4
// MQTT_CONNECT_UNAUTHORIZED    5