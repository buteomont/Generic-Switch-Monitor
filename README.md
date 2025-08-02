# A General Purpose Port Monitor
This is a general purpose program to monitor up to 10 ports of a processor and report their status via MQTT.

This program will put the processor in deep-sleep mode and will stay there until one of two things occur:
 1. The processor is reset by momentarily pulling the reset pin low.
 2. The number of seconds in the configuration parameter ***reportInterval*** has passed since the last wakeup.
When it wakes, it will connect to the specified router, subscribe to the command
topic (&lt;topicRoot&gt;/command) on the specified broker, and publish a set of values.

If the configuration has not yet been set up, or if the processor can't establish a WiFi connection to the configured router, the program will open its own WiFi in AP mode. This will allow you to connect directly to the processor and configure it via the web interface.
  
Configuration can be done via serial connection<sup>1</sup>, web page, or MQTT topic. 

## Configuration Options

### Serial Connection
To configure the program via the serial port, simply connect the processor to a serial (USB) port on your computer, start a terminal program such as Putty<sup>2</sup> or Minicom, and set the terminal parameters to 115200,8,N,1. You should then be able to type configuration commands directly in the terminal. Commands take the form of *name=value* and spaces are not allowed. Available commands are:

 - broker=MQTT broker name or address&gt;
 - port=&lt;port number&gt;   (defaults to 1883)
 - topicroot=&lt;topic root&gt; (ex. basement/mousetrap/). your port suffix(s) will be added when reporting.
 - user=&lt;mqtt user&gt;
 - pass=&lt;mqtt password&gt;
 - ssid=&lt;wifi ssid&gt;
 - wifipass=&lt;wifi password&gt;
 - reportinterval=&lt;seconds&gt; (How long to sleep between status reports)
 - address=&lt;Static IP address if so desired&gt;
 - netmask=&lt;Network mask to be used with static IP&gt; (255.255.255.0)
 - mdnsname=<Name to use for MDNS> (ex. *mousetrap* for http://mousetrap.local)
 - debug=&lt;1 | true | 0 | false&gt; (Prints debug messages to the serial port)
 - portadd=gpioPort,highTopicSuffix,lowTopicSuffix
 - portremove=gpioPort

Pressing ENTER without any parameters will list the current settings.

### MQTT commands
Once connected to an MQTT broker, configuration can be done similarly via the 
&lt;topicroot&gt;/command topic. Because this program sleeps most of the time, you will need
to send a &lt;topicroot&gt;/command with the RETAIN bit set and a message of "reportinterval=0"
to keep it awake while you make changes. Reset *reportinterval* to the desired value when you are finished
and don't forget to remove the retained MQTT message.

To get a list of the current settings, subscribe to *&lt;topicroot&gt;/#* on the broker, and then publish a message to *&lt;topicroot&gt;/command* with **settings** as the message payload.

### REST commands (web page)
If you have the MDNS name set, and the device is connected to WiFi, you can open a browser to *http://&lt;mdnsname&gt;.local* to get a handy configuration page.  

If you can't or don't want to use the MDNS name but the device is connected to WiFi, you can browse to the IP address of the device. Finding the value of that address is left as an exercise for the reader. 

If the device is not connected to the WiFi, it will open an access point (the ssid is ***monitor***). Connect your computer to the access point and browse to http://192.168.4.1 for the configuration page.

![This should be a helpful picture of the web page](resources/Settings%20Page%20Image.png)


## Waking On Event
As mentioned, the device will awaken periodically at intervals specified by *reportInterval*, and send a report.  It can also be awakened by an external event, such as a switch closure. In this case, the switch must be connected to the RESET pin of the processor, pulling it low for a minimum of 100 microseconds.  When released, the processor will awaken and report the values immediately.

If you want the switch to not only wake the processor, but be part of the report, you will need either a double-pole switch or a circuit to convert the switch closure to a short pulse to ground.  I've found that the circuit below works well in the second case:

| ![Schematic of sample usage](resources/Wake%20on%20event.png) |
|:--:|
| *This circuit was used on a livetrap. The microswitch was held open by the open door, and would close when the door slammed shut.* |

This circuit allows one switch to send a short LOW pulse to the reset pin, but allow the switch's state to be read by the processor.  This example uses a tiny ESP8266-01s<sup>3</sup> that can run for weeks on 2 AAA batteries.

NOTES:
 1. Most ports on ESP devices are multi-purpose, so you have to be very careful about which one you choose for your use case. Some ***must*** be in a certain state (high or low) when the device wakes up, or it simply will not boot. This is especially a problem on the ESP8266-01s, as only ports 0 and 2 are brought to the interface and they both have this limitation. The TX and RX lines (GPIO 1 and GPIO 3, respectively) can be used for general purpose I/O, but the serial port initialization code must be modified to prevent conflicts (I am working on a way to automate this) and you will lose the ability to configure via the serial port.
 2. In Putty you may have to use ctl-M ctl-J instead of the ENTER key when entering configuration parameters. I don't know why.
 3. If you're using an ESP8266-01s, don't forget you have to add a bodge wire from GPIO16 to the reset pin. It takes a good eye (or magnifier) and a steady hand, but it is possible. There are multiple articles on the web describing how to do this.

