#!/usr/bin/python2.7

##
# StarPU --- Runtime system for heterogeneous multicore architectures.
#
# Copyright (C) 2016 INRIA
#
# StarPU is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2.1 of the License, or (at
# your option) any later version.
#
# StarPU is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#
# See the GNU Lesser General Public License in COPYING.LGPL for more details.
##

##
# This script parses the generated trace.rec file and reports statistics about
# the number of different events/tasks and their durations. The report is
# similar to the starpu_paje_state_stats.in script, except that this one
# doesn't need R and pj_dump (from the pajeng repository), and it is also much
# faster.
##

import getopt
import os
import sys

class Event():
    def __init__(self, type, name, category, start_time):
        self._type = type
        self._name = name
        self._category = category
        self._start_time = start_time

class EventStats():
    def __init__(self, name, duration_time, category, count = 1):
        self._name = name
        self._duration_time = duration_time
        self._category = category
        self._count = count

    def aggregate(self, duration_time):
        self._duration_time += duration_time
        self._count += 1

    def show(self):
        if not self._name == None:
            print "\"" + self._name + "\"," + str(self._count) + ",\"" + self._category + "\"," + str(round(self._duration_time, 6))

class Worker():
    def __init__(self, id):
        self._id        = id
        self._events    = []
        self._stats     = []
        self._stack     = []

    def get_event_stats(self, name):
        for stat in self._stats:
            if stat._name == name:
                return stat
        return None

    def add_event(self, type, name, category, start_time):
        self._events.append(Event(type, name, category, start_time))

    def calc_stats(self):
        num_events = len(self._events) - 1
        for i in xrange(0, num_events):
            curr_event = self._events[i]
            next_event = self._events[i+1]

            if next_event._type == "PushState":
                self._stack.append(next_event)
                for j in xrange(i+1, num_events):
                    next_event = self._events[j]
                    if next_event._type == "SetState":
                        break
            elif next_event._type == "PopState":
                curr_event = self._stack.pop()

            # Compute duration with the next event.
            a = curr_event._start_time
            b = next_event._start_time

            found = False
            for j in xrange(len(self._stats)):
                if self._stats[j]._name == curr_event._name:
                    self._stats[j].aggregate(b - a)
                    found = True
                    break
            if not found == True:
                self._stats.append(EventStats(curr_event._name, b - a, curr_event._category))

def read_blocks(input_file):
    empty_lines = 0
    first_line = 1
    blocks = []
    for line in open(input_file):
        if first_line:
            blocks.append([])
            blocks[-1].append(line)
            first_line = 0

        # Check for empty lines
        if not line or line[0] == '\n':
            # If 1st one: new block
            if empty_lines == 0:
                blocks.append([])
            empty_lines += 1
        else:
            # Non empty line: add line in current(last) block
            empty_lines = 0
            blocks[-1].append(line)
    return blocks

def read_field(field, index):
    return field[index+1:-1]

def insert_worker_event(workers, block):
    worker_id = -1
    name = None
    start_time = 0.0
    category = None

    for line in block:
        if line[:2] == "E:": # EventType
            event_type = read_field(line, 2)
        elif line[:2] == "C:": # Category
            category = read_field(line, 2)
        elif line[:2] == "W:": # WorkerId
            worker_id = int(read_field(line, 2))
        elif line[:2] == "N:": # Name
            name = read_field(line, 2)
        elif line[:2] == "S:": # StartTime
            start_time = float(read_field(line, 2))

    for worker in workers:
        if worker._id == worker_id:
            worker.add_event(event_type, name, category, start_time)
            return
    worker = Worker(worker_id)
    worker.add_event(event_type, name, category, start_time)
    workers.append(worker)

def calc_times(stats):
    tr = 0.0 # Runtime
    tt = 0.0 # Task
    ti = 0.0 # Idle
    for stat in stats:
        if stat._category == None:
            continue
        if stat._category == "Runtime":
            tr += stat._duration_time
        elif stat._category == "Task":
            tt += stat._duration_time
        elif stat._category == "Other":
            ti += stat._duration_time
        else:
            sys.exit("Unknown category '" + stat._category + "'!")
    return (ti, tr, tt)

def save_times(ti, tr, tt):
    f = open("times.csv", "w+")
    f.write("\"Time\",\"Duration\"\n")
    f.write("\"Runtime\"," + str(tr) + "\n")
    f.write("\"Task\"," + str(tt) + "\n")
    f.write("\"Idle\"," + str(ti) + "\n")
    f.close()

def calc_et(tt_1, tt_p):
    """ Compute the task efficiency (et). This measures the exploitation of
    data locality. """
    return tt_1 / tt_p

def calc_er(tt_p, tr_p):
    """ Compute the runtime efficiency (er). This measures how the runtime
    overhead affects performance."""
    return tt_p / (tt_p + tr_p)

def calc_ep(tt_p, tr_p, ti_p):
    """ Compute the pipeline efficiency (et). This measures how much
    concurrency is available and how well it's exploited. """
    return (tt_p + tr_p) / (tt_p + tr_p + ti_p)

def calc_e(et, er, ep):
    """ Compute the parallel efficiency. """
    return et * er * ep

def save_efficiencies(e, ep, er, et):
    f = open("efficiencies.csv", "w+")
    f.write("\"Efficiency\",\"Value\"\n")
    f.write("\"Parallel\"," + str(e) + "\n")
    f.write("\"Task\"," + str(et) + "\n")
    f.write("\"Runtime\"," + str(er) + "\n")
    f.write("\"Pipeline\"," + str(ep) + "\n")
    f.close()

def usage():
    print "USAGE:"
    print "starpu_trace_state_stats.py [ -te -s=<time> ] <trace.rec>"
    print
    print "OPTIONS:"
    print " -t or --time            Compute and dump times to times.csv"
    print
    print " -e or --efficiency      Compute and dump efficiencies to efficiencies.csv"
    print
    print " -s or --seq_task_time   Used to compute task efficiency between sequential and parallel times"
    print "                         (if not set, task efficiency will be 1.0)"
    print
    print "EXAMPLES:"
    print "# Compute event statistics and report them to stdout:"
    print "python starpu_trace_state_stats.py trace.rec"
    print
    print "# Compute event stats, times and efficiencies:"
    print "python starpu_trace_state_stats.py -te trace.rec"
    print
    print "# Compute correct task efficiency with the sequential task time:"
    print "python starpu_trace_state_stats.py -s=60093.950614 trace.rec"

def main():
    try:
        opts, args = getopt.getopt(sys.argv[1:], "hets:",
                                   ["help", "time", "efficiency", "seq_task_time="])
    except getopt.GetoptError as err:
        usage()
        sys.exit(1)

    dump_time = False
    dump_efficiency = False
    tt_1 = 0.0

    for o, a in opts:
        if o in ("-h", "--help"):
            usage()
            sys.exit()
        elif o in ("-t", "--time"):
            dump_time = True
        elif o in ("-e", "--efficiency"):
            dump_efficiency = True
        elif o in ("-s", "--seq_task_time"):
            tt_1 = float(a)

    if len(args) < 1:
        usage()
        sys.exit()
    recfile = args[0]

    if not os.path.isfile(recfile):
        sys.exit("File does not exist!")

    # Declare a list for all workers.
    workers = []

    # Read the recutils file format per blocks.
    blocks = read_blocks(recfile)
    for block in blocks:
        if not len(block) == 0:
            first_line = block[0]
            if first_line[:2] == "E:":
                insert_worker_event(workers, block)

    # Compute worker statistics.
    stats = []
    for worker in workers:
        worker.calc_stats()
        for stat in worker._stats:
            found = False
            for s in stats:
                if stat._name == s._name:
                    found = True
                    break
            if not found == True:
                stats.append(EventStats(stat._name, 0.0, stat._category, 0))

    # Compute global statistics for all workers.
    for i in xrange(0, len(workers)):
        for stat in stats:
            s = workers[i].get_event_stats(stat._name)
            if not s == None:
                # A task might not be executed on all workers.
                stat._duration_time += s._duration_time
                stat._count += s._count

    # Output statistics.
    print "\"Name\",\"Count\",\"Type\",\"Duration\""
    for stat in stats:
        stat.show()

    # Compute runtime, task, idle times and dump them to times.csv
    ti_p = tr_p = tt_p = 0.0
    if dump_time == True:
        ti_p, tr_p, tt_p = calc_times(stats)
        save_times(ti_p, tr_p, tt_p)

    # Compute runtime, task, idle efficiencies and dump them to
    # efficiencies.csv.
    if dump_efficiency == True or not tt_1 == 0.0:
        if dump_time == False:
            ti_p, tr_p, tt_p = calc_times(stats)
        if tt_1 == 0.0:
            sys.stderr.write("WARNING: Task efficiency will be 1.0 because -s is not set!\n")
            tt_1 = tt_p

        # Compute efficiencies.
        ep = round(calc_ep(tt_p, tr_p, ti_p), 6)
        er = round(calc_er(tt_p, tr_p), 6)
        et = round(calc_et(tt_1, tt_p), 6)
        e  = round(calc_e(et, er, ep), 6)
        save_efficiencies(e, ep, er, et)
if __name__ == "__main__":
    main()
