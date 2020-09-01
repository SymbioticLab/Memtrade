#include <iostream>
#include <thread>
#include <fstream>
#include <atomic>
#include <algorithm>
#include <list>
#include <mutex>
#include <condition_variable>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include "yaml-cpp/yaml.h"
#include "config.h"

#define CGROUP_PATH_MAX_LEN 256
#define MAX_PERFORMANCE_LEN 256
#define PAGE_SHIFT 12

using namespace std;

enum state_type {
	HARVEST = 0,
	RECOVERY = 1
};

struct control_context {
	/* configuration */
	control_config config;

	/* control loop state */
	state_type state;
	mutex state_lock;
	condition_variable state_cv;

	/* cgroup limit */
	atomic<long> cgroup_limit;

	/* timestamp */
	long timestamp;

	/* performance */
	int perf_fd;

	/* recovery time */
	chrono::time_point<chrono::steady_clock> recovery_start_time;
	int recovery_time;

	/* threads */
	thread harvest_thread;
	thread recovery_thread;

	/* logging */
	ofstream logging_file;
} g_ctx;

bool apply_cgroup_limit() {
	char cgroup_limit_path[CGROUP_PATH_MAX_LEN];
	sprintf(cgroup_limit_path,
		"/sys/fs/cgroup/memory/%s/memory.limit_in_bytes",
		g_ctx.config.cgroup_name.c_str());

	ofstream limit_file;
	limit_file.open(cgroup_limit_path);
	if (!limit_file) {
		cout << "[ERROR] cannot open cgroup limit file" << endl;
		exit(1);
	}

	limit_file << g_ctx.cgroup_limit << endl;

	return !!limit_file;
}

long get_memory_size() {
	ifstream in("/proc/meminfo");
	if (!in) {
		cout << "[ERROR] cannot open meminfo file" << endl;
		exit(1);
	}
	string key;
	long value;
	string unit;
	while (in >> key >> value >> unit) {
		if (key == "MemTotal:") {
			if (unit == "kB") {
				return value << 10;
			} else if (unit == "mB") {
				return value << 20;
			} else if (unit == "gB") {
				return value << 30;
			}
		}
	}
	cout << "[ERROR] cannot read meminfo file" << endl;
	exit(1);
}

int file_read_lock(int fd) {
	struct flock fl;

	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_RDLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	fl.l_pid = 0;

	return fcntl(fd, F_SETLKW, &fl);
}

int file_read_unlock(int fd) {
	struct flock fl;

	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_UNLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	fl.l_pid = 0;

	return fcntl(fd, F_SETLK, &fl);
}

float get_performance() {
	if (g_ctx.perf_fd < 0) {
		return 0;
	}

	int ret = file_read_lock(g_ctx.perf_fd);
	if (ret < 0) {
		cout << "[ERROR] cannot lock performance file" << endl;
		exit(1);
	}

	char buffer[MAX_PERFORMANCE_LEN];
	lseek(g_ctx.perf_fd, 0, SEEK_SET);
	int cnt = read(g_ctx.perf_fd, buffer, MAX_PERFORMANCE_LEN);
	if (cnt <= 0) {
		cout << "[ERROR] cannot read performance file" << endl;
		exit(1);
	}
	if (cnt == MAX_PERFORMANCE_LEN) {
		cout << "[WARNING] potential performance overflow" << endl;
	}

	float performance;
	sscanf(buffer, "%f", &performance);

	file_read_unlock(g_ctx.perf_fd);
	return performance;
}

long get_cgroup_rss() {
	char cgroup_path[CGROUP_PATH_MAX_LEN];
	sprintf(cgroup_path, "/sys/fs/cgroup/memory/%s/memory.stat",
		g_ctx.config.cgroup_name.c_str());

	ifstream in(cgroup_path);
	if (!in) {
		cout << "[ERROR] cannot open cgroup stat file" << endl;
		exit(1);
	}

	string key;
	long value;
	long rss = 0;
	while (in >> key >> value) {
		if (key == "total_rss" || key == "total_mapped_file" || key == "total_cache") {
			rss += value;
		}
	}
	return rss;
}

long get_cgroup_swap() {
	char cgroup_path[CGROUP_PATH_MAX_LEN];
	sprintf(cgroup_path, "/sys/fs/cgroup/memory/%s/memory.stat",
		g_ctx.config.cgroup_name.c_str());

	ifstream in(cgroup_path);
	if (!in) {
		cout << "[ERROR] cannot open cgroup stat file" << endl;
		exit(1);
	}

	string key;
	long value;
	while (in >> key >> value) {
		if (key == "total_swap") {
			return value;
		}
	}
	cout << "[ERROR] cannot read cgroup swap, please make sure that swap extension is enabled" << endl;
	exit(1);
}

long get_silo_memory_size() {
	ifstream in(g_ctx.config.silo.stat_path);
	if (!in) {
		cout << "[ERROR] cannot open silo stat file" << endl;
		exit(1);
	}
	long nr_memory_page = 0;
	string key;
	long value;
	while (in >> key >> value) {
		if (key == "nr_zombie_page:"
		    || key == "nr_in_memory_page:"
		    || key == "nr_in_memory_zombie_page:"
		    || key == "nr_in_flight_page:"
		    || key == "nr_prefetched_page:") {
			nr_memory_page += value;
		}
	}
	return nr_memory_page << PAGE_SHIFT;
}

long get_silo_promotion_rate() {
	ifstream in(g_ctx.config.silo.promotion_rate_path);
	if (!in) {
		cout << "[ERROR] cannot open promotion rate file" << endl;
		exit(1);
	}

	long nr_promoted_page;
	in >> nr_promoted_page;

	return nr_promoted_page << PAGE_SHIFT;
}

long get_silo_disk_promotion_rate() {
	ifstream in(g_ctx.config.silo.disk_promotion_rate_path);
	if (!in) {
		cout << "[ERROR] cannot open disk promotion rate file" << endl;
		exit(1);
	}

	long nr_promoted_page;
	in >> nr_promoted_page;

	return nr_promoted_page << PAGE_SHIFT;
}

void silo_prefetch(long prefetch_size) {
	ofstream file(g_ctx.config.silo.prefetch_path);
	if (!file) {
		cout << "[ERROR] cannot open silo prefetch file" << endl;
		exit(1);
	}
	file << (prefetch_size >> PAGE_SHIFT) << endl;
}

void harvest_thread_fn() {
	while (true) {
		std::unique_lock<std::mutex> lock(g_ctx.state_lock);
		g_ctx.state_cv.wait(lock, [] { return g_ctx.state == HARVEST; });

		g_ctx.cgroup_limit = max(get_cgroup_rss() - g_ctx.config.control_loop.harvest.step_size, 0l);
		apply_cgroup_limit();

		g_ctx.recovery_time = (int) ((float) g_ctx.recovery_time - g_ctx.config.control_loop.recovery_time.ad);
		g_ctx.recovery_time = max(g_ctx.config.control_loop.recovery_time.min, g_ctx.recovery_time);

		lock.unlock();
		this_thread::sleep_for(chrono::seconds(g_ctx.config.control_loop.harvest.sleep_time));
	}
}

void recovery_thread_fn() {
	long cur_step_size = g_ctx.config.control_loop.recovery.step_size;
	while (true) {
		std::unique_lock<std::mutex> lock(g_ctx.state_lock);
		g_ctx.state_cv.wait(lock, [&cur_step_size] {
			if (g_ctx.state == RECOVERY) {
				return true;
			} else {
				cur_step_size = g_ctx.config.control_loop.recovery.step_size;
				return false;
			}
		});

		long cgroup_rss = get_cgroup_rss();
		if (g_ctx.cgroup_limit - cgroup_rss < g_ctx.config.control_loop.recovery.step_size) {
			g_ctx.cgroup_limit += cur_step_size;
			apply_cgroup_limit();

			cur_step_size = (long) ((float) cur_step_size * g_ctx.config.control_loop.recovery.step_mi);
		}

		lock.unlock();
		this_thread::sleep_for(chrono::seconds(g_ctx.config.control_loop.recovery.sleep_time));
	}
}

void init_ctx(YAML::Node &config_file) {
	g_ctx.config = control_config::parse_yaml(config_file);

	g_ctx.state = RECOVERY;

	g_ctx.cgroup_limit = get_memory_size();
	apply_cgroup_limit();

	g_ctx.timestamp = 0;

	if (g_ctx.config.logging.performance_file_path != "") {
		g_ctx.perf_fd = open(g_ctx.config.logging.performance_file_path.c_str(),
		                     O_RDONLY | O_CREAT, 00777);
		if (g_ctx.perf_fd < 0) {
			cout << "[ERROR] cannot open performance file" << endl;
			exit(1);
		}
	} else {
		g_ctx.perf_fd = -1;
	}

	get_silo_promotion_rate();  /* clear promotion rate */
	get_silo_disk_promotion_rate();  /* clear disk promotion rate */

	g_ctx.recovery_start_time = chrono::steady_clock::now();
	g_ctx.recovery_time = g_ctx.config.control_loop.recovery_time.min;

	g_ctx.harvest_thread = thread(harvest_thread_fn);
	g_ctx.recovery_thread = thread(recovery_thread_fn);

	g_ctx.logging_file.open(g_ctx.config.logging.file_path);
	if (!g_ctx.logging_file) {
		cout << "[ERROR] cannot open logging file" << endl;
		exit(1);
	}
	g_ctx.logging_file << "timestamp,"
			   << "state,"
			   << "cgroup_limit,"
			   << "recovery_time,"
			   << "performance,"
			   << "promotion_rate,"
			   << "disk_promotion_rate,"
			   << "silo_memory_size,"
			   << "cgroup_rss,"
			   << "cgroup_swap"
			   << endl;
}

int main(int argc, char *argv[]) {
	if (argc != 2) {
		cout << "Usage: " << argv[0] << " <path to config.yaml>" << endl;
		exit(1);
	}

	YAML::Node config_file = YAML::LoadFile(argv[1]);
	init_ctx(config_file);

	while (true) {
		/* collect measurements */
		float performance = get_performance();
		long promotion_rate = get_silo_promotion_rate();
		long disk_promotion_rate = get_silo_disk_promotion_rate();
		long silo_memory_size = get_silo_memory_size();
		long cgroup_rss = get_cgroup_rss();
		long cgroup_swap = get_cgroup_swap();

		/* run performance drop detection */
		bool perf_dropped = promotion_rate >= g_ctx.config.performance_drop_detection.promo_rate
		                    || disk_promotion_rate >= g_ctx.config.performance_drop_detection.disk_promo_rate;

		/* handle state transition */
		std::unique_lock<std::mutex> lock(g_ctx.state_lock);
		state_type prev_state = g_ctx.state;
		if (g_ctx.config.control_loop.enable) {
			if (g_ctx.state == HARVEST) {
				if (perf_dropped) {
					g_ctx.state = RECOVERY;
					g_ctx.recovery_start_time = chrono::steady_clock::now();
					if (disk_promotion_rate >= g_ctx.config.performance_drop_detection.disk_promo_rate) {
						g_ctx.recovery_time = (int) ((float) g_ctx.recovery_time * g_ctx.config.control_loop.recovery_time.mi);
						g_ctx.recovery_time = min(g_ctx.config.control_loop.recovery_time.max, g_ctx.recovery_time);
					}
				}
			} else {
				if (chrono::steady_clock::now() >=
				    g_ctx.recovery_start_time + chrono::seconds(g_ctx.recovery_time)) {
					if (!perf_dropped) {
						g_ctx.state = HARVEST;
					} else {
						g_ctx.recovery_start_time = chrono::steady_clock::now();
						if (disk_promotion_rate >= g_ctx.config.performance_drop_detection.disk_promo_rate) {
							g_ctx.recovery_time = (int) ((float) g_ctx.recovery_time * g_ctx.config.control_loop.recovery_time.mi);
							g_ctx.recovery_time = min(g_ctx.config.control_loop.recovery_time.max, g_ctx.recovery_time);
						}
					}
				}
			}
		}
		state_type cur_state = g_ctx.state;
		int cur_recovery_time = g_ctx.recovery_time;
		lock.unlock();

		if (prev_state != cur_state) {
			g_ctx.state_cv.notify_all();
			cout << "[INFO] state transited" << endl;
		}

		/* prefetch */
		if (promotion_rate >= g_ctx.config.control_loop.prefetch.promo_rate
		    || disk_promotion_rate >= g_ctx.config.control_loop.prefetch.disk_promo_rate) {
			silo_prefetch(g_ctx.config.control_loop.prefetch.size);
			cout << "[INFO] prefetched" << endl;
		}

		/* log */
		cout << "[INFO] timestamp: " << g_ctx.timestamp << ", "
		     << "state: " << ((cur_state == HARVEST) ? "HARVEST" : "RECOVERY") << ", "
		     << "cgroup limit: " << (g_ctx.cgroup_limit >> 20) << " MB, "
		     << "recovery time: " << cur_recovery_time << " s, "
		     << "performance: " << performance << ", "
		     << "promotion rate: " << (promotion_rate >> 20) << " MB, "
		     << "disk promotion rate: " << (disk_promotion_rate >> 20) << " MB, "
		     << "silo memory size: " << (silo_memory_size >> 20) << " MB, "
		     << "cgroup rss: " << (cgroup_rss >> 20) << " MB, "
		     << "cgroup swap: " << (cgroup_swap >> 20) << " MB"
		     << endl;
		g_ctx.logging_file << g_ctx.timestamp << ","
				   << cur_state << ","
				   << g_ctx.cgroup_limit << ","
				   << cur_recovery_time << ","
				   << performance << ","
				   << promotion_rate << ","
				   << disk_promotion_rate << ","
				   << silo_memory_size << ","
				   << cgroup_rss << ","
				   << cgroup_swap << endl;
		++g_ctx.timestamp;
		this_thread::sleep_for(chrono::seconds(g_ctx.config.control_loop.sleep_time));
	}

	return 0;
}
