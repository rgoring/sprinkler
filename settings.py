#!/usr/bin/python

#convert the json data saved by the sprinkler controller back into
#commands to program the sprinkler if settings are wiped

import json

def get_tf_str(opt):
	if opt:
		return "true"
	return "false"

def print_options(opts):
	print "spr valvedelay/" + str(jdata['options']['valvedelay'])
	print "spr scale/" + str(jdata['options']['scale'])
	print "spr tz/" + str(jdata['options']['tz'])
	print "spr dst/" + str(jdata['options']['dst'])
	print "spr raindelay/" + str(jdata['options']['raindelay'])
	print "spr master/" + get_tf_str(jdata['options']['master'])

def print_program(prog, idx):
	print "spr add"
	print "spr name/" + str(idx) + "/" + str(prog['name'])
	print "spr duration/" + str(idx) + "/" + str(prog['duration'])
	print "spr valve/" + str(idx) + "/" + str(prog['valve'])
	days = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
	for x,c in enumerate(prog['day']):
		if c != ' ':
			print "spr day/" + str(idx) + "/+" + days[x]
	for t in range(len(prog['times'])):
		if prog['times'][t]['start'] != '00:00':
			print "spr time/" + str(idx) + "/" + str(t+1) + "/" + prog['times'][t]['start']
	print "spr enable/" + str(idx) + "/" + get_tf_str(prog['enabled'])
#begin main

#read settings
with open("sprinkler.json", "r") as jsonfile:
	filedata = jsonfile.read()

jdata = json.loads(filedata)

#import pprint
#pprint.pprint(jdata)

#options
print_options(jdata['options'])


if jdata['options']['programs'] != len(jdata['status']):
	raise Exception("Program count does not match number of programs defined!!")

programs = list()
for i in range(len(jdata['status'])):
	print_program(jdata['status'][i], i+1)