#include <vector>
#include <mutex>
#include <atomic>
#include <iostream>
#include <fstream>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sys/mman.h>

#define MAX_HARVESTED_LEN 64
#define CGROUP_PATH_MAX_LEN 256
#define PAGE_SHIFT 12

using namespace std;

/*
 * Global Variables
 */

long g_total_memory;
long g_est_available_memory;
mutex g_est_available_memory_lock;

long g_harvested_memory;
mutex g_harvested_lock;

const char *g_cgroup_name;
const char *g_file_path;
int g_fd;

/*
 * Constants
 */

constexpr float g_ewma_beta = 0.2;
constexpr long g_alloc_threshold = (8l << 30);
constexpr long g_evict_threshold = (1l << 30);
constexpr int g_sleep_time = 1;
constexpr long g_node_size = (64l << 20);

/*
 * Helper Functions
 */

long get_total_memory_size()
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

long get_cgroup_rss()
{
	char cgroup_path[CGROUP_PATH_MAX_LEN];
	sprintf(cgroup_path, "/sys/fs/cgroup/memory/%s/memory.stat", g_cgroup_name);

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

long get_tswap_memory_size()
{
	ifstream in("/sys/kernel/tswap/tswap_stat");
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

long get_available_memory()
{
	g_harvested_lock.lock();
	long harvested_memory = g_harvested_memory;
	g_harvested_lock.unlock();
	return g_total_memory - get_cgroup_rss() - get_tswap_memory_size() - harvested_memory;
}

long atomic_update_est_available_memory(long available_memory)
{
	long cur_est_available_memory;
	g_est_available_memory_lock.lock();
	g_est_available_memory = (long) (g_ewma_beta * available_memory
	                                 + (1 - g_ewma_beta) * g_est_available_memory);
	cur_est_available_memory = min(g_est_available_memory, available_memory);
	g_est_available_memory_lock.unlock();
	return cur_est_available_memory;
}

int file_write_lock(int fd)
{
	struct flock fl;

	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	fl.l_pid = 0;

	return fcntl(fd, F_SETLKW, &fl);
}

int file_write_unlock(int fd)
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

void atomic_update_file(int fd, long data)
{
	int ret = file_write_lock(fd);
	if (ret < 0) {
		cout << "cannot lock harvested size file" << endl;
		exit(1);
	}

	char buffer[MAX_HARVESTED_LEN];
	int data_len = sprintf(buffer, "%ld ", data);
	lseek(fd, 0, SEEK_SET);
	int cnt = write(fd, buffer, data_len);
	if (cnt <= 0) {
		cout << "cannot write to harvested size file" << endl;
		exit(1);
	}
	file_write_unlock(fd);
}

/*
 * Main Function
 */

int main(int argc, char *argv[])
{
	if (argc != 3) {
		cout << "Usage: <cgroup name> <harvested size file path>" << endl;
		exit(1);
	}
	g_cgroup_name = argv[1];
	g_file_path = argv[2];
	g_est_available_memory = 0;
	g_total_memory = get_total_memory_size();
	g_harvested_memory = 0;

	g_fd = open(g_file_path, O_WRONLY | O_CREAT, 00777);
	if (g_fd < 0) {
		cout << "cannot open harvested size file" << endl;
		exit(1);
	}
	atomic_update_file(g_fd, g_harvested_memory);

	for (this_thread::sleep_for(chrono::seconds(g_sleep_time));;
	     this_thread::sleep_for(chrono::seconds(g_sleep_time))) {
		long available_memory = get_available_memory();
		long cur_est_available_memory = atomic_update_est_available_memory(available_memory);
		if (cur_est_available_memory < g_evict_threshold) {
			long diff = g_evict_threshold - cur_est_available_memory;
			int evict_count = 0;

			g_harvested_lock.lock();
			while (diff > 0 && g_harvested_memory > 0) {
				g_harvested_memory -= g_node_size;
				++evict_count;
			}
			atomic_update_file(g_fd, g_harvested_memory);
			int remain_count = g_harvested_memory / g_node_size;
			g_harvested_lock.unlock();

			cout << "EVICT | available memory: " << (available_memory >> 20) << " MB, "
			     << "estimated available memory: " << (cur_est_available_memory >> 20) << " MB, "
			     << "allocated memory: " << ((remain_count * g_node_size) >> 20) << " MB, "
			     << "evicted: " << ((evict_count * g_node_size) >> 20) << " MB" << endl;
		} else if (cur_est_available_memory > g_alloc_threshold) {
			g_harvested_lock.lock();
			g_harvested_memory += g_node_size;
			int remain_count = g_harvested_memory / g_node_size;
			atomic_update_file(g_fd, g_harvested_memory);
			g_harvested_lock.unlock();

			cout << "ALLOC | available memory: " << (available_memory >> 20) << " MB, "
			     << "estimated available memory: " << (cur_est_available_memory >> 20) << " MB, "
			     << "allocated memory: " << ((remain_count * g_node_size) >> 20) << " MB" << endl;
		} else {
			g_harvested_lock.lock();
			int remain_count = g_harvested_memory / g_node_size;
			g_harvested_lock.unlock();

			cout << "SKIP  | available memory: " << (available_memory >> 20) << " MB, "
			     << "estimated available memory: " << (cur_est_available_memory >> 20) << " MB, "
			     << "allocated memory: " << ((remain_count * g_node_size) >> 20) << " MB" << endl;
		}
	}
}
