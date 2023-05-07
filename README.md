# Sprinkler controller which runs on Arduino UNO
Sprinkler controller which runs on Adruino UNO. Uses an external relay board to switch valves on and off. Also uses an ethernet shield for network connection.
Exposes a simple HTTP 1.0 REST interface (output in JSON) to control and get current status.

## Usage
* Rain sensor hooked up on Pin 14

Valves controlled on pins (via 8 port external relay board to switch low voltage AC on valves):
* Valve 1: Pin 2
* Valve 2: Pin 3
* Valve 3: Pin 5
* Valve 4: Pin 6
* Valve 5: Pin 7
* Valve 6: Pin 8
* Valve 7: Pin 9
* Valve 8: Pin 16

### REST Commands
* <none>, status[/opt] - give the status of the global options
* status/prog/<id> - give the status of a specific program
* status/all - give the status of all programs
* duration/<id>/<min> - specify program duration in minutes
* on/<id> - turn on program
* off[/<id>] - turn off everything, or turn off specific program
* day/<id>/(+-)(sun,mon,tue,wed,thu,fri,sat) - add or remove day of week from program
* valve/<id>/<valve> - set program to valve
* time/<slot>/<id>/MM:HH - set start time for program (24 hr time)
* erase/<id> - erase a program
* erase - erase global options (and programs)
* enable/<id>[/on|off] - toggle/disable/enable a program
* master[/on|off] - toggle/disable/enable master switch
* name/<id>/<name> - set the name for a program (30 chars max)
* valvedelay/<sec> - delay between valve transitions in seconds
* raindelay/<min> - set rain delay in minutes
* timezone/<int> - timezone (-8 for PST)
* scale/<scale> - set the watering scale in integers
* dst[/on|off] - toggle/enable/disable daylight savings


## Known limitations
Limited to controlling 8 valves, with 16 programs across any of the valves with 1 program running at a time.

## License
Copyright 2015 Russell Goring. Licensed under GPLv3. See LICENSE for full licensing information.
