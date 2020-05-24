import datetime
from dateutil import parser
import numpy as np
import random
import pyhash

# We assume a Zipfian distribution on the key
class Zipfian:
    def __init__(self, record_count, zipfian_constant):
        self.record_count = record_count
        self.zipfian_constant = zipfian_constant
        self.zetan = np.sum(1 / (np.arange(1, record_count + 1) ** zipfian_constant))
        
        self.theta = self.zipfian_constant
        self.zeta2theta = np.sum(1 / (np.arange(1, 3) ** zipfian_constant))
        self.alpha = 1.0 / (1.0 - self.theta)
        self.eta = (1 - ((2.0 / self.record_count) ** (1 - self.theta))) / (1 - (self.zeta2theta / self.zetan))
        self.next_value()
        
    def next_value(self):
        return int(self.next_long())
    
    def next_long(self):
        u = random.random()
        uz = u * self.zetan
        
        if uz < 1:
            return 0
        if uz < 1 + 0.5 ** self.theta:
            return 1
        
        ret = int(self.record_count * ((self.eta * u - self.eta + 1) ** self.alpha))
        return ret

# 1. Extract read and write requests from the trace
req_list = list()
with open("snowset-main.csv", "r") as fp:
    for line_index, line in enumerate(fp):
        # skip table header
        if line_index == 0:
            continue
        line_split = line.split(",")
        time_stamp = parser.parse(line_split[3]).timestamp()
        read_size = int(line_split[14]) + int(line_split[16])
        write_size = int(line_split[20])
        
        if read_size > 0:
            req_list.append((time_stamp, 0, read_size))
        if write_size > 0:
            req_list.append((time_stamp, 1, write_size))
        
        if line_index % 10000 == 0:
            print("{}/{} ({}%)".format(line_index + 1, 69182075, ((line_index + 1) / (69182075)) * 100))

# 2. Sort the trace by their request time
req_arr = np.array(req_list)
req_arr = req_arr[req_arr[:, 0].argsort(), :]
req_arr[:, 0] -= req_arr[0, 0]

# 3. Generate a Memcachier-like workload file:
record_count = 1000000
zipfian_constant = 0.7
zipfian = Zipfian(record_count, zipfian_constant)
hasher = pyhash.fnv1_64()

with open("snowset-m.out", "w") as fp:
    for record_index in range(req_arr.shape[0]):
        key_id = hasher(zipfian.next_long().to_bytes(length=8, byteorder='little')) % record_count
        fp.write("{},0,{},0,{},{},0\n".format(req_arr[record_index, 0], 
                                              int(req_arr[record_index, 1] + 1),
                                              int(req_arr[record_index, 2]),
                                              key_id,))
        if record_index % 1000000 == 0:
            print("{}/{} ({}%)".format(record_index + 1, req_arr.shape[0], ((record_index + 1) / (req_arr.shape[0])) * 100))