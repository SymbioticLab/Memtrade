import sys;
import os;
import csv;
import json;
from os import path;
from enum import Enum;

JOB_FILENAME = "jobs.json";
JOB_TASK_FILENAME = "jobs_with_task.json";
JOB_TASK_USAGE_FILENAME = "jobs_with_task_usage.json";
TRACE_DIR = "/newdir/google_trace";
TASK_USAGE = "task_usage";
MACHINE_USAGE = "machine_usage";
JOB_EVENTS = "job_events";
TASK_EVENTS = "task_events";
file_type = {};

class task_usage(Enum):
	START_TIME = 0
	END_TIME = 1
	JOB_ID = 2
	TASK_ID = 3
	MACHINE_ID = 4
	CPU_USAGE = 5
	MEMORY_USAGE = 6
	ASSIGNED_MEMORY = 7 
	FILE_PAGE_CACHE = 8
	TOTAL_PAGE_CACHE = 9
	MAX_MEMORY_USAGE = 10
	DISK_IO_TIME = 11
	DISK_USAGE = 12

class job_event(Enum):
	TIME = 0
	JOB_ID = 2
	EVENT_TYPE = 3
	CLASS = 5

class task_event(Enum):
	TIME = 0
	JOB_ID = 2
	TASK_ID = 3
	MACHINE_ID = 4
	EVENT_TYPE = 5
	CLASS = 7
	PRIORITY = 8
	CPU_REQ = 9
	MEM_REQ = 10
	DISK_REQ = 11

event_type = {0: 'SUBMIT', 1 : 'SCHEDULE', 2: 'EVICT', 3: 'FAIL', 4: 'FINISH', 5: 'KILL', 6: 'LOST', 7: 'UPDATE_PENDING', 8: 'UPDATE_RUNNING'}

def usage():
	print("python3 %s -p <path_where_to_download_the_trace> -l <how_many_files_to_download>" % (sys.argv[0]));

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

def write_finished_jobs(filepath, data):
	temp_dict = {}
	for job in data:
		if event_type[1] in data[job] and event_type[4] in data[job]:
			#print(data[job]);
			temp_dict[job] = data[job];
	with open(filepath, "w" ) as f:
		json.dump(temp_dict, f);
	return;

def availability(record_limit):
	files = sorted(get_file_names(TASK_USAGE));
	usage_count = 0;
	time_count = 0;
	cluster = {};

	fieldnames = ['time', 'machines', 'cpu', 'avg cpu', 'assigned mem', 'avg assigned mem', 'file cache', 'avg file cache', 'total cache', 'avg total cache', 'max mem', 'avg max mem', 'disk', 'avg disk'];

	with open('usage.csv', mode='w') as csv_file:
		writer = csv.DictWriter(csv_file, fieldnames=fieldnames);
		writer.writeheader();

		for filename in files:
			print("reading data from %s" % filename);
			file_path = path.join(path.join(TRACE_DIR, TASK_USAGE), filename);
			with open(file_path) as csv_file:
				csv_reader = csv.reader(csv_file, delimiter=' ');
				for row in csv_reader:
					#print(row);
					splits = row[0].split(",");
					time = splits[task_usage.START_TIME.value];

					if time not in cluster:
						if  time_count > 0:
							for t in cluster:
								machine_count = 0;
								row = {"cpu" : 0.0, "assigned mem": 0.0, "file cache": 0.0, "total cache": 0.0, "max mem": 0.0, "disk": 0.0};
								for machineid in cluster[t]:
									machine_count = machine_count + 1;
									for key in cluster[t][machineid]:
										row[key] = row[key] + (float) (cluster[t][machineid][key]);
								
								keys = (list)(row.keys());
								for key in keys:
									new_key = "avg " + key;
									row[new_key] = row[key]/machine_count;

								row["machines"] = machineid;
								row["time"] = (int) (t);
								writer.writerow(row);

							cluster = {};
						
						cluster[time] = {};
						time_count = time_count + 1;

					time = splits[task_usage.START_TIME.value];
					jobid = splits[task_usage.JOB_ID.value];
					taskid = splits[task_usage.TASK_ID.value];
					machineid = splits[task_usage.MACHINE_ID.value];

					if machineid not in cluster[time]:
						cluster[time][machineid] = {"cpu" : 0.0, "assigned mem": 0.0, "file cache": 0.0, "total cache": 0.0, "max mem": 0.0, "disk": 0.0};

					cluster[time][machineid]["cpu"] = cluster[time][machineid]["cpu"] + (float) (splits[task_usage.CPU_USAGE.value]);
					cluster[time][machineid]["assigned mem"] = cluster[time][machineid]["assigned mem"] + (float) (splits[task_usage.ASSIGNED_MEMORY.value]);
					cluster[time][machineid]["file cache"] = cluster[time][machineid]["file cache"] + (float) (splits[task_usage.FILE_PAGE_CACHE.value]);
					cluster[time][machineid]["total cache"] = cluster[time][machineid]["total cache"] + (float) (splits[task_usage.TOTAL_PAGE_CACHE.value]);
					cluster[time][machineid]["max mem"] = cluster[time][machineid]["max mem"] + (float) (splits[task_usage.MAX_MEMORY_USAGE.value]);
					cluster[time][machineid]["disk"] = cluster[time][machineid]["disk"] + (float) (splits[task_usage.DISK_USAGE.value]);

					usage_count = usage_count + 1;

					if record_limit != -1 and time_count > record_limit:
						break;
			
			if record_limit != -1 and usage_count > record_limit:
				break;
	return;

def task_wait_time(record_limit):
	files = sorted(get_file_names(TASK_USAGE));
	job_filename = path.join(path.join(TRACE_DIR, JOB_EVENTS), JOB_TASK_FILENAME);
	jobs = {};
	task_count = 0;
	wait_time = 0.0;
	wait_times = [];

	with open(job_filename) as job_file:
		jobs = json.load(job_file);

	for job_id in jobs:
		if "tasks" not in jobs[job_id]:
			continue;
		for task_id in jobs[job_id]["tasks"]:
			if "SUBMIT" in jobs[job_id]["tasks"][task_id] and "SCHEDULE" in jobs[job_id]["tasks"][task_id]:
				_wait = (int) (jobs[job_id]["tasks"][task_id]["SCHEDULE"]["time"]) - (int) (jobs[job_id]["tasks"][task_id]["SUBMIT"]["time"]);
				task_count = task_count + 1;
				wait_time = wait_time + _wait;
				wait_times.append(_wait);
				if record_limit != -1 and task_count >= record_limit:
					print("for {0} tasks, wait time : {1}, avg: {2}".format(task_count, wait_time, (wait_time*1.0)/task_count));
					return;

	print("for {0} tasks, wait time : {1}, avg: {2}".format(task_count, wait_time, (wait_time*1.0)/task_count));
	wait_times = sorted(wait_times);
	length = len(wait_times);
	fivth = (int)(length*0.95);
	nineth = (int)(length*0.99);

	print("95th, 99th", wait_times[fivth], wait_times[nineth]);
	return;

def machine_usage(time_limit):
	files = sorted(get_file_names(TASK_USAGE));
	usage_count = 0;
	start_time = 0;
	raw_time = 0;
	cluster = {};

	fieldnames = ['time', 'machine', 'cpu', 'assigned mem', 'file cache', 'total cache', 'max mem', 'total mem', 'disk'];

	for filename in files:
			print("reading data from %s" % filename);
			file_path = path.join(path.join(TRACE_DIR, TASK_USAGE), filename);
			with open(file_path) as csv_file:
				csv_reader = csv.reader(csv_file, delimiter=' ');
				for row in csv_reader:
					#print(row);
					splits = row[0].split(",");
					raw_time = (int)(splits[task_usage.START_TIME.value]);
					time = (int)((int)(splits[task_usage.START_TIME.value])/(60*1000000));
					jobid = splits[task_usage.JOB_ID.value];
					taskid = splits[task_usage.TASK_ID.value];
					machineid = splits[task_usage.MACHINE_ID.value];

					if start_time == 0 :
						start_time = raw_time;
						print("start time: {0}".format(start_time));

					if machineid not in cluster:
						cluster[machineid] = {};

					if time not in cluster[machineid]:
						cluster[machineid][time] = {"cpu" : 0.0, "assigned mem": 0.0, "file cache": 0.0, "total cache": 0.0, "max mem": 0.0, "disk": 0.0, "total mem": 0.0};

					cluster[machineid][time]["cpu"] = cluster[machineid][time]["cpu"] + (float) (splits[task_usage.CPU_USAGE.value]);
					cluster[machineid][time]["assigned mem"] = cluster[machineid][time]["assigned mem"] + (float) (splits[task_usage.ASSIGNED_MEMORY.value]);
					cluster[machineid][time]["file cache"] = cluster[machineid][time]["file cache"] + (float) (splits[task_usage.FILE_PAGE_CACHE.value]);
					cluster[machineid][time]["total cache"] = cluster[machineid][time]["total cache"] + (float) (splits[task_usage.TOTAL_PAGE_CACHE.value]);
					cluster[machineid][time]["max mem"] = cluster[machineid][time]["max mem"] + (float) (splits[task_usage.MAX_MEMORY_USAGE.value]);
					cluster[machineid][time]["disk"] = cluster[machineid][time]["disk"] + (float) (splits[task_usage.DISK_USAGE.value]);
					cluster[machineid][time]["total mem"] = cluster[machineid][time]["total mem"] + (float) (splits[task_usage.ASSIGNED_MEMORY.value]) + (float) (splits[task_usage.TOTAL_PAGE_CACHE.value]);

					usage_count = usage_count + 1;

					if usage_count % 100000 == 0:
						print("read {0} rows, actual time {1}, elapsed {2}".format(usage_count, raw_time, (raw_time-start_time)/1000000));

					if time_limit != -1 and (raw_time - start_time)/1000000 > time_limit :
						break;
			
			if time_limit != -1 and (raw_time - start_time)/1000000 > time_limit:
				break;

	print("read {0} rows, time : {1}, #machine: {2}".format(usage_count, raw_time, len(cluster)));
	
	machine_usage_row = {};
	for machineid in cluster:
		machine_filepath = path.join(path.join(TRACE_DIR, MACHINE_USAGE), "machine_usage_" + machineid + '.csv' );
		machine_usage_row[machineid] = 0;
		with open(machine_filepath, mode='w') as csv_file:
			writer = csv.DictWriter(csv_file, fieldnames=fieldnames);
			writer.writeheader();

			row = {};

			for t in cluster[machineid]:
				for key in cluster[machineid][t]:
					row[key] = cluster[machineid][t][key];
				
				row["machine"] = machineid;
				writer.writerow(row);
				machine_usage_row[machineid] = machine_usage_row[machineid] + 1;

	with open(path.join(path.join(TRACE_DIR, MACHINE_USAGE), "stat.json"), "w" ) as f:
		machine_stat = [];
		for k, v in sorted(machine_usage_row.items(), key=lambda item: item[1], reverse=True):
			machine_stat.append((k,v));
		json.dump(machine_stat, f, indent=4);	
						
	return;


def main():
	myargvs = process_argv(sys.argv);

	file_count = 10;
	global TRACE_DIR;

	if('-h' in myargvs):
		usage();
		return;
	if('-p' in myargvs):
		TRACE_DIR = myargvs['-p'];
	if('-l' in myargvs):
		file_count = (int)(myargvs['-l']);
	if('-r' in myargvs):
		record_limit = (int)(myargvs['-r']);

#	task_wait_time(record_limit);
#	availability(record_limit);
	machine_usage(record_limit);
if __name__ == "__main__":
	main();
