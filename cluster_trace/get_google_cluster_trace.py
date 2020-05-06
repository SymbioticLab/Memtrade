import sys;
import os;
import csv;
import wget;
import json;
from os import path;
from sh import gunzip
from enum import Enum;

BASE_URL = "https://commondatastorage.googleapis.com/clusterdata-2011-2/"
SHA_FILE_NAME = "SHA256SUM";
JOB_FILENAME = "jobs.json";
JOB_TASK_FILENAME = "jobs_with_task.json";
JOB_TASK_USAGE_FILENAME = "jobs_with_task_usage.json";
TRACE_DIR = "./";
TASK_USAGE = "task_usage";
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

def download_file(file_path, file_name):
	url = BASE_URL+file_name;
	filename = path.join(file_path, file_name)
	if not path.exists(filename):
		print(file_name);
		wget.download(url, file_path);
		print("\n");

def print_trace_info():
	for item in file_type:
		print(item, file_type[item]);

def download_trace(file_count):	
	download_file(TRACE_DIR, SHA_FILE_NAME);
	
	with open(TRACE_DIR+SHA_FILE_NAME) as csv_file:
		csv_reader = csv.reader(csv_file, delimiter=' ');
		line_count = 0;
		for row in csv_reader:
			trace_file_name = row[1];
			trace_file_name = trace_file_name.replace("*", "");
			splits = trace_file_name.split("/");
			if len(splits) > 0 :
				_type = splits[0];
				if _type not in file_type:
					file_type[_type] = 0;
					try:
						os.mkdir(path.join(TRACE_DIR,_type));
					except OSError as error:
						print("path %s already exists" % (path.join(TRACE_DIR,_type)));
				file_type[_type] = file_type[_type] + 1;
			
			download_file(path.join(TRACE_DIR,_type), trace_file_name);
			if trace_file_name.find(".gz") != -1:
				gunzip(path.join(TRACE_DIR,trace_file_name));
			line_count = line_count + 1;
			if file_count != 0 and line_count >= file_count :
				break;
	print_trace_info();
	return;

def get_file_names(trace_type):
	files = os.listdir(path.join(TRACE_DIR, trace_type));
	return files;

def process_task_resource_usage_data(filename, record_limit):
	data = {};
	print(filename);
	with open(filename) as csv_file:
		csv_reader = csv.reader(csv_file, delimiter=' ');
		line_count = 0;
		for row in csv_reader:
#			print(row[0]);
			splits = row[0].split(",");
			tid = splits[task_usage.JOB_ID.value]+'-'+splits[task_usage.TASK_ID.value];
			if tid not in data:
				data[tid] = [];
			entry = [];
			for idx in task_usage:
				entry.append(splits[idx.value]);
			data[tid].append(entry);
			line_count = line_count + 1;	
			if line_count >= record_limit:
				break;
	return data;

def get_task_resource_usage(record_limit):
	files = sorted(get_file_names(TASK_USAGE));
	limit = record_limit;
	data = {};

	for filename in files:
		print("reading data from %s" % filename);
		file_path = path.join(path.join(TRACE_DIR, TASK_USAGE), filename);
		task_events = process_task_resource_usage_data(file_path, limit);
		record_count = 0;
		for tid in task_events:
			if tid not in data:
				data[tid] = [];
			data[tid].extend(task_events[tid]);
			record_count = record_count + len(task_events[tid]);

		if limit > record_count:
			limit = limit - record_count;
		else:
			break;

#	print(data);
	print("Task Id\t\tStart\t\tEnd\t\tMachine Id\t\tMemory Use\tMemory Assign");
	for k in sorted(data, key=lambda k: len(data[k]), reverse=True):
		for items in data[k]:
			print("{0}\t{1}\t{2}\t{3}\t\t{4}\t\t{5}".format(k,items[task_usage.START_TIME.value], items[task_usage.END_TIME.value], items[task_usage.MACHINE_ID.value], items[task_usage.MEMORY_USAGE.value], items[task_usage.ASSIGNED_MEMORY.value]))
		#break;

def write_finished_jobs(filepath, data):
	temp_dict = {}
	for job in data:
		if event_type[1] in data[job] and event_type[4] in data[job]:
			#print(data[job]);
			temp_dict[job] = data[job];
	with open(filepath, "w" ) as f:
		json.dump(temp_dict, f);
	return;

def parse_jobs(record_limit):
	files = sorted(get_file_names(JOB_EVENTS));
	data = {};
	job_count = 0;
	row_count = 0;

	for filename in files:
		print("reading data from %s" % filename);
		file_path = path.join(path.join(TRACE_DIR, JOB_EVENTS), filename);
		with open(file_path) as csv_file:
			csv_reader = csv.reader(csv_file, delimiter=' ');
			for row in csv_reader:
				#print(row);
				row_count = row_count + 1;
				splits = row[0].split(",");
				jobid = splits[job_event.JOB_ID.value];
				event = event_type[(int)(splits[job_event.EVENT_TYPE.value])];
				if jobid not in data:
					data[jobid] = {'id': jobid, 'class': splits[job_event.CLASS.value]};

				data[jobid][event] = splits[job_event.TIME.value];

				if event == event_type[4] and event_type[1] in data[jobid]:
					job_count = job_count + 1;
				if record_limit != -1 and job_count >= record_limit:
					write_finished_jobs(path.join(path.join(TRACE_DIR, JOB_EVENTS), JOB_FILENAME), data);
					print("after reading {0} rows, found {1} complete jobs".format(row_count, job_count));
					return;

	write_finished_jobs(path.join(path.join(TRACE_DIR, JOB_EVENTS), JOB_FILENAME), data);
	print("after reading {0} rows, found {1} complete jobs".format(row_count, job_count));

def parse_tasks(record_limit):
	files = sorted(get_file_names(TASK_EVENTS));
	job_filename = path.join(path.join(TRACE_DIR, JOB_EVENTS), JOB_FILENAME);
	jobs = {};
	task_count = 0;
	row_count = 0;

	with open(job_filename) as job_file:
		jobs = json.load(job_file);

	for filename in files:
		print("reading data from %s" % filename);
		file_path = path.join(path.join(TRACE_DIR, TASK_EVENTS), filename);
		with open(file_path) as csv_file:
			csv_reader = csv.reader(csv_file, delimiter=' ');
			for row in csv_reader:
				#print(row);
				row_count = row_count + 1;
				splits = row[0].split(",");
				jobid = splits[task_event.JOB_ID.value];
				taskid = splits[task_event.TASK_ID.value];
				event = event_type[(int)(splits[task_event.EVENT_TYPE.value])];
				
				if jobid not in jobs:
					continue;

				if "tasks" not in jobs[jobid]:
					jobs[jobid]["tasks"] = {};

				if taskid not in jobs[jobid]["tasks"]:
					jobs[jobid]["tasks"][taskid] = {};

				jobs[jobid]["tasks"][taskid][event] = {
					"time": 	splits[task_event.TIME.value],
					"machine": 	splits[task_event.MACHINE_ID.value],
					"class": 	splits[task_event.CLASS.value],
					"priority":	splits[task_event.PRIORITY.value],
					"cpu":		splits[task_event.CPU_REQ.value],
					"mem":		splits[task_event.MEM_REQ.value],
					"disk":		splits[task_event.DISK_REQ.value],
				}

				task_count = task_count + 1;

				if record_limit != -1 and task_count >= record_limit:
					write_finished_jobs(path.join(path.join(TRACE_DIR, JOB_EVENTS), JOB_TASK_FILENAME), jobs);
					print("after reading {0} rows, for {1} complete jobs, {2} tasks are found".format(row_count, len(jobs.keys()), task_count));
					return;

	write_finished_jobs(path.join(path.join(TRACE_DIR, JOB_EVENTS), JOB_TASK_FILENAME), jobs);
	print("after reading {0} rows, for {1} complete jobs, {2} tasks are found".format(row_count, len(jobs.keys()), task_count));

def parse_task_usage(record_limit):
	files = sorted(get_file_names(TASK_USAGE));
	job_filename = path.join(path.join(TRACE_DIR, JOB_EVENTS), JOB_TASK_FILENAME);
	jobs = {};
	usage_count = 0;
	row_count = 0;

	with open(job_filename) as job_file:
		jobs = json.load(job_file);

	for filename in files:
		print("reading data from %s" % filename);
		file_path = path.join(path.join(TRACE_DIR, TASK_USAGE), filename);
		with open(file_path) as csv_file:
			csv_reader = csv.reader(csv_file, delimiter=' ');
			for row in csv_reader:
				#print(row);
				row_count = row_count + 1;
				splits = row[0].split(",");
				jobid = splits[task_usage.JOB_ID.value];
				taskid = splits[task_usage.TASK_ID.value];
				time = splits[task_usage.START_TIME.value];
				
				if jobid not in jobs:
					continue;

				if taskid not in jobs[jobid]["tasks"] :
					jobs[jobid]["tasks"][taskid] = {};

				if "usage" not in jobs[jobid]["tasks"][taskid]:
					jobs[jobid]["tasks"][taskid]["usage"] = {};

				jobs[jobid]["tasks"][taskid]["usage"][time] = {
					"start": 	time,
					"end":		splits[task_usage.END_TIME.value],
					"machine": 	splits[task_usage.MACHINE_ID.value],
					"cpu":		splits[task_usage.CPU_USAGE.value],
					"ass_mem":	splits[task_usage.ASSIGNED_MEMORY.value],
					"u_cache":	splits[task_usage.FILE_PAGE_CACHE.value],
					"t_cache":	splits[task_usage.TOTAL_PAGE_CACHE.value],
					"max_mem":	splits[task_usage.MAX_MEMORY_USAGE.value],
					"disk_io":	splits[task_usage.DISK_IO_TIME.value],
					"disk":		splits[task_usage.DISK_USAGE.value]
				}

				usage_count = usage_count + 1;

				if record_limit != -1 and usage_count >= record_limit:
					write_finished_jobs(path.join(path.join(TRACE_DIR, JOB_EVENTS), JOB_TASK_USAGE_FILENAME), jobs);
					print("after reading {0} rows, for {1} complete jobs, {2} usage are found".format(row_count, len(jobs.keys()), usage_count));
					return;

	write_finished_jobs(path.join(path.join(TRACE_DIR, JOB_EVENTS), JOB_TASK_USAGE_FILENAME), jobs);
	print("after reading {0} rows, for {1} complete jobs, {2} usage are found".format(row_count, len(jobs.keys()), usage_count));

def main():
	myargvs = process_argv(sys.argv);

	file_count = 10;
	trace_type = TASK_USAGE;
	global TRACE_DIR;

	if('-p' in myargvs):
		TRACE_DIR = myargvs['-p'];
	if('-l' in myargvs):
		file_count = (int)(myargvs['-l']);
	if('-h' in myargvs):
		usage();
		return;
	if('-t' in myargvs):
		trace_type = myargvs['-t'];
	if('-r' in myargvs):
		record_limit = (int)(myargvs['-r']);

#	get_file_names(trace_dir, trace_type);

#	download_trace(TRACE_DIR, file_count);

#	get_task_resource_usage(record_limit);
#	parse_jobs(record_limit);
#	parse_tasks(record_limit);
	parse_task_usage(record_limit);
if __name__ == "__main__":
	main();
