#!/usr/bin/env python
#
# client.py
#
#  Created on: 20 mars 2014
#      Author: Sylvain Afchain <safchain@gmail.com>
#

import getopt
import socket
import struct
import sys
import yaml

from twisted.internet import protocol
from twisted.internet import reactor
import drone_pb2

TOTAL_PACKET = 1
INTERVAL_PACKET = 2
ERROR_PACKET = 3


class DroneConnectionException(Exception):
    def __init__(self, value):
        self.value = value
    def __str__(self):
        return repr(self.value)


class DroneClient(protocol.Protocol):
    
    def connectionMade(self):
        run = self.factory.run
        connection = self.factory.connection
        
        to = connection['to'].split(':')
        
        job_request = drone_pb2.JobRequest()
        job_request.port = int(to[1])
        job_request.block_size = run['block_size']
        job_request.sessions = run['sessions']
        job_request.seconds = run['seconds']
        job_request.interval = run['interval']
        job_request.rampup_interval = run['rampup_interval']
        job_request.rampup_sessions = run['rampup_sessions']
        job_request.messages_interval = run['messages_interval']
        job_request.messages_percent = run['messages_percent']
        job_request.timeout = run['timeout']
        
        addr = socket.gethostbyname(to[0])
        job_request.ipv4 = struct.unpack("!I", socket.inet_aton(addr))[0]
        
        job_str = job_request.SerializeToString()
        self.transport.write(struct.pack('!I', len(job_str)) + job_str)
    
    def dataReceived(self, data):
        (size, type), data = struct.unpack("!IB", data[:5]), data[5:]

        if type == TOTAL_PACKET:
            self.totalReceived(data)
        elif type == INTERVAL_PACKET:
            self.intervalReceived(data)
        elif type == ERROR_PACKET:
            self.errorMessageReceived(data)
            
    def errorMessageReceived(self, data):
        connection = self.factory.connection
        
        try:
            job_error = drone_pb2.JobErrorMessage()
            job_error.ParseFromString(data)
        except:
            return

        print "%s - %s" % (connection['from'], job_error.message)
          
    def intervalReceived(self, data):
        connection = self.factory.connection
        
        self.factory.interval += 1
        
        try:
            job_interval = drone_pb2.JobIntervalResult()
            job_interval.ParseFromString(data)  
        except:
            return

        print ("%s - [%5d] Messages: %8d, Bytes: %12d, "
               "Throughtput(KB/sec): %5d, Connections: %6d"
               ) % (connection['from'],
                    self.factory.interval,
                    job_interval.total_messages_read,
                    job_interval.total_bytes_read,
                    job_interval.total_bytes_read / 
                    (self.factory.run['interval'] * 1024 * 1024),
                    job_interval.total_sessions)
            
    def totalReceived(self, data):
        job_result = drone_pb2.JobResult()
        job_result.ParseFromString(data)
        
        run_id = self.factory.run['id'] 
        
        # global result
        result = self.factory.results.get(run_id)
        if not result:
            result = {'total_sessions':
                      job_result.total_sessions,
                      'total_drone_results': 1,
                      'total_bytes_read': 
                      job_result.total_bytes_read,
                      'total_messages_read': 
                      job_result.total_messages_read}
            self.factory.results[run_id] = result
        else:
            result['total_sessions'] += job_result.total_sessions
            result['total_drone_results'] += 1
            result['total_bytes_read'] += job_result.total_bytes_read
            result['total_messages_read'] += job_result.total_messages_read
            
        # result per drone
        per_drone = result.get('drones', {})
        per_drone[self.factory.connection['from']] = {
            'total_sessions':
            job_result.total_sessions,
            'total_bytes_read': 
            job_result.total_bytes_read,
            'total_messages_read': 
            job_result.total_messages_read,
            'total_throughtput_kbs': (
                job_result.total_bytes_read / 
                (self.factory.run['seconds'] * 1024))}
        result['drones'] = per_drone

        self.transport.loseConnection()

    def connectionLost(self, reason):
        connection = self.factory.connection

        print "Drone %s has finished its job" % connection['from']


class DroneFactory(protocol.ClientFactory):
    protocol = DroneClient

    def __init__(self, run, connection, results, num_jobs=1):
        self.run = run
        self.connection = connection
        self.results = results
        self.num_jobs = num_jobs
        self.interval = 0
        
    def clientConnectionFailed(self, connector, reason):
        results = self.results.get(self.run['id'])
        if not results:
            raise DroneConnectionException(
                "Error no result for connection : %s" % self.connection)
        
        if(results['total_drone_results'] == self.num_jobs and 
           reactor.running):
            reactor.stop()
    
    def clientConnectionLost(self, connector, reason):
        results = self.results.get(self.run['id'])
        if not results:
            raise DroneConnectionException(
                "Error no result for connection : %s" % self.connection)
        
        if(results['total_drone_results'] == self.num_jobs and 
           reactor.running):
            reactor.stop()


def usage():
    print 'Usage: ' + sys.argv[0] + ' -r <requests file>'
    sys.exit(2)


def main(argv):
    
    try:                                
        opts, args = getopt.getopt(argv, "r:", ["requests="])
    except getopt.GetoptError: 
        usage()
            
    for opt, arg in opts:
        if opt in ('-r', '--requests'):
           
            with open(arg, 'r') as stream:
                runs = yaml.load(stream)
            
            results = {}
            for run in runs:
                num_jobs = len(run['connections'])
                for connection in run['connections']:
                    f = connection['from'].split(':')
            
                    df = DroneFactory(run,
                                      connection,
                                      results,
                                      num_jobs)
                    
                    reactor.connectTCP(f[0], int(f[1]), df)
            
                reactor.run()
                
                for connection in run['connections']:
                    results[run['id']]['total_throughtput_kbs'] = (
                        results[run['id']]['total_bytes_read'] / 
                    (run['seconds'] * 1024))
                
            print yaml.safe_dump(results, default_flow_style=False)
            
            break
    else:
        usage()
    

if __name__ == '__main__':
    main(sys.argv[1:])
