import sys;
import os;
import csv;
import json;
from os import path;
from enum import Enum;

JOB_FILENAME = "jobs.json";
JOB_TASK_FILENAME = "jobs_with_task.json";
TRACE_DIR = "/newdir/google_trace";
JOB_EVENTS = "job_events";

event_type = {0: 'SUBMIT', 1 : 'SCHEDULE', 2: 'EVICT', 3: 'FAIL', 4: 'FINISH', 5: 'KILL', 6: 'LOST', 7: 'UPDATE_PENDING', 8: 'UPDATE_RUNNING'}

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

def get_tasks(record_limit):
	job_filename = path.join(path.join(TRACE_DIR, JOB_EVENTS), JOB_TASK_FILENAME);
	tasks = [];
	latency_class = {};
	machines = {};
	task_count = 0;

	with open(job_filename) as job_file:
		jobs = json.load(job_file);

	for job_id in jobs:
		if "tasks" not in jobs[job_id]:
			continue;
		total_duration = 0;
		for task_id in sorted(jobs[job_id]["tasks"].keys()):
			if "SUBMIT" in jobs[job_id]["tasks"][task_id] and ("FINISH" in jobs[job_id]["tasks"][task_id] or "KILL" in jobs[job_id]["tasks"][task_id] or "LOST" in jobs[job_id]["tasks"][task_id]):
				if "FINISH" in jobs[job_id]["tasks"][task_id]:
					duration = abs((int) (jobs[job_id]["tasks"][task_id]["FINISH"]["time"]) - (int) (jobs[job_id]["tasks"][task_id]["SUBMIT"]["time"]));
				elif "KILL" in jobs[job_id]["tasks"][task_id]:
					duration = abs((int) (jobs[job_id]["tasks"][task_id]["KILL"]["time"]) - (int) (jobs[job_id]["tasks"][task_id]["SUBMIT"]["time"]));
				elif "LOST" in jobs[job_id]["tasks"][task_id]:
					duration = abs((int) (jobs[job_id]["tasks"][task_id]["LOST"]["time"]) - (int) (jobs[job_id]["tasks"][task_id]["SUBMIT"]["time"]));

				task = {
					"j_id" : job_id,
					"t_id": task_id, 
					"submit": total_duration, 
					"duration": duration, 
					"memory": (float)(jobs[job_id]["tasks"][task_id]["SUBMIT"]["mem"]), 
					"priority": (int)(jobs[job_id]["tasks"][task_id]["SUBMIT"]["priority"])
				};

				total_duration = total_duration + duration;

				_class = (int) (jobs[job_id]["tasks"][task_id]["SUBMIT"]["class"]);

				if _class not in latency_class:
					latency_class[_class] = 0;

				latency_class[_class] = latency_class[_class] + 1;
				total_duration = total_duration + duration;
				machines[jobs[job_id]["tasks"][task_id]["SUBMIT"]["machine"]] = 1;
				tasks.append(task);
				task_count = task_count + 1;

				if record_limit != -1 and task_count >= record_limit:
					break;
		
		if record_limit != -1 and task_count >= record_limit:
			break;

	tasks.sort(key=lambda x: x["submit"], reverse=False);
	return tasks;

def setup_machine(machine_count, start_time):
	machines = {};
	for i in range(0, machine_count):
		machines[i] = {start_time:0.0};
	return machines;

def get_available_machines(machines, memory_req, current_time):
	machine_list = [];
	for _id in machines:
		max_t = 0;
		max_t_usage = 0;
		found = 0;
		for t in sorted(machines[_id].keys()):
			if t >  max_t :
				max_t = t;
				max_t_usage = machines[_id][t];
			if t >= current_time and memory_req <= (1 - machines[_id][t]):
				machine_list.append({"id": _id, "available_time": t, "usage": machines[_id][t]});
				found = 1;
				break;
		if found == 0 and max_t < current_time and memory_req <= (1 - max_t_usage):
			machine_list.append({"id": _id, "available_time": current_time, "usage": max_t_usage});

	while len(machine_list) == 0 :
		machine_list = get_available_machines(machines, memory_req, current_time + 10000000);

	machine_list.sort(key=lambda x: x["available_time"], reverse=False);
	return machine_list;

def add_task_to_machine(machine, task):
	mem_usage = 0.0;
	prev_mem_usage = 0.0;
	found = 0;
	start_time = (int) (task["submit"]);
	end_time = (int) (task["submit"] + task["duration"]);
	max_time = 0;
	min_time = 0;
	times = (list) (sorted(machine.keys()));
#	print(times);
#	print(task);
#	print("--------");
	for key in times:
		time = (int) (key);

		if time > max_time and time < end_time:
			max_time = time;

		if time > min_time and time < start_time:
			min_time = time;

		if time >= start_time and time <= end_time:
			machine[time] = machine[time] + task["memory"];

			if time == start_time and time != 0:
				machine[time - 1] = machine[min_time];
			found = 1;

	if found == 0:
		machine[start_time] = machine[min_time] + task["memory"];
	if max_time > start_time:
		machine[end_time+1] = machine[max_time] - task["memory"];
	else:
		machine[end_time+1] = machine[max_time];
	return machine;

def duration_dist(task_count):
	duration_map = {};
	tasks = get_tasks(task_count);
	tasks.sort(key=lambda x: x["duration"], reverse=False);

	for task in tasks:
		_bin = (int)(task["duration"] / 10000000);
		if _bin not in duration_map:
			duration_map[_bin] = 0;

		duration_map[_bin] = duration_map[_bin] + 1;

	total_task = len(tasks);
	fieldnames = ['duration', 'count', 'normalized'];
	with open('duration_dist.csv', mode='w') as csv_file:
		writer = csv.DictWriter(csv_file, fieldnames=fieldnames);
		writer.writeheader();

		for _bin in sorted(duration_map.keys()):
			row = {"duration": _bin, "count": duration_map[_bin], "normalized":(duration_map[_bin]*1.0)/total_task};
			writer.writerow(row);

def run_simulator(task_count, machine_count):
	tasks = get_tasks(task_count);
	machines = setup_machine(machine_count, 0);
	wait_time = 0;
	total_wait_time = 0;
	time = 0;
	task_count = 0;
	wait_times = [];

	# print(tasks);

	for task in tasks:
		time = task["submit"];
		candidate_machines = get_available_machines(machines, task["memory"], time);
		#machines[candidate_machines[0]["id"]][candidate_machines[0]["available_time"]] = candidate_machines[0]["usage"];
		selected_machine = machines[candidate_machines[0]["id"]];
		# print(time, task["duration"], candidate_machines[0]["id"]);
		wait_time = (candidate_machines[0]["available_time"] - task["submit"]);
		wait_times.append(wait_time);
		total_wait_time = total_wait_time + wait_time;
		task["submit"] = candidate_machines[0]["available_time"];
		selected_machine = add_task_to_machine(selected_machine, task);
		machines[candidate_machines[0]["id"]] = selected_machine;
		#print(task, selected_machine);
		task_count = task_count + 1;

		if task_count % 10000 == 0:
			print("avg wait time", task_count, (total_wait_time*1.0)/task_count);
			wait_times = sorted(wait_times);
			length = len(wait_times);
			fivth = (int) (length*0.95);
			nineth = (int) (length*0.99);
			print("95th, 99th ", wait_times[fivth], wait_times[nineth]);

	print(total_wait_time, task_count, (total_wait_time*1.0)/task_count);

def main():
	myargvs = process_argv(sys.argv);

	global TRACE_DIR;

	if('-h' in myargvs):
		usage();
		return;
	if('-p' in myargvs):
		TRACE_DIR = myargvs['-p'];
	if('-r' in myargvs):
		record_limit = (int)(myargvs['-r']);

#	run_simulator(record_limit, 54);
	duration_dist(record_limit);
if __name__ == "__main__":
	main();
