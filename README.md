# gps_iot_thingy

Copy from gps_iot but then converted, to run on the thingy91. This also connects a person MQTT (mosquito) instance running on a raspberry pi not AWS.

# What
This listens to the uart1 link and transfers it to MQTT, it also adds GPS data to the MQTT server.  The data coming on uart1 is bike power data as received from the 52840 (ant+).