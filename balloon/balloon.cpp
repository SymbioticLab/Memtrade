#include <iostream>
#include <fstream>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#include "balloon.h"

#define CGROUP_PATH_MAX_LEN 256
#define MAX_LINE_LEN 256

/*
 * Global Variables
 */
const char *g_cgroup_name;

atomic_long g_swap_size;
atomic_long g_grab_size_lower_bound;
atomic_long g_grab_size_upper_bound;

atomic_long g_cur_grab_size;
atomic_int g_cur_status;
long g_total_free_size;

atomic_long g_est_rss_diff;
atomic_long g_est_swap_margin;

vector<void *> g_node_vec;

mutex g_inflate_deflate_lock;
mutex g_swap_lock;
mutex g_force_deflate_lock;

mutex log_lock;

thread inflate_thread;
thread deflate_thread;
thread swap_out_thread;

thread sigterm_thread;
thread sigint_thread;
thread mem_pressure_thread;

thread curses_stat_thread;
thread curses_op_thread;

/*
 * Static Helper Functions
 */

static void init_global_variables()
{
	g_swap_size.store(0);
	g_grab_size_lower_bound.store(0);
	g_grab_size_upper_bound.store(0);

	g_cur_grab_size.store(0);
	g_cur_status.store(NORMAL);
	g_total_free_size = get_memory_size();

	g_est_rss_diff.store(g_init_rss_diff);
	g_est_swap_margin.store(g_init_swap_margin);
}

/*
 * Non-Static Helper Functions
 */

inline void block_signals()
{
	/*
	 * Block SIGINT and SIGTERM so that we can have dedicated threads
	 * to handle them with sigwait
	 */
	sigset_t sigset;
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &sigset, nullptr);
}

long get_cgroup_stat(const char *cgroup_name, const char *target_key)
{
	char cgroup_path[CGROUP_PATH_MAX_LEN];
	sprintf(cgroup_path, "/sys/fs/cgroup/memory/%s/memory.stat", cgroup_name);

	ifstream in(cgroup_path);
	if (!in) {
		return -1;
	}

	string key;
	long value;
	while (in >> key >> value) {
		if (key == target_key) {
			return value >> 20;
		}
	}
	return -1;
}

long get_cgroup_rss(const char *cgroup_name)
{
	// TODO: might consider using hierarchical limit
	return get_cgroup_stat(cgroup_name, "rss");
}

long get_cgroup_swap(const char *cgroup_name)
{
	// TODO: might consider using hierarchical limit
	return get_cgroup_stat(cgroup_name, "swap");
}

long get_memory_size()
{
	ifstream in("/proc/meminfo");
	if (!in) {
		return -1;
	}
	string key;
	long value;
	string unit;
	while (in >> key >> value >> unit) {
		if (key == "MemTotal:") {
			return value >> 10;
		}
	}
	return -1;
}

void set_cgroup_mp_to_local(const char *cgroup_name)
{
	/*
	 * Prevent memory pressure incurred by swap_out function
	 * propagate to the root memory cgroup by setting the mode
	 * to "local"
	 */

	int efd = eventfd(0, 0);
	if (efd == -1) {
		return;
	}

	char buffer[MAX_LINE_LEN];
	sprintf(buffer, "/sys/fs/cgroup/memory/%s/memory.pressure_level", cgroup_name);
	int mem_pressure = open(buffer, O_RDONLY);
	if (mem_pressure == -1) {
		return;
	}

	sprintf(buffer, "/sys/fs/cgroup/memory/%s/cgroup.event_control", cgroup_name);
	int event_control = open(buffer, O_WRONLY);
	if (event_control == -1) {
		return;
	}

	int ret;
	sprintf(buffer, "%d %d low,local", efd, mem_pressure);
	ret = write(event_control, buffer, strlen(buffer) + 1);
	if (ret < 0) {
		return;
	}

	sprintf(buffer, "%d %d medium,local", efd, mem_pressure);
	ret = write(event_control, buffer, strlen(buffer) + 1);
	if (ret < 0) {
		return;
	}

	sprintf(buffer, "%d %d critical,local", efd, mem_pressure);
	ret = write(event_control, buffer, strlen(buffer) + 1);
	if (ret < 0) {
		return;
	}
}

inline long convert_to_nr_node(long size)
{
	return size >> g_node_shift;
}

inline long convert_to_size(long nr_node)
{
	return nr_node << g_node_shift;
}

/*
 * Balloon Operations
 */

void inflate(long nr_nodes)
{
	for (int node_index = 0; node_index < nr_nodes; ++node_index) {
		if (g_cur_status.load() != NORMAL)
			break;

		void *addr = malloc(g_node_size << MB_SHIFT);
		if (!addr)
			break;
		memset(addr, 0, g_node_size << MB_SHIFT);
		g_node_vec.push_back(addr);
		g_cur_grab_size.fetch_add(g_node_size);

		std::this_thread::sleep_for(std::chrono::seconds(g_inflate_sleep_time));
	}

	g_inflate_deflate_lock.unlock();
}

void deflate(long nr_nodes)
{
	for (int node_index = 0; node_index < nr_nodes; ++node_index) {
		if (g_cur_status.load() != DEFLATE)
			break;

		// TODO: gracefully migrate data to other producers
		void *addr = g_node_vec.back();
		free(addr);
		g_node_vec.pop_back();
		g_cur_grab_size.fetch_sub(g_node_size);
	}

	g_inflate_deflate_lock.unlock();
	int expected_status = DEFLATE;
	g_cur_status.compare_exchange_strong(expected_status, NORMAL);
}

void swap_out()
{
	char cgroup_limit_path[CGROUP_PATH_MAX_LEN];
	sprintf(cgroup_limit_path, "/sys/fs/cgroup/memory/%s/memory.limit_in_bytes", g_cgroup_name);

	ofstream limit_file;
	limit_file.open(cgroup_limit_path);
	if (!limit_file)
		return;

	for (; g_swap_size.load() >= get_cgroup_swap(g_cgroup_name) + g_swap_unit_size
	       && get_cgroup_rss(g_cgroup_name) >= g_swap_limit_lower_bound + g_swap_unit_size
	       && g_cur_status.load() == NORMAL;
	       std::this_thread::sleep_for(std::chrono::seconds(g_swap_sleep_time))) {
		if (g_est_rss_diff.load() >= g_swap_threshold)
			continue;

		long cur_swap_size = get_cgroup_swap(g_cgroup_name);
		long cur_rss_size = get_cgroup_rss(g_cgroup_name);
		long target_limit = max(g_swap_limit_lower_bound,
		                        cur_rss_size - (g_est_swap_margin.load() + g_swap_unit_size));
		long expected_swap_diff = cur_rss_size - target_limit;

		limit_file << target_limit << "M" << endl;
		limit_file << "-1" << endl;

		long actual_swap_diff = get_cgroup_swap(g_cgroup_name) - cur_swap_size;
		g_est_swap_margin.store(max(0.0f, (float) g_est_swap_margin.load() * (1.0f - g_alpha_swap_margin)
		                                  + (float) (expected_swap_diff - actual_swap_diff)
		                                    * g_alpha_swap_margin));
	}

	limit_file.close();
	g_swap_lock.unlock();
}

void force_deflate_all(bool exit)
{
	g_cur_status.store(FORCE_DEFLATE);
	g_inflate_deflate_lock.lock();

	// TODO: notify the coordinator (not necessary to migrate data)
	while (!g_node_vec.empty()) {
		void *addr = g_node_vec.back();
		free(addr);
		g_node_vec.pop_back();
		g_cur_grab_size.fetch_sub(g_node_size);
	}

	if (exit) {
		g_cur_status.store(EXIT);
		// keep inflate & deflate lock and force deflate lock intentionally
	} else {
		g_cur_status.store(NORMAL);
		g_inflate_deflate_lock.unlock();
		g_force_deflate_lock.unlock();
	}
}

/*
 * Main Function
 */

int main(int argc, char *argv[])
{
	// loading input
	if (argc < 2) {
		printf("cannot get cgroup name\n");
		return -1;
	}
	g_cgroup_name = argv[1];

	// TODO: check capacities (CAP_SYS_RAWIO, ...)

	// initialization
	freopen("log.txt", "w", stderr);
	init_global_variables();
	init_curses();
	set_cgroup_mp_to_local(g_cgroup_name);
	block_signals();
	mlockall(MCL_CURRENT | MCL_FUTURE);

	sigterm_thread = thread(sigterm_handler);
	sigint_thread = thread(sigint_handler);
	mem_pressure_thread = thread(mem_pressure_handler);

	curses_stat_thread = thread(curses_stat);
	curses_op_thread = thread(curses_op);

	// main loop
	long prev_rss = get_cgroup_rss(g_cgroup_name);

	for (std::this_thread::sleep_for(std::chrono::seconds(g_main_loop_sleep_time));
	     g_cur_status.load() != EXIT;
	     std::this_thread::sleep_for(std::chrono::seconds(g_main_loop_sleep_time))) {
		// update rss diff estimation
		long cur_rss = get_cgroup_rss(g_cgroup_name);
		long cur_rss_diff = cur_rss - prev_rss;
		long old_rss_diff = g_est_rss_diff.load();
		if (cur_rss_diff > old_rss_diff) {
			g_est_rss_diff.store(cur_rss_diff);
		} else {
			g_est_rss_diff.store((float) old_rss_diff * (1.0f - g_alpha_rss_diff)
			                     + (float) cur_rss_diff * g_alpha_rss_diff);
		}
		prev_rss = cur_rss;

		// issue deflate
		long cur_grab_nr_nodes = convert_to_nr_node(g_cur_grab_size.load());
		long target_grab_nr_nodes = convert_to_nr_node(min(g_grab_size_upper_bound.load(),
		                                                   max(0l, g_total_free_size - cur_rss - g_free_margin_size
		                                                           - max(0l, g_est_rss_diff.load()))));
		int expected_status = NORMAL;
		if (target_grab_nr_nodes < cur_grab_nr_nodes
		    && g_cur_status.compare_exchange_strong(expected_status,
		                                            DEFLATE)) {
			g_inflate_deflate_lock.lock();
			if (deflate_thread.joinable())
				deflate_thread.join();
			deflate_thread = thread(deflate, cur_grab_nr_nodes - target_grab_nr_nodes);
			continue;
		}

		// issue inflate
		target_grab_nr_nodes = convert_to_nr_node(min(g_grab_size_lower_bound.load(),
		                                              max(0l, g_total_free_size - cur_rss - g_free_margin_size
		                                                      - max(0l, g_est_rss_diff.load()))));
		if (target_grab_nr_nodes > cur_grab_nr_nodes
		    && g_cur_status.load() == NORMAL
		    && g_inflate_deflate_lock.try_lock()) {
			if (inflate_thread.joinable())
				inflate_thread.join();
			inflate_thread = thread(inflate, target_grab_nr_nodes - cur_grab_nr_nodes);
		}

		// issue swap
		long target_swap_size = g_swap_size.load();
		long cur_swap_size = get_cgroup_swap(g_cgroup_name);
		if (target_swap_size >= cur_swap_size + g_swap_unit_size
		    && cur_rss >= g_swap_limit_lower_bound + g_swap_unit_size
		    && g_cur_status.load() == NORMAL
		    && g_swap_lock.try_lock()) {
			if (swap_out_thread.joinable())
				swap_out_thread.join();
			swap_out_thread = thread(swap_out);
		}
	}

	exit_curses();
	return 0;
}
