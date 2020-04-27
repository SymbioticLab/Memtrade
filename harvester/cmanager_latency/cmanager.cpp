#include <iostream>
#include <fstream>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <fcntl.h>
#include <unistd.h>

#define CGROUP_PATH_MAX_LEN 256
#define MAX_PERFORMANCE_LEN 256
#define PAGE_SHIFT 12

using namespace std;

/*
 * Data Structures
 */

struct latency_point {
	double latency;
	long epoch;
};

// TODO: handle epoch overflow

/*
 * Global Variables
 */

const char *g_cgroup_name;
const char *g_latency_file_path;
const char *g_logging_file_path;
const char *g_tswap_stat_path;
long g_physical_memory_size;

atomic_long g_epoch;

mutex g_cgroup_limit_lock;
long g_cgroup_limit;

mutex g_moving_min_lock;
deque<double> g_recent_latency_queue;
double g_sum_recent_latency;
deque<latency_point> g_moving_min_queue;

int g_latency_fd;
mutex g_latency_lock;

mutex g_bottom_line_lock;
long g_bottom_line;
chrono::time_point<chrono::system_clock> g_bottom_line_timestamp;

/*
 * Constants
 */

constexpr long g_unit_size = (64 << 20);
constexpr long g_min_cgroup_limit = (1 << 30);

constexpr float g_latency_increase_threshold = 1.2;
constexpr float g_latency_increase_prefetch_threshold = 10;
constexpr long g_prefetch_size = (1 << 27);

constexpr long g_ad = g_unit_size;
constexpr float g_md_threshold = 1.2;
constexpr float g_md = 0.95;

constexpr float g_mi_rss_threshold = 2 * g_ad;
constexpr float g_mi = 1.2;

constexpr long g_dec_sleep_time = 5;
constexpr long g_warrior_sleep_time = 1;
constexpr long g_logging_sleep_time = 1;

constexpr long g_moving_min_window_size = 800;
constexpr long g_min_latency_smooth_window_size = 4;
constexpr long g_bottom_line_expire_time = 180;
constexpr long g_overflow_threshold = g_unit_size;

/*
 * Helper Functions
 */

long get_memory_size()
{
	ifstream in("/proc/meminfo");
	if (!in) {
		cout << "cannot open meminfo file" << endl;
		exit(1);
	}
	string key;
	long value;
	string unit;
	while (in >> key >> value >> unit) {
		if (key == "MemTotal:") {
			return value << 10;
		}
	}
	cout << "cannot read meminfo file" << endl;
	exit(1);
}

bool set_cgroup_limit(const char *cgroup_name, long limit_size)
{
	char cgroup_limit_path[CGROUP_PATH_MAX_LEN];
	sprintf(cgroup_limit_path,
	        "/sys/fs/cgroup/memory/%s/memory.limit_in_bytes",
	        cgroup_name);

	ofstream limit_file;
	limit_file.open(cgroup_limit_path);
	if (!limit_file) {
		cout << "cannot open cgroup limit file" << endl;
		exit(1);
	}

	limit_file << limit_size << endl;

	return !!limit_file;
}

int file_read_lock(int fd)
{
	struct flock fl;

	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_RDLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	fl.l_pid = 0;

	return fcntl(fd, F_SETLKW, &fl);
}

int file_read_unlock(int fd)
{
	struct flock fl;

	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_UNLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	fl.l_pid = 0;

	return fcntl(fd, F_SETLK, &fl);
}

double atomic_read_latency(int fd)
{
	g_latency_lock.lock();
	int ret = file_read_lock(fd);
	if (ret < 0) {
		cout << "cannot lock latency file" << endl;
		exit(1);
	}

	char buffer[MAX_PERFORMANCE_LEN];
	lseek(fd, 0, SEEK_SET);
	int cnt = read(fd, buffer, MAX_PERFORMANCE_LEN);
	if (cnt <= 0) {
		cout << "cannot read latency file" << endl;
		exit(1);
	}

	double latency;
	sscanf(buffer, "%lf", &latency);

	file_read_unlock(fd);
	g_latency_lock.unlock();
	return latency;
}

double atomic_get_min_latency()
{
	g_moving_min_lock.lock();
	double min_latency;
	if (g_moving_min_queue.empty()) {
		min_latency = DBL_MAX;
	} else {
		min_latency = g_moving_min_queue.front().latency;
	}
	g_moving_min_lock.unlock();
	return min_latency;
}

void atomic_update_min_latency(long epoch, double latency)
{
	g_moving_min_lock.lock();

	/* smooth latency first */
	g_recent_latency_queue.push_back(latency);
	g_sum_recent_latency += latency;
	if (g_recent_latency_queue.size() > g_min_latency_smooth_window_size) {
		g_sum_recent_latency -= g_recent_latency_queue.front();
		g_recent_latency_queue.pop_front();
	}

	double smoothed_latency = g_sum_recent_latency / g_recent_latency_queue.size();

	while (!g_moving_min_queue.empty()
	       && (g_moving_min_queue.front().epoch <= epoch - g_moving_min_window_size
	           || g_moving_min_queue.front().epoch + 1 < g_min_latency_smooth_window_size)) {
		g_moving_min_queue.pop_front();
	}
	while (!g_moving_min_queue.empty()
	       && g_moving_min_queue.back().latency > smoothed_latency) {
		g_moving_min_queue.pop_back();
	}
	struct latency_point point;
	point.epoch = epoch;
	point.latency = smoothed_latency;
	g_moving_min_queue.push_back(point);
	g_moving_min_lock.unlock();
}

void atomic_set_min_latency(long epoch, double latency)
{
	g_moving_min_lock.lock();
	g_moving_min_queue.clear();
	struct latency_point point;
	point.epoch = epoch;
	point.latency = latency;
	g_moving_min_queue.push_back(point);
	g_moving_min_lock.unlock();
}

long get_cgroup_rss(const char *cgroup_name)
{
	char cgroup_path[CGROUP_PATH_MAX_LEN];
	sprintf(cgroup_path, "/sys/fs/cgroup/memory/%s/memory.stat", cgroup_name);

	ifstream in(cgroup_path);
	if (!in) {
		cout << "cannot open cgroup stat file" << endl;
		exit(1);
	}

	string key;
	long value;
	long rss = 0;
	while (in >> key >> value) {
		if (key == "rss" || key == "mapped_file") {
			rss += value;
		}
	}
	return rss;
}

long get_cgroup_swap(const char *cgroup_name)
{
	char cgroup_path[CGROUP_PATH_MAX_LEN];
	sprintf(cgroup_path, "/sys/fs/cgroup/memory/%s/memory.stat", cgroup_name);

	ifstream in(cgroup_path);
	if (!in) {
		cout << "cannot open cgroup stat file" << endl;
		exit(1);
	}

	string key;
	long value;
	while (in >> key >> value) {
		if (key == "swap") {
			return value;
		}
	}
	cout << "cannot read cgroup swap, please make sure that swap extension is enabled" << endl;
	exit(1);
}

long atomic_get_bottom_line()
{
	long bottom_line;
	g_bottom_line_lock.lock();
	if (g_bottom_line >= 0) {
		chrono::time_point<chrono::system_clock> now = chrono::system_clock::now();
		if (now > g_bottom_line_timestamp + chrono::seconds(g_bottom_line_expire_time)) {
			g_bottom_line = -1;
		}
	}
	bottom_line = g_bottom_line;
	g_bottom_line_lock.unlock();
	return bottom_line;
}

long atomic_update_bottom_line(long bottom_line)
{
	long new_bottom_line;

	g_bottom_line_lock.lock();
	g_bottom_line = max(g_bottom_line, bottom_line);
	new_bottom_line = g_bottom_line;
	g_bottom_line_timestamp = chrono::system_clock::now();
	g_bottom_line_lock.unlock();

	return new_bottom_line;
}

long get_tswap_memory_size(const char *tswap_stat_path)
{
	ifstream in(tswap_stat_path);
	if (!in) {
		cout << "cannot open tswap stat file" << endl;
		exit(1);
	}
	long nr_memory_page = 0;
	string key;
	long value;
	while (in >> key >> value) {
		if (key == "nr_zombie_page:"
		    || key == "nr_in_memory_page:"
		    || key == "nr_in_memory_zombie_page:"
		    || key == "nr_in_flight_page:") {
			nr_memory_page += value;
		}
	}
	return nr_memory_page << PAGE_SHIFT;
}

void tswap_prefetch()
{
	ofstream file("/sys/kernel/tswap/tswap_prefetch");
	if (!file) {
		cout << "cannot open tswap prefetch file" << endl;
		exit(1);
	}
	file << (g_prefetch_size >> PAGE_SHIFT) << endl;
}

/*
 * Warrior Thread Function
 */

void warrior_thread_fn()
{
	for (this_thread::sleep_for(chrono::seconds(g_warrior_sleep_time));;
	     this_thread::sleep_for(chrono::seconds(g_warrior_sleep_time))) {
		double latency = atomic_read_latency(g_latency_fd);
		double min_latency = atomic_get_min_latency();
		long rss = get_cgroup_rss(g_cgroup_name);
		long swap = (g_tswap_stat_path != nullptr) ? get_cgroup_swap(g_cgroup_name) : 0;
		long tswap_mem = (g_tswap_stat_path != nullptr) ? get_tswap_memory_size(g_tswap_stat_path) : 0;
		long bottom_line = atomic_get_bottom_line();

		if (latency > min_latency * g_latency_increase_prefetch_threshold
		    && g_tswap_stat_path != nullptr) {
			tswap_prefetch();
		}

		if (latency > min_latency * g_latency_increase_threshold
		    || (g_tswap_stat_path != nullptr && tswap_mem - swap > g_overflow_threshold)) {
			g_cgroup_limit_lock.lock();
			if (rss < g_cgroup_limit - g_mi_rss_threshold) {
				cout << "INC LOOP | latency: " << latency
				     << ", min latency: " << min_latency
				     << ", ratio: " << latency / min_latency
				     << ", cgroup limit: " << (g_cgroup_limit >> 20)
				     << " MB, rss: " << (rss >> 20)
				     << " MB, bottom line: " << (bottom_line >> 20)
				     << " MB, SKIP" << endl;

				g_cgroup_limit_lock.unlock();
				continue;
			}

			/* perform MI */
			g_cgroup_limit = min(g_physical_memory_size, (long) (g_mi * g_cgroup_limit));
			set_cgroup_limit(g_cgroup_name, g_cgroup_limit);

			cout << "INC LOOP | latency: " << latency
			     << ", min latency: " << min_latency
			     << ", ratio: " << latency / min_latency
			     << ", cgroup limit: " << (g_cgroup_limit >> 20)
			     << " MB, rss: " << (rss >> 20)
			     << " MB, bottom line: " << (bottom_line >> 20)
			     << " MB, MI" << endl;

			g_cgroup_limit_lock.unlock();
		}
	}
}

/*
 * Logging Thread Function
 */

void logging_thread_fn()
{
	ofstream logging_file;
	logging_file.open(g_logging_file_path);
	if (!logging_file) {
		cout << "cannot open logging file" << endl;
		exit(1);
	}

	for (this_thread::sleep_for(chrono::seconds(g_logging_sleep_time));;
	     this_thread::sleep_for(chrono::seconds(g_logging_sleep_time))) {
		double latency = atomic_read_latency(g_latency_fd);
		double min_latency = atomic_get_min_latency();
		long cgroup_limit = g_cgroup_limit;
		long rss = get_cgroup_rss(g_cgroup_name);
		long swap = get_cgroup_swap(g_cgroup_name);
		long bottom_line = atomic_get_bottom_line();

		logging_file << latency << ","
		             << min_latency << ","
		             << cgroup_limit << ","
		             << rss << ","
		             << swap << ","
		             << bottom_line;

		if (g_tswap_stat_path != nullptr) {
			long tswap_memory_size = get_tswap_memory_size(g_tswap_stat_path);
			logging_file << "," << tswap_memory_size;
		}

		logging_file << endl;
	}
}

/*
 * Main Function
 */

int main(int argc, char *argv[])
{
	/* loading input */
	if (argc != 5 && argc != 6) {
		cout << "usage: <cgroup name> <latency file path> "
		        "<initial cgroup size (MB)> <logging file path> "
		        "<tswap stat path (optional)>" << endl;
		return -1;
	}
	g_cgroup_name = argv[1];
	g_latency_file_path = argv[2];
	g_logging_file_path = argv[4];
	g_tswap_stat_path = (argc == 6) ? argv[5] : nullptr;

	/* initialization */
	g_physical_memory_size = get_memory_size();
	g_epoch.store(0);
	sscanf(argv[3], "%ld", &g_cgroup_limit);
	g_cgroup_limit <<= 20;
	g_cgroup_limit = min(g_physical_memory_size, g_cgroup_limit);
	set_cgroup_limit(g_cgroup_name, g_cgroup_limit);
	g_bottom_line = -1;
	g_sum_recent_latency = 0;

	/* get performance file */
	g_latency_fd = open(g_latency_file_path, O_RDONLY | O_CREAT, 00777);
	if (g_latency_fd < 0) {
		cout << "cannot open performance file" << endl;
		return -1;
	}

	/* start threads */
	thread warrior_thread = thread(warrior_thread_fn);
	thread logging_thread = thread(logging_thread_fn);

	/* start AD loop */
	for (this_thread::sleep_for(chrono::seconds(g_dec_sleep_time));;
	     this_thread::sleep_for(chrono::seconds(g_dec_sleep_time))) {
		/* update min latency */
		double latency = atomic_read_latency(g_latency_fd);
		long epoch = g_epoch.fetch_add(1) + 1;
		atomic_update_min_latency(epoch, latency);
		double min_latency = atomic_get_min_latency();
		long rss = get_cgroup_rss(g_cgroup_name);
		long bottom_line = atomic_get_bottom_line();
		long swap = (g_tswap_stat_path != nullptr) ? get_cgroup_swap(g_cgroup_name) : 0;
		long tswap_mem = (g_tswap_stat_path != nullptr) ? get_tswap_memory_size(g_tswap_stat_path) : 0;

		if (latency > min_latency * g_latency_increase_threshold
		    || (g_tswap_stat_path != nullptr && tswap_mem - swap > g_overflow_threshold)) {
			cout << "DEC LOOP | epoch: " << epoch
			     << ", latency: " << latency
			     << ", min latency: " << min_latency
			     << ", cgroup limit: " << (g_cgroup_limit >> 20)
			     << " MB, rss: " << (rss >> 20)
			     << " MB, bottom line: " << (bottom_line >> 20)
			     << " MB, SKIP" << endl;

			continue;
		}

		/* perform AD/MD */
		g_cgroup_limit_lock.lock();

		const char *op;
		long proposed_cgroup_limit;
		if (g_cgroup_limit > rss * g_md_threshold) {
			proposed_cgroup_limit = max(max(g_min_cgroup_limit, (long)(rss * g_md_threshold)),
			                            (long)(g_cgroup_limit * g_md));
			op = "MD";
		} else {
			proposed_cgroup_limit = max(g_min_cgroup_limit, g_cgroup_limit - g_ad);
			op = "AD";
		}

		if (bottom_line >= 0) {
			proposed_cgroup_limit = max(proposed_cgroup_limit, bottom_line);
			if (proposed_cgroup_limit >= g_cgroup_limit) {
				cout << "DEC LOOP | epoch: " << epoch
				     << ", latency: " << latency
				     << ", min latency: " << min_latency
				     << ", cgroup limit: " << (g_cgroup_limit >> 20)
				     << " MB, rss: " << (rss >> 20)
				     << " MB, bottom line: " << (bottom_line >> 20)
				     << " MB, SKIP" << endl;

				g_cgroup_limit_lock.unlock();
				continue;
			}
		}
		if (proposed_cgroup_limit < rss) {
			bottom_line = max(g_min_cgroup_limit, rss - g_ad);
			bottom_line = atomic_update_bottom_line(bottom_line);
			proposed_cgroup_limit = max(proposed_cgroup_limit, bottom_line);
		}

		g_cgroup_limit = proposed_cgroup_limit;
		cout << "DEC LOOP | epoch: " << epoch
		     << ", latency: " << latency
		     << ", min latency: " << min_latency
		     << ", cgroup limit: " << (g_cgroup_limit >> 20)
		     << " MB, rss: " << (rss >> 20)
		     << " MB, bottom line: "<< (bottom_line >> 20)
		     << " MB, " << op << endl;

		int ret = set_cgroup_limit(g_cgroup_name, g_cgroup_limit);
		if (!ret) {
			g_cgroup_limit = min(g_physical_memory_size, (long)(g_mi * g_cgroup_limit));
			set_cgroup_limit(g_cgroup_name, g_cgroup_limit);

			cout << "DEC LOOP | cgroup limit failed, MI, cgroup limit: "
			     << (g_cgroup_limit >> 20) << " MB, bottom line: "
			     << (bottom_line >> 20) << " MB" << endl;
		}
		g_cgroup_limit_lock.unlock();
	}
	return 0;
}
