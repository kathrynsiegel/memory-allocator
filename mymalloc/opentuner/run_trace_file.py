#!/usr/bin/python2.7
#
import logging
import os
import threading
import opentuner
from opentuner import ConfigurationManipulator
from opentuner import MeasurementInterface
from opentuner import Result
from opentuner.measurement.inputmanager import FixedInputManager
from opentuner.search.objective import MaximizeAccuracy
from opentuner.search.manipulator import PowerOfTwoParameter
import params

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

def parse_lankaout(f):
    result = {}
    for line in f:
        line_split = line.split(':', 1)
        if len(line_split) == 2:
            key, value = line_split
            result[key] = try_num(value)
    return result
            

class MdriverTuner(MeasurementInterface):
  best_accuracy = 0
  best_make_cmd = best_bin_cmd = ''

  lock = threading.Lock()

  def __init__(self, args):
    assert args.trace_file != None

    super(MdriverTuner, self).__init__(
      args,
      objective=MaximizeAccuracy(),
      input_manager=FixedInputManager())

  def manipulator(self):
    """
    Define the search space by creating a
    ConfigurationManipulator
    """
    return params.mdriver_manipulator

  def save_final_config(self, configuration):
    """
    Called at the end of tuning
    """
    print 'trace_file:' + self.args.trace_file
    print 'make_cmd:' + self.best_make_cmd
    print 'bin_cmd:' + self.best_bin_cmd
    print 'perfidx:' + str(self.best_accuracy)
    print

  def run(self, desired_result, input, limit):
    """
    Compile and run a given configuration then
    return performance
    """
    accuracy = 0
    time = 0
    cfg = desired_result.configuration.data

    lanka = self.args.lanka
    print "lanka=%d" % lanka

    params = ''
    for key, value in cfg.iteritems():
      params += '-D {0}={1} '.format(key, value)
    make_cmd = ''
    if lanka==0:
        make_cmd = 'make partial_clean mdriver DEBUG=0 PARAMS="{0}"'.format(params)
    else:
        make_cmd = 'make partial_clean mdriver GETTIME=1 DEBUG=0 PARAMS="{0}"'.format(params)

    compile_result = self.call_program(make_cmd, limit=4)
    #time += compile_result['time']
    if compile_result['returncode'] != 0:
      return Result(accuracy=accuracy, time=time)


    bin_cmd = ''
    if lanka==0:
        bin_cmd = './mdriver -g -f ' + self.args.trace_file
    else:
        bin_cmd = './mdriver -g -f ' + self.args.trace_file + ' >> lanka_out'

    run_result = self.call_program(bin_cmd, limit=2)
    if lanka==0:
        time += run_result['time']
        if run_result['timeout'] or run_result['returncode'] != 0:
            return Result(accuracy=accuracy, time=time)

        result = parse_stdout(run_result['stdout'])
        accuracy = result.get('perfidx', accuracy)
        with self.lock:
            if accuracy > self.best_accuracy:
                self.best_accuracy = accuracy
                self.best_make_cmd = make_cmd
                self.best_bin_cmd = bin_cmd
        return Result(accuracy=accuracy, time=time)

    else:
        f = open('lanka_out', 'r')
        result = parse_lankaout(f)
        if len(result)!=3:
            return Result(accuracy=accuracy, time=time)
        time += result['runtime']
        accuracy = result['perfidx']
        with self.lock:
            if accuracy > self.best_accuracy:
                self.best_accuracy = accuracy
                self.best_make_cmd = make_cmd
                self.best_bin_cmd = bin_cmd
        os.system('rm lanka_out');
        return Result(accuracy=accuracy, time=time)
        

if __name__ == '__main__':
  logging.basicConfig(level=logging.ERROR)
  argparser = opentuner.default_argparser()
  argparser.add_argument('--trace-file', default=None)
  argparser.add_argument('--lanka', type=int, default=0)
  args = argparser.parse_args()
  MdriverTuner.main(args)
