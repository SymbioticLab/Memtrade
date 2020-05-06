import sys;
import os;
import csv;
import json;
from os import path;
from enum import Enum;

JOB_FILENAME = "jobs.json";
STAT_FILENAME = "/newdir/stat.json";
TRACE_DIR = "/newdir/google_trace";
MACHINE_USAGE = "machine_usage";
REQUEST = "/newdir/request.json";
PRODUCER = "/newdir/producer.json";
load_from_file = 1;

def usage():
	print("python3 %s -p <trace_location> -r <task_count>" % (sys.argv[0]));

def process_argv(argv):
	options = {};
	while argv:
		if argv[0][0] == '-':
			if len(argv) > 1:
				options[argv[0]] = argv[1];
			else:
				options[argv[0]] = -1;
		argv = argv[1:];
	return options;

def get_file_names(trace_type):
	files = os.listdir(path.join(TRACE_DIR, trace_type));
	return files;


def get_machine(machineid, time_limit):
	usage_file_name = path.join(path.join(TRACE_DIR, MACHINE_USAGE), "machine_usage_" + machineid + '.csv' );
	machine = [];
	dist = [];
	max_mem = 0;
	for i in range(0, time_limit):
		machine.append(-1);

	#fieldnames = ['time', 'machine', 'cpu', 'assigned mem', 'file cache', 'total cache', 'max mem', 'total mem', 'disk'];
	with open(usage_file_name) as csv_file:
		csv_reader = csv.reader(csv_file, delimiter=' ');
		for row in csv_reader:
			#print(row);
			splits = row[0].split(",");
			if splits[0] == 'time':
				continue;
			time = (int)(splits[0]);
			memory = (float)(splits[7]);
			if time < len(machine):
				machine[time] = memory;

			if memory > max_mem:
				max_mem = memory;
	
	for i in range(0, max(10, (int)(max_mem*10) + 1)):
		dist.append(0);

	if machine[0] == -1 :
		machine[0] = 0;
	
	for i in range(1, time_limit):
		if machine[i] == -1 :
			machine[i] = machine[i-1];
		dist_idx = (int)(machine[i]*10);
		dist[dist_idx] = dist[dist_idx] + 1;
	
	for i in range(0, len(dist)):
		dist[i] = (float)(dist[i]*1.0/time_limit);
		if i > 0:
			dist[i] = dist[i] + dist[i-1];

	return {"usage" : machine, "dist": dist};

max_usage_cap = 0.75
time_limit = 2890
producer_use_duration_cap = 0.5 #(producer_use_duration_cap *10) % of time
producer_use_memory_cap = 5 #(producer_use_memory_cap +1)*10 % of memory

consumer_use_duration_cap = 0.1
consumer_use_memory_cap = 9

consumer_size = 512; # 512GB machine
producer_size = 64; # 64 GB machine
producer_count = 100;
split_factor = 1;

def generate_requests(usage):
	time_limit = len(usage);
	request = [];
	temp_req = {};
	max_mem = 0;

	exceed = 0;
	for t in range(0, time_limit):
		if usage[t] >= max_usage_cap:
			if exceed == 0:
				exceed = 1;
				temp_req = {"start": (int)(t)}
				if usage[t] - max_usage_cap > max_mem:
					max_mem = usage[t] - max_usage_cap;
		elif exceed == 1:
			temp_req["duration"] = t - temp_req["start"];
			temp_req["memory"] = max_mem;
			request.append(temp_req);

			exceed = 0;
			max_mem = 0;
			temp_req = {};
	if exceed == 1:
		temp_req["duration"] = t - temp_req["start"];
		temp_req["memory"] = max_mem;
		request.append(temp_req);

	return request;

def arrival_distribution(requests):
	fields = ["time", "requests", "memory"];
	arrival_distribution_path = path.join(TRACE_DIR, "arrival_dist_"+ str((int)(max_usage_cap*100))+".csv" );
	with open(arrival_distribution_path, mode='w') as csv_file:
		writer = csv.DictWriter(csv_file, fieldnames=fields);
		writer.writeheader();
		for time in sorted(requests.keys()):
			row = {"time":time, "requests": len(requests[time])}
			memory = 0;
			for item in requests[time]:
				memory = item[1];
			row["memory"] = memory;
			writer.writerow(row);

def memory_duration_distribution(requests, _memory_cdf =[], _duration_cdf = []):
	memory = [];
	duration = [];
	memory_cdf = [];
	duration_cdf = [];
	length = 0;
	distribution_path = path.join(TRACE_DIR, "memory_duration_dist_"+ str((int)(max_usage_cap*100))+".csv" );
	
	for time in requests.keys():
		for item in requests[time]:
			memory.append(item[1]);
			duration.append(item[0]);
			length = length + 1;
	
	memory.sort(reverse=False);
	duration.sort(reverse=False);

	memory_cdf.append(memory[0]);
	duration_cdf.append(duration[0]);

	for i in range(1, 101):
		idx = (int)(length*0.01* i) - 1
		memory_cdf.append(memory[idx]);
		duration_cdf.append(duration[idx]); 

	fields = ["%", "memory", "duration"];

	with open(distribution_path, mode='w') as csv_file:
		writer = csv.DictWriter(csv_file, fieldnames=fields);
		writer.writeheader();
		for i in range(0,101):
			row = {"%": i, "memory": memory_cdf[i], "duration": duration_cdf[i]};
			writer.writerow(row);

	_memory_cdf = memory_cdf;
	_duration_cdf = duration_cdf;
	return _memory_cdf;

def select_producers(cluster, candidate):
	producer = {};
	count = 0;
	for k in sorted(candidate, key=lambda k: candidate[k], reverse=True):
		producer[k] = cluster[k];
		count = count + 1;
		if count > producer_count:
			break;

	return producer;

def scale_request(requests):
	for time in requests:
		for i in range(0, len(requests[time])):
			requests[time][i][1] = requests[time][i][1] * consumer_size;

	return requests;

def scale_producer(producer):
	for m in producer:
		for i in range(0, len(producer[m])):
			producer[m][i] = producer[m][i] * producer_size;

	return producer;

def preprocess(machine_limit, request_limit):
	cluster = {};
	consumer = {};
	producer = {};
	request = {};
	request_count = 0;
	i = 0; 
	with open(STAT_FILENAME) as stat_file:
		stat = json.load(stat_file);

	for i in range(0, machine_limit):
		machine_id = stat[i][0];
		m = get_machine(machine_id, time_limit);
		cluster[machine_id] = m["usage"];
		if len(m["dist"]) <= 10 and m["dist"][producer_use_memory_cap] <= producer_use_duration_cap:
			producer[machine_id] = m["dist"][producer_use_memory_cap];
		elif len(m["dist"]) > 11 and (1 - m["dist"][consumer_use_memory_cap]) >= consumer_use_duration_cap:
			consumer[machine_id] = m["dist"][producer_use_memory_cap];

		r = generate_requests(cluster[machine_id]);

		for item in r:
			key = (int) (item["start"]);
			if key not in request:
				request[key] = [];
			request[key].append([item["duration"], item["memory"]]);
			request_count = request_count + 1;
		
		if request_limit < request_count:
			break;
		#print("\nMachine id: {0}\n{1}".format(machine_id, m["dist"]));
	print("\nfrom top {0} machines, {1} request generated\n #producer: {2}, #consumer: {3}".format(i+1, request_count, len(producer), len(consumer)));
	
	_memory_cdf = memory_duration_distribution(request);
	arrival_distribution(request);
	
	_requests = scale_request(request);
	_producer = scale_producer(select_producers(cluster, producer));

	with open(REQUEST, "w" ) as f:
		json.dump(_requests, f);
	
	with open(PRODUCER, "w" ) as f:
		json.dump(_producer, f);

	return (_requests, _producer, _memory_cdf)

def available_machines(producer, request, current_time):
	t = 0;
	machines = {};

	for m in producer:
		min_memory = 1000000;
		if current_time + request[0] > len(producer[m]):
			continue;
		for t in range(0, request[0]):
			free_mem = producer_size - producer[m][current_time+t];
			if free_mem < request[1]:
				break;
			elif min_memory > free_mem:
				min_memory = free_mem;
		if t >= request[0] - 1:
			machines[m] = min_memory;
	return machines;	

def add_request_to_machine(producer, request, current_time):
	for t in range(0, request[0]):
		producer[current_time+t] = producer[current_time+t] + request[1];
	return producer;

def find_greedy_best_mach(candidate):
	return sorted(candidate.items(), key=lambda c: c[1])[0][0];

def run_simulator(machine_limit, request_limit):
	requests = {};
	producer = {};
	memory_cdf = [];
	pending_request = [];
	request_count = 0;
	next_count = 1000;
	splits = 0;

	if load_from_file == 1:
		with open(PRODUCER) as f:
			producer = json.load(f);
		with open(REQUEST) as f:
			requests = json.load(f);
		print("loading done\n\n")
	else:
		(requests, producer, memory_cdf) = preprocess(machine_limit, request_limit);
		print("preprocessing done\n\n");
	
	for time in sorted(requests.keys()):
		assigned = [];
		for req in requests[time]:
			pending_request.append({"request": req, "submit": time, "left": split_factor, "mem": req[1]});
			request_count = request_count + 1;
		
		for idx in range(0, len(pending_request)):
			req = pending_request[idx]["request"];
			split_done = 0;
			remained_splits = (int) (pending_request[idx]["left"]);
			for split in range(1, remained_splits+1):
				req[1] = req[1] / split_factor;
				candidate_machine = available_machines(producer, req, (int)(time));

				if len(candidate_machine) > 0:
					m = find_greedy_best_mach(candidate_machine);
					producer[m] = add_request_to_machine(producer[m], req, (int)(time));
					split_done = split_done + 1;
		
			pending_request[idx]["left"] = max(0, pending_request[idx]["left"] - split_done);
			pending_request[idx]["request"][1] = pending_request[idx]["mem"] * (pending_request[idx]["left"]*1.0/split_factor);

			if pending_request[idx]["left"] == 0:
				assigned.append(pending_request[idx]);

		for item in assigned:
			pending_request.remove(item);

		if request_count > next_count:
			next_count = next_count + 1000;
			print("out of {0} requests, {1} requests could not assigned, {2}% was assigned".format(request_count, len(pending_request), 100-((len(pending_request)*100.0)/request_count)))		
	
	print("out of {0} requests, {1} requests could not assigned, {2}% was assigned".format(request_count, len(pending_request), 100-((len(pending_request)*100.0)/request_count)))

def main():
	myargvs = process_argv(sys.argv);
	machine_limit = 10000;
	request_limit = 100000;

	global TRACE_DIR;
	global max_usage_cap;
	global load_from_file;
	global producer_size;
	global split_factor;

	if('-h' in myargvs):
		usage();
		return;
	if('-p' in myargvs):
		TRACE_DIR = myargvs['-p'];
	if('-m' in myargvs):
		machine_limit = (int)(myargvs['-m']);
	if('-r' in myargvs):
		request_limit = (int)(myargvs['-r']);
	if('-l' in myargvs):
		load_from_file = (int)(myargvs['-l']);
	if('-s' in myargvs):
		producer_size = (int)(myargvs['-s']);
	if('-c' in myargvs):
		max_usage_cap = (float)(myargvs['-c']);
	if('-f' in myargvs):
		split_factor = (int)(myargvs['-f']);

	run_simulator(machine_limit, request_limit);
	
if __name__ == "__main__":
	main();
