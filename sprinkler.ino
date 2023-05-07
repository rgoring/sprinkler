
#include <Time.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <avr/wdt.h>

//start sprinkler info
#define NUM_PROGRAMS 16
#define MAX_RUNNING 1
#define RAIN_SENSOR_PIN 14
#define RUNNING_ERR 99

#define NUM_TIMES 3
#define HR_IDX 0
#define MIN_IDX 1

#define PROGREV 14

//uncomment to enable debug prints
//also enables SERIAL_PRINT
//#define DEBUG_PRINT

//uncomment to enable printing
//#define SERIAL_PRINT

#define PROG_OFF 0
#define PROG_ON 1
#define PROG_PENDING 2

struct program_t {
  char name[30];
  boolean enabled;
  unsigned char times[NUM_TIMES][2]; //hour,minute
  unsigned char duration;
  unsigned char dow;
  unsigned char valve;
  unsigned long last;
};

struct status_t {
  unsigned char state;
  unsigned long finish;
};

struct options_t {
  unsigned char progrev;
  boolean master; //turn on or off the programs
  unsigned long rain_delay; //delay to keep off due to rain in minutes
  unsigned int scale; //scale of program duration
  boolean dst; //daylight savings on or off
  int timezone; //timezone
  unsigned long rain_time; //last rain
  int numprograms; //number of programs used
  unsigned char valve_delay; //seconds between valve activations
  uint8_t ntpipaddr[4]; //ntp ip address (4 octets)
};

//store the programs at an offset in eeprom
//this offset must be larger than options_t size in bytes
#define PROGRAM_MEM_OFFSET sizeof(struct options_t)

struct options_t options;
struct status_t progstat[NUM_PROGRAMS];
unsigned long start_time;
unsigned long last_run;
int running = 0;

const int valve_pins[] = {2, 3, 5, 6, 7, 8, 9, 16};

boolean rain = false;
//end sprinkler info

//start web server info
EthernetServer server(80);
//end web server info

//start ntp client info (ntp ip address)
IPAddress ntp_ip(192, 168, 1, 1);

// local port to listen for UDP packets
#define NTP_PORT 8888
#define NTP_PACKET_SIZE 48
byte ntp_buffer[ NTP_PACKET_SIZE];
EthernetUDP ntp_udp;
//end ntp client info

//start program string/print functions
//buffer size for web hdr processing and program string retrieval
#define BUFSIZE 80

#ifdef DEBUG_PRINT
#define SERIAL_PRINT
#endif

// Retrieve program string for local use
char p_buffer[BUFSIZE];
#define cstr(x) get_p_string(PSTR(x))
char *get_p_string(const char* str)
{
  strcpy_P(p_buffer, (char*)str);
  return p_buffer;
}

// SERIAL PRINT for program strings
#ifdef SERIAL_PRINT
#define SerialPrint(x) SerialPrint_P(PSTR(x), false)
#define SerialPrintln(x) SerialPrint_P(PSTR(x), true)
void SerialPrint_P(PGM_P str, boolean newline)
{
  for (uint8_t c; (c = pgm_read_byte(str)); str++) {
    Serial.write(c);
  }
  if (newline) {
    Serial.println();
  }
}
#endif

// ETHERNET CLIENT PRINT for program strings
#define ClientPrint(c, s) ClientPrint_P(c, PSTR(s), false)
#define ClientPrintln(c, s) ClientPrint_P(c, PSTR(s), true)
void ClientPrint_P(Client &client, PGM_P str , boolean newline)
{
  for (uint8_t c; (c = pgm_read_byte(str)); str++) {
    client.write(c);
  }
  if (newline) {
    client.println();
  }
}

/*
int freeRam()
{
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}
*/
//end program print functions

void reset_options()
{
  memset(&options, 0, sizeof(struct options_t));
  options.progrev = PROGREV;
  save_options();
}

void save_options()
{
  eeprom_write_block(&options, (void *)0, sizeof(struct options_t));
}

void load_options()
{
  eeprom_read_block(&options, (void *)0, sizeof(struct options_t));

  if (options.progrev != PROGREV) {
    #ifdef SERIAL_PRINT
    Serial.print(F("EEPROM does not match program version\n"));
    #endif
    reset_options();
    //erase program eeprom?
  }

  //update the ntp ip address from the options
  ntp_ip[0] = options.ntpipaddr[0];
  ntp_ip[1] = options.ntpipaddr[1];
  ntp_ip[2] = options.ntpipaddr[2];
  ntp_ip[3] = options.ntpipaddr[3];
}

void save_program(int idx, struct program_t *prog)
{
  eeprom_write_block(prog, (void *)(PROGRAM_MEM_OFFSET + (sizeof(struct program_t) * idx)), sizeof(struct program_t));
  wdt_reset();
  #ifdef SERIAL_PRINT
  SerialPrintln("program saved");
  #endif
}

void load_program(int idx, struct program_t *prog)
{
  eeprom_read_block(prog, (void *)(PROGRAM_MEM_OFFSET + (sizeof(struct program_t) * idx)), sizeof(struct program_t));
  wdt_reset();
  #ifdef SERIAL_PRINT
  Serial.write(F("Program "));
  Serial.write(idx);
  Serial.write(F("loaded\n"));
  #endif
}

//make sure all programs start in an "OFF" state
void init_programs()
{
  struct program_t pgm;

  for (int i = 0; i < NUM_PROGRAMS; i++) {
    progstat[i].state = PROG_OFF;
    progstat[i].finish = 0;
  }
}

void queue_program(int idx)
{
  //only turn on a program if it's off
  if (progstat[idx].state == PROG_OFF) {
    progstat[idx].state = PROG_PENDING;
  }
}

void web_hdr_ok(EthernetClient &client)
{
  #ifdef DEBUG_PRINT
  SerialPrintln("response OK");
  #endif
  ClientPrintln(client, "HTTP/1.1 200 OK");
  ClientPrintln(client, "Content-Type: text/plain");
  client.println();
}

void web_hdr_404(EthernetClient &client)
{
  #ifdef DEBUG_PRINT
  SerialPrintln("response 404");
  #endif
  ClientPrintln(client, "HTTP/1.1 404 Not Found");
  ClientPrintln(client, "Content-Type: text/plain");
  client.println();
}

void print_timestamp(unsigned long ptime, boolean pdate, EthernetClient &client)
{
  client.print(hour(ptime));
  ClientPrint(client, ":");
  if (minute(ptime) < 10) {
    ClientPrint(client, "0");
  }
  client.print(minute(ptime));
  ClientPrint(client, ":");
  if (second(ptime) < 10) {
    ClientPrint(client, "0");
  }
  client.print(second(ptime));
  if (pdate) {
    ClientPrint(client, " ");
    client.print(month(ptime));
    ClientPrint(client, "/");
    client.print(day(ptime));
    ClientPrint(client, "/");
    client.print(year(ptime));
  }
}

void print_program(EthernetClient &client, int idx)
{
  int i;
  unsigned long curtime = now();
  char *days = cstr(" SMTWTFS");
  struct program_t pgm;

  load_program(idx, &pgm);

  wdt_reset();

  ClientPrint(client, " {\"pgm\":");
  client.print(idx+1);
  ClientPrint(client, ", \"name\":\"");
  client.print(pgm.name);
  ClientPrint(client, "\", \"enabled\":");
  if (pgm.enabled) {
      ClientPrint(client, "true");
  } else {
      ClientPrint(client, "false");
  }
  ClientPrint(client, ", \"status\":\"");
  if (progstat[idx].state == PROG_OFF) {
    ClientPrint(client, "OFF");
  } else if (progstat[idx].state == PROG_ON) {
    ClientPrint(client, "ON");
  } else if (progstat[idx].state == PROG_PENDING) {
    ClientPrint(client, "PEND");
  }
  ClientPrint(client, "\", \"times\":[");
  for (i = 0; i < NUM_TIMES; i++) {
    if (i > 0) {
      ClientPrint(client, ",");
    }
    ClientPrint(client, "{\"start\":\"");
    if (pgm.times[i][HR_IDX] < 10) {
      ClientPrint(client, "0");
    }
    client.print(pgm.times[i][HR_IDX]);
    ClientPrint(client, ":");
    if (pgm.times[i][MIN_IDX] < 10) {
      ClientPrint(client, "0");
    }
    client.print(pgm.times[i][MIN_IDX]);
    ClientPrint(client, "\"}");
  }
  ClientPrint(client, "], \"duration\":");
  client.print(pgm.duration);
  ClientPrint(client, ", \"day\":\"");
  for (i = 1; i < 8; i++) {
    if (bitRead(pgm.dow, i)) {
      client.print(days[i]);
    } else {
      client.print(days[0]);
    }
  }
  ClientPrint(client, "\", \"valve\":");
  client.print(pgm.valve);
  ClientPrint(client, ", \"last\":\"");
  print_timestamp(pgm.last, true, client);
  ClientPrint(client, "\", \"remaining\":\"");
  if (progstat[idx].finish > 0) {
    print_timestamp(progstat[idx].finish - curtime, false, client);
  } else {
    ClientPrint(client, "00:00:00");
  }
  ClientPrint(client, "\"}");

  wdt_reset();
}

void print_programs(EthernetClient &client)
{
	int i;

    ClientPrintln(client, "\"status\": [");
    for (i = 0; i < options.numprograms; i++) {
      if (i > 0) {
        ClientPrintln(client, ",");
      }
      print_program(client, i);
    }
    ClientPrintln(client, "\n]");
}

void print_options(EthernetClient &client)
{
  ClientPrint(client, "\"time\":\"");
  print_timestamp(now(), true, client);
  ClientPrintln(client, "\",");
  ClientPrint(client, "\"tz\":");
  client.print(options.timezone);
  ClientPrintln(client, ",");
  ClientPrint(client, "\"rev\":");
  client.print(options.progrev);
  ClientPrintln(client, ",");
  ClientPrint(client, "\"dst\":");
  if (options.dst) {
    ClientPrint(client, "true");
  } else {
    ClientPrint(client, "false");
  }
  ClientPrintln(client, ",");
  ClientPrint(client, "\"ntpip\":\"");
  client.print(options.ntpipaddr[0]);
  ClientPrint(client, ".");
  client.print(options.ntpipaddr[1]);
  ClientPrint(client, ".");
  client.print(options.ntpipaddr[2]);
  ClientPrint(client, ".");
  client.print(options.ntpipaddr[3]);
  ClientPrintln(client, "\",");
  ClientPrint(client, "\"master\":");
  if (options.master) {
    ClientPrint(client, "true");
  } else {
    ClientPrint(client, "false");
  }
  ClientPrintln(client, ",");
  ClientPrint(client, "\"rain\":");
  if (rain) {
    ClientPrint(client, "true");
  } else {
    ClientPrint(client, "false");
  }
  ClientPrintln(client, ",");
  ClientPrint(client, "\"uptime\":\"");
  print_timestamp(start_time, true, client);
  ClientPrintln(client, "\",");
  ClientPrint(client, "\"raintime\":\"");
  print_timestamp(options.rain_time, true, client);
  ClientPrintln(client, "\",");
  ClientPrint(client, "\"raindelay\":");
  client.print(options.rain_delay);
  ClientPrintln(client, ",");
  ClientPrint(client, "\"scale\":");
  client.print(options.scale);
  ClientPrintln(client, ",");
  ClientPrint(client, "\"valvedelay\":");
  client.print(options.valve_delay);
  ClientPrintln(client, ",");
  ClientPrint(client, "\"running\":");
  client.print(running);
  ClientPrintln(client, ",");
  ClientPrint(client, "\"programs\":");
  client.print(options.numprograms);
  wdt_reset();
}

int cmd_status(EthernetClient &client, char *arg1, char *arg2)
{
  int i;

  wdt_reset();

  //send header here so we can print out the status
  web_hdr_ok(client);

  //opening json statement
  ClientPrint(client, "{");

  //print the header
  if (arg1 == NULL || strcasecmp(arg1, cstr("opt")) == 0) {
    print_options(client);
  } else if (strcasecmp(arg1, cstr("all")) == 0) {
    //print all the programs
	print_programs(client);
  } else if (strcasecmp(arg1, cstr("prog")) == 0) {
    i = atoi(arg2);
    ClientPrintln(client, "\"status\": [");
    if (i < 1 || i > options.numprograms) {
      ClientPrintln(client, "unknown_program:0");
    } else {
      print_program(client, i-1);
    }
    ClientPrintln(client, "\n]");
  } else if (strcasecmp(arg1, cstr("save")) == 0) {
	ClientPrintln(client, "\"options\": {");
	print_options(client);
	ClientPrintln(client, "},");
	print_programs(client);
  }

  //closing json statement
  ClientPrintln(client, "}");

  wdt_reset();

  return 0;
}

boolean get_true_false(char *str)
{
  if (strcasecmp(str, cstr("true")) == 0) {
    return true;
  }
  return false;
}

//set the run duration (minutes)
int cmd_duration(EthernetClient &client, char *arg1, char *arg2)
{
  int i;
  struct program_t pgm;

  if (arg1 == NULL || arg2 == NULL) {
    return 404;
  }

  i = atoi(arg1);

  if (i < 1 || i > options.numprograms) {
    return 404;
  }

  load_program(i - 1, &pgm);

  pgm.duration = atoi(arg2);

  save_program(i - 1, &pgm);

  return 200;
}

//turn a program on
int cmd_on(EthernetClient &client, char *arg1, char *arg2)
{
  int i;
  struct program_t pgm;

  if (arg1 == NULL) {
    return 404;
  }

  i = atoi(arg1);

  if (i < 1 || i > options.numprograms) {
    return 404;
  }

  queue_program(i-1);

  return 200;
}

//turn program(s) off
int cmd_off(EthernetClient &client, char *arg1, char *arg2)
{
  int i;

  if (arg1 == NULL) {
    for (i = 0; i < options.numprograms; i++) {
      if (progstat[i].state != PROG_OFF) {
        progstat[i].finish = 0;
        if (progstat[i].state == PROG_PENDING) {
          progstat[i].state = PROG_OFF;
        }
      }
    }
  } else {
    i = atoi(arg1);
    if (i < 1 || i > options.numprograms) {
      return 404;
    }
    if (progstat[i-1].state != PROG_OFF) {
      progstat[i-1].finish = 0;
      if (progstat[i-1].state == PROG_PENDING) {
        progstat[i-1].state = PROG_OFF;
      }
    }
  }

  return 200;
}

//set the day of week to run a program on (+/-dow)
int cmd_day(EthernetClient &client, char *arg1, char *arg2)
{
  bool set = false;
  int i;
  int dayidx;
  char *daystr;
  struct program_t pgm;

  if (arg1 == NULL || arg2 == NULL) {
    return 404;
  }

  i = atoi(arg1);
  if (i < 1 || i > options.numprograms) {
    return 404;
  }

  if (arg2[0] == '-') {
    set = false;
  } else if (arg2[0] == '+') {
    set = true;
  }
  daystr = arg2 + 1;

  if (strcasecmp(daystr, cstr("sun")) == 0) {
    dayidx = 1;
  } else if (strcasecmp(daystr, cstr("mon")) == 0) {
    dayidx = 2;
  } else if (strcasecmp(daystr, cstr("tue")) == 0) {
    dayidx = 3;
  } else if (strcasecmp(daystr, cstr("wed")) == 0) {
    dayidx = 4;
  } else if (strcasecmp(daystr, cstr("thu")) == 0) {
    dayidx = 5;
  } else if (strcasecmp(daystr, cstr("fri")) == 0) {
    dayidx = 6;
  } else if (strcasecmp(daystr, cstr("sat")) == 0) {
    dayidx = 7;
  }

  load_program(i-1, &pgm);

  if (set) {
    bitWrite(pgm.dow, dayidx, 1);
  } else {
    bitWrite(pgm.dow, dayidx, 0);
  }

  save_program(i - 1, &pgm);

  return 200;
}

//set the valve for a program
int cmd_valve(EthernetClient &client, char *arg1, char *arg2)
{
  int i;
  int valve;
  struct program_t pgm;

  if (arg1 == NULL || arg2 == NULL) {
    return 404;
  }

  i = atoi(arg1);
  if (i < 1 || i > options.numprograms) {
    return 404;
  }

  valve = atoi(arg2);
  if (valve < 1 || valve > (sizeof(valve_pins)/sizeof(int))) {
    return 404;
  }

  load_program(i-1, &pgm);

  if (progstat[i-1].state == PROG_ON) {
    turn_valve_off(pgm.valve);
    progstat[i-1].state = PROG_OFF;
    progstat[i-1].finish = 0;
  }

  pgm.valve = valve;

  save_program(i - 1, &pgm);

  return 200;
}

//set the start time (military time->HH:MM)
int cmd_time(EthernetClient &client, char *arg1, char *arg2, char *arg3)
{
  char *divider;
  int i, tm;
  struct program_t pgm;

  if (arg1 == NULL || arg2 == NULL) {
    return 404;
  }

  i = atoi(arg1);
  if (i < 1 || i > options.numprograms) {
    return 404;
  }

  tm = atoi(arg2);
  if (tm < 1 || tm > NUM_TIMES) {
    return 404;
  }

  divider = strchr(arg3, ':');
  if (divider == NULL) {
    return 404;
  }

  load_program(i-1, &pgm);

  divider[0] = '\0';
  pgm.times[tm-1][HR_IDX] = atoi(arg3);
  pgm.times[tm-1][MIN_IDX] = atoi(divider+1);

  save_program(i - 1, &pgm);

  return 200;
}

//erase a program
int cmd_erase(EthernetClient &client, char *arg1, char *arg2)
{
  int i;

  if (arg1 == NULL) {
	return 404;
  } else if (strcasecmp(arg1, cstr("opt")) == 0) {
    //erase the options
    reset_options();
  } else {
    //erase a program
    i = atoi(arg1);
    if (i < 1 || i > options.numprograms) {
      return 404;
    }

    struct program_t pgm;
    load_program(i-1, &pgm);

    if (progstat[i-1].state == PROG_ON) {
      turn_valve_off(pgm.valve);
    }

    //shift the remaining programs down
    for (int x = (i-1); x < options.numprograms; x++) {
      struct program_t next;
      if ((x+1) < options.numprograms) {
        progstat[x].state = progstat[x+1].state;
        progstat[x].finish = progstat[x+1].finish;
        load_program(x+1, &next);
        save_program(x, &next);
      }
    }

    options.numprograms = options.numprograms - 1;
    save_options();
  }

  return 200;
}

//disable a program
int cmd_enable(EthernetClient &client, char *arg1, char *arg2)
{
  int i;
  struct program_t pgm;

  if (arg1 == NULL) {
    return 404;
  }

  i = atoi(arg1);
  if (i < 1 || i > options.numprograms) {
    return 404;
  }

  load_program(i-1, &pgm);

  if (arg2 == NULL) {
    pgm.enabled = !pgm.enabled;
  } else {
    pgm.enabled = get_true_false(arg2);
  }

  save_program(i - 1, &pgm);

  return 200;
}

//toggle the master on/off
int cmd_master(EthernetClient &client, char *arg1, char *arg2)
{
  if (arg1 == NULL) {
    options.master = !options.master;
  } else {
    options.master = get_true_false(arg1);
  }

  save_options();

  return 200;
}

int cmd_dst(EthernetClient &client, char *arg1, char *arg2)
{
  unsigned long newtime;

  if (arg1 == NULL) {
    options.dst = !options.dst;
  } else {
    options.dst = get_true_false(arg1);
  }
  //reset the time with dst
  newtime = update_time();
  if (newtime != 0) {
    setTime(newtime);
  }

  save_options();

  return 200;
}

//set the name for a program
int cmd_name(EthernetClient &client, char *arg1, char *arg2)
{
  int i;
  struct program_t pgm;

  if (arg1 == NULL || arg2 == NULL) {
    return 404;
  }

  i = atoi(arg1);
  if (i < 1 || i > options.numprograms) {
    return 404;
  }

  load_program(i-1, &pgm);

  strncpy(pgm.name, arg2, sizeof(pgm.name)-1);

  save_program(i - 1, &pgm);

  return 200;
}

//set a rain delay (minutes)
int cmd_rain_delay(EthernetClient &client, char *arg1, char *arg2)
{
  if (arg1 == NULL) {
    return 404;
  }
  options.rain_delay = atol(arg1);
  save_options();
  return 200;
}

//set the delay between valve toggles (seconds)
int cmd_valve_delay(EthernetClient &client, char *arg1, char *arg2)
{
  if (arg1 == NULL) {
    return 404;
  }
  options.valve_delay = atol(arg1);
  save_options();
  return 200;
}

//scale program run times globally (0-100)
int cmd_scale(EthernetClient &client, char *arg1, char *arg2)
{
  if (arg1 == NULL) {
    return 404;
  }
  options.scale = atol(arg1);
  save_options();
  return 200;
}

int cmd_add(EthernetClient &client, char *arg1, char *arg2)
{
  if (options.numprograms == NUM_PROGRAMS) {
    return 404;
  }
  struct program_t pgm;
  load_program(options.numprograms, &pgm);
  memset(&pgm, 0, sizeof(struct program_t));
  save_program(options.numprograms, &pgm);
  options.numprograms = options.numprograms + 1;
  save_options();
  return 200;
}

int cmd_tz(EthernetClient &client, char *arg1, char *arg2)
{
  unsigned long newtime;
  options.timezone = atoi(arg1);
  //reset the time with the new timezone
  newtime = update_time();
  if (newtime != 0) {
    setTime(newtime);
  }
  save_options();
  return 200;
}

int cmd_update_ntp(EthernetClient &client, char *arg1)
{
    unsigned long newtime;

    //parse the ip address and set the bytes
    char *oct0 = strtok(arg1, ".");
    char *oct1 = strtok(NULL, ".");
    char *oct2 = strtok(NULL, ".");
    char *oct3 = strtok(NULL, ".");

    options.ntpipaddr[0] = atoi(oct0);
    options.ntpipaddr[1] = atoi(oct1);
    options.ntpipaddr[2] = atoi(oct2);
    options.ntpipaddr[3] = atoi(oct3);

  //reset the time with the new timezone
  newtime = update_time();
  if (newtime != 0) {
    setTime(newtime);
  }
  save_options();
  return 200;
}

void process_command(EthernetClient &client, char *cmdbuf)
{
  int rc = 0;

  wdt_reset();

  //parse arguments
  char *cmd = strtok(cmdbuf, "/");
  char *arg1 = strtok(NULL, "/");
  char *arg2 = strtok(NULL, "/");
  char *arg3 = strtok(NULL, "/");

  #ifdef DEBUG_PRINT
  SerialPrint("cmd = ");
  Serial.println(cmd);
  SerialPrint("arg1 = ");
  Serial.println(arg1);
  SerialPrint("arg2 = ");
  Serial.println(arg2);
  SerialPrint("arg3 = ");
  Serial.println(arg3);
  #endif

  if (cmd == NULL || strcasecmp(cmd, cstr("status")) == 0) {
    rc = cmd_status(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("duration")) == 0) {
    rc = cmd_duration(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("on")) == 0) {
    rc = cmd_on(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("off")) == 0) {
    rc = cmd_off(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("day")) == 0) {
    rc = cmd_day(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("valve")) == 0) {
    rc = cmd_valve(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("time")) == 0) {
    rc = cmd_time(client, arg1, arg2, arg3);
  } else if (strcasecmp(cmd, cstr("erase")) == 0) {
    rc = cmd_erase(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("enable")) == 0) {
    rc = cmd_enable(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("master")) == 0) {
    rc = cmd_master(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("dst")) == 0) {
    rc = cmd_dst(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("name")) == 0) {
    rc = cmd_name(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("raindelay")) == 0) {
    rc = cmd_rain_delay(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("valvedelay")) == 0) {
    rc = cmd_valve_delay(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("scale")) == 0) {
    rc = cmd_scale(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("add")) == 0) {
    rc = cmd_add(client, arg1, arg2);
  } else if (strcasecmp(cmd, cstr("tz")) == 0) {
    rc = cmd_tz(client, arg1, arg2);
} else if (strcasecmp(cmd, cstr("ntpip")) == 0) {
    rc = cmd_update_ntp(client, arg1);
  } else {
    rc = 404;
  }

  wdt_reset();

  if (rc == 200) {
    //send ok
    web_hdr_ok(client);
    ClientPrint(client, "{\"response\":\"ok\"}");
  } else if (rc != 0) {
    web_hdr_ok(client);
    ClientPrint(client, "{\"response\":\"bad\"}");
  }

  wdt_reset();
}
/*
void print_time()
{
  Serial.print("PST time is ");       // UTC is the time at Greenwich Meridian (GMT)
  // digital clock display of the time
  Serial.print(hour());
  printDigits(minute());
  printDigits(second());
  Serial.print(" ");
  Serial.print(day());
  Serial.print(" ");
  Serial.print(month());
  Serial.print(" ");
  Serial.print(year());
  //Serial.println();
  if (timeStatus() == timeNotSet) {
    Serial.println(" Not set");
  } else if (timeStatus() == timeNeedsSync) {
    Serial.println(" Need Sync");
  } else if (timeStatus() == timeSet) {
    Serial.println(" Set");
  } else {
    Serial.println(" Unknown");
  }
}

void printDigits(int digits){
  // utility function for digital clock display: prints preceding colon and leading 0
  Serial.print(":");
  if(digits < 10)
    Serial.print('0');
  Serial.print(digits);
}*/

void toggle_programs()
{
  int i;
  unsigned long curtime;
  struct program_t pgm;

  curtime = now();

  //turn off programs first
  for (i = 0; i < options.numprograms; i++) {
    load_program(i, &pgm);

    //see if the program needs to be turned off
    if (progstat[i].state == PROG_ON) {
      //if we're past the finish time, turn it off
      if (curtime >= progstat[i].finish) {
        progstat[i].state = PROG_OFF;
        progstat[i].finish = 0;
        turn_valve_off(pgm.valve);
      }
    } else if (progstat[i].state == PROG_OFF && options.master && !rain) {
      //see if it needs to be turned on
      if (pgm.enabled
          && bitRead(pgm.dow, weekday(curtime)) == 1
          //prevent rescheduling self if turned off by forcing a 60 sec cooldown for auto programs
          && (curtime - pgm.last) > 60) {
            //check all the times to see if we should start
            for (int t = 0; t < NUM_TIMES; t++) {
              if (pgm.times[t][HR_IDX] == 0 && pgm.times[t][MIN_IDX] == 0) {
                //midnight is not supported -- used to determine if a time is valid or not
                continue;
              }
              if (pgm.times[t][HR_IDX] == hour(curtime)
                  && pgm.times[t][MIN_IDX] == minute(curtime)
                  && pgm.duration > 0) {
                    queue_program(i);
              }
            }
          }
    }
    wdt_reset();
  }

  curtime = now();
  //must have a minimum amount of time between switching valves so we don't
  //make the pipes jerk around
  if (options.valve_delay > 0 && ((last_run + options.valve_delay) > curtime)) {
    return;
  }

  //turn on waiting programs
  for (i = 0; i < options.numprograms && running < MAX_RUNNING; i++) {
    load_program(i, &pgm);
    if (progstat[i].state == PROG_PENDING) {
      progstat[i].finish = (curtime + (unsigned int)((pgm.duration * 60) * ((float)options.scale/100)));
      turn_valve_on(pgm.valve);
      progstat[i].state = PROG_ON;
      pgm.last = curtime;
      save_program(i, &pgm);
    }
    wdt_reset();
  }
}

void turn_valve_on(int valve)
{
  running = running + 1;
  if (valve < 1 || valve > (sizeof(valve_pins) / sizeof(int))) {
    return;
  }
  digitalWrite(valve_pins[valve - 1], LOW);
}

void turn_valve_off(int valve)
{
  running = running - 1;
  if (valve < 1 || valve > (sizeof(valve_pins) / sizeof(int))) {
    return;
  }
  digitalWrite(valve_pins[valve - 1], HIGH);
  last_run = now();
}

// send an NTP request to the time server at the given address
unsigned long sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(ntp_buffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  ntp_buffer[0] = 0b11100011;   // LI, Version, Mode
  ntp_buffer[1] = 0;     // Stratum, or type of clock
  ntp_buffer[2] = 6;     // Polling Interval
  ntp_buffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  ntp_buffer[12]  = 49;
  ntp_buffer[13]  = 0x4E;
  ntp_buffer[14]  = 49;
  ntp_buffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  ntp_udp.beginPacket(address, 123); //NTP requests are to port 123
  ntp_udp.write(ntp_buffer, NTP_PACKET_SIZE);
  ntp_udp.endPacket();
}

unsigned long update_time()
{
  unsigned long ntptime = 0;

  #ifdef SERIAL_PRINT
  SerialPrintln("ntp update");
  #endif

  while (ntp_udp.parsePacket() > 0) ; // discard any previously received packets

  sendNTPpacket(ntp_ip);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = ntp_udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      ntp_udp.read(ntp_buffer, NTP_PACKET_SIZE);
      unsigned long since_1900;
      // convert four bytes starting at location 40 to a long integer
      //this is NTP time (seconds since Jan 1 1900):
      since_1900 =  (unsigned long)ntp_buffer[40] << 24;
      since_1900 |= (unsigned long)ntp_buffer[41] << 16;
      since_1900 |= (unsigned long)ntp_buffer[42] << 8;
      since_1900 |= (unsigned long)ntp_buffer[43];

      //epoch offset = 70 years (Jan 1, 1970)
      //subtract the epoch offset & time zone
      ntptime = since_1900 - 2208988800UL + (options.timezone * SECS_PER_HOUR);
      //accomodate for DST
      if (options.dst) {
        ntptime = ntptime + SECS_PER_HOUR;
      }
    }
  }

  return ntptime;
}

void process_web()
{
  char buf[BUFSIZE];
  char *http;
  char *cmd;
  int idx;
  boolean blank;
  char c;
  EthernetClient client = server.available();

  if (!client) {
    return;
  }

  #ifdef DEBUG_PRINT
  SerialPrintln("new client");
  #endif

  memset(buf, '\0', BUFSIZE);
  blank = true;
  idx = 0;

  while (client.connected()) {
    wdt_reset();
    if (client.available()) {
      c = client.read();

      #ifdef DEBUG_PRINT
      Serial.write(c);
      #endif

      if (c == '\n' && blank) {
        //if we're on a blank line and we hit a newline, stop
        //end of http header
        break;
      } else if (c == '\n') {
        //starting a new line
        blank = true;
      } else if (c != '\r') {
        //any other character
        //only store the first BUFSIZE - 1 characters
        blank = false;
        if (idx < (BUFSIZE - 1)) {
          buf[idx] = c;
          idx++;
        }
      }
    }
  }

  wdt_reset();

  if (strstr(buf, cstr("GET /")) != NULL) {
    http = strstr(buf, cstr(" HTTP/1.1"));
    if (http != NULL) {
      cmd = buf + 5;
      http[0] = '\0';
      process_command(client, cmd);
    } else {
      web_hdr_404(client);
    }
  } else {
    web_hdr_404(client);
  }

  delay(1);
  client.stop();
}

void check_rain()
{
  int sensor;

  sensor = digitalRead(RAIN_SENSOR_PIN);
  if (sensor == HIGH) {
    //rain sensor isn't triggered, but see if the rain delay is finished
    if (now() < (options.rain_time + (options.rain_delay * 60))) {
      rain = true;
    } else {
      rain = false;
    }
  } else {
    rain = true;
    options.rain_time = now();
  }
}

void sanity()
{
  int count = 0;
  int i;
  struct program_t pgm;

  if (running == RUNNING_ERR) {
    return;
  }

  //see how many programs are running
  for (i = 0; i < options.numprograms; i++) {
    load_program(i, &pgm);
    if (progstat[i].state == PROG_ON) {
      count++;
    }
  }
  //make sure the status matches the number of running programs
  if (count == running) {
    return;
  }

  wdt_reset();

  //program status does not match valve status
  //reset all program status
  for (i = 0; i < options.numprograms; i++) {
    progstat[i].state = PROG_OFF;
  }
  //turn off all valves
  for (i = 0; i < (sizeof(valve_pins)/sizeof(int)); i++) {
    pinMode(valve_pins[i], OUTPUT);
    digitalWrite(valve_pins[i], HIGH);
  }
  //show that we hit an error
  running = RUNNING_ERR;

  wdt_reset();
}

void setup()
{
  int idx;
  //network info
  IPAddress web_ip(192, 168, 1, 180); //sprinkler ip address
  byte mac[] = { 0x90, 0xa2, 0xda, 0x0d, 0x3f, 0xd6 };

  //start the watchdog timer
  wdt_enable(WDTO_8S);

  //debug serial
  #ifdef SERIAL_PRINT
  Serial.begin(9600);
  SerialPrintln("started");
  #endif

  //initialize pins to high to turn off relays
  for (idx = 0; idx < (sizeof(valve_pins)/sizeof(int)); idx++) {
    pinMode(valve_pins[idx], OUTPUT);
    digitalWrite(valve_pins[idx], HIGH);
  }
  wdt_reset();

  //set analog pin 0 to input for rain sensor
  pinMode(RAIN_SENSOR_PIN, INPUT);

  //load the options
  load_options();
  wdt_reset();

  //initialize programs
  init_programs();
  wdt_reset();

  //start network
  #ifdef SERIAL_PRINT
  SerialPrintln("starting network");
  #endif
  Ethernet.begin(mac, web_ip);
  wdt_reset();

  //get the time
  #ifdef SERIAL_PRINT
  SerialPrintln("starting ntp");
  #endif
  ntp_udp.begin(NTP_PORT);
  setSyncInterval(43200); //12 hours
  setSyncProvider(update_time);
  now(); //try to force an update of the clock
  start_time = update_time();
  if (start_time != 0) {
      setTime(start_time);
  }
  //record the starting time
  start_time = now();

  //start the web server
  #ifdef SERIAL_PRINT
  SerialPrintln("starting web server");
  #endif
  server.begin();
  wdt_reset();

  //print_time();
  #ifdef SERIAL_PRINT
  SerialPrintln("done with setup");
  #endif
}

void loop()
{
  //print_time();
  wdt_reset();
  check_rain();
  wdt_reset();
  toggle_programs();
  wdt_reset();
  process_web();
  wdt_reset();
  sanity();
  wdt_reset();
  delay(5);
}
