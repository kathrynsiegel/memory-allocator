#!/usr/bin/python2.6
#
import argparse
import re
import os
import subprocess

def try_num(s):
    try:
        return int(s)
    except ValueError:
        try:
            return float(s)
        except ValueError:
            return s

def parse_stdout(stdout):
    result = {}
    for line in stdout.splitlines():
        line_split = line.split(':', 1)
        if len(line_split) == 2:
            key, value = line_split
            result[key] = try_num(value)
    return result

if __name__ == '__main__':
  argparser = argparse.ArgumentParser()
  argparser.add_argument('--trace-dir', default='traces')
  argparser.add_argument('--trace-file', default=None)
  argparser.add_argument('--cq', action='store_true')
  args = argparser.parse_args()

  if args.trace_file == None:
    trace_files = [os.path.join(args.trace_dir, trace_file) for trace_file in os.listdir(args.trace_dir)]
    if len(trace_files) != 10:
      print "# Missing any traces? expected: 10, actual: " + str(len(trace_files))
  else:
    trace_files = [args.trace_file]
  trace_files.sort()

  total_accuracy = 0.0
  num_trace_files = 0

  for trace_file in trace_files:
    print 'trace_file:' + trace_file

    m = re.search('trace_c(\d)_v(\d)', trace_file)
    if m == None:
      print '# Trace file {0} does not match trace_c{{C}}_v{{V}} pattern.'.format(trace_file)
      continue
    else:
      trace_class = m.group(1)
      subprocess.check_call('make clean mdriver DEBUG=0'
          ' PARAMS="-D TRACE_CLASS={0}" >/dev/null'.format(trace_class),
          shell=True)

    bin_cmd = './mdriver -g -f ' + trace_file
    if args.cq:
      bin_cmd = 'cqsub ' + bin_cmd

    proc = subprocess.Popen(bin_cmd, shell=True, stdout=subprocess.PIPE)
    stdout, _ = proc.communicate()
    print stdout
    assert(proc.returncode == 0)

    result = parse_stdout(stdout)
    total_accuracy += result.get('perfidx', 0)
    num_trace_files += 1

  print 'average_accuracy:' + str(total_accuracy / num_trace_files)
