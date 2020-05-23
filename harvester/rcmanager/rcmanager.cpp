#include <iostream>
#include <fstream>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#define CGROUP_PATH_MAX_LEN 256
#define MAX_PERFORMANCE_LEN 256
#define PAGE_SHIFT 12

using namespace std;

/*
 * Global Variables
 */

const char *g_cgroup_name;
const char *g_promo_file_path;
const char *g_disk_promo_file_path;
const char *g_logging_file_path;
const char *g_tswap_stat_path;
long g_physical_memory_size;

mutex g_cgroup_limit_lock;
long g_cgroup_limit;

atomic_long g_promo_rate;
atomic_long g_disk_promo_rate;

const char *g_perf_file_path;
int g_perf_fd;

mutex g_bottom_line_lock;
long g_bottom_line;
chrono::time_point<chrono::system_clock> g_bottom_line_expire_time;

/*
 * Constants
 */

constexpr long g_unit_size = (64 << 20);
constexpr long g_min_cgroup_limit = 0;

constexpr long g_promo_rate_mi_threshold = (4 << 20);
constexpr long g_disk_promo_rate_mi_threshold = (64 << 10);
constexpr long g_promo_rate_bottom_line_threshold = (512 << 20);
constexpr long g_disk_promo_rate_bottom_line_threshold = (128 << 20);
constexpr long g_promo_bottom_line_ttl = 900;
constexpr long g_promo_rate_prefetch_threshold = (512 << 20);
constexpr long g_disk_promo_rate_prefetch_threshold = (128 << 20);
constexpr long g_promo_prefetch_size = (1 << 25);

constexpr long g_ad = g_unit_size;
constexpr float g_md_threshold = 2;
constexpr float g_md = 0.95;

constexpr long g_mi_rss_threshold = 2 * g_ad;
constexpr float g_mi = 2;

constexpr long g_dec_sleep_time = 5;
constexpr long g_warrior_sleep_time = 1;
constexpr long g_logging_sleep_time = 1;

constexpr long g_touch_rss_bottom_line_ttl = 3 * 300;  /* depends on quarantine time */
constexpr long g_touch_rss_bottom_line_threshold = 2 * g_ad;
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

double atomic_read_performance(int fd)
{
	int ret = file_read_lock(fd);
	if (ret < 0) {
		cout << "cannot lock performance file" << endl;
		exit(1);
	}

	char buffer[MAX_PERFORMANCE_LEN];
	lseek(fd, 0, SEEK_SET);
	int cnt = read(fd, buffer, MAX_PERFORMANCE_LEN);
	if (cnt < 0) {
		cout << "cannot read performance file" << endl;
		exit(1);
	} else if (cnt == 0) {
		file_read_unlock(fd);
		return 0;
	}

	double performance;
	sscanf(buffer, "%lf", &performance);

	file_read_unlock(fd);
	return performance;
}

long get_promotion_rate(const char *promo_file_path)
{
	ifstream in(promo_file_path);
	if (!in) {
		cout << "cannot open promotion rate file" << endl;
		exit(1);
	}

	long nr_promoted_page;
	in >> nr_promoted_page;

	return nr_promoted_page << PAGE_SHIFT;
}

long get_disk_promotion_rate(const char *disk_promo_file_path)
{
	ifstream in(disk_promo_file_path);
	if (!in) {
		cout << "cannot open disk promotion rate file" << endl;
		exit(1);
	}

	long nr_promoted_page;
	in >> nr_promoted_page;

	return nr_promoted_page << PAGE_SHIFT;
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
		if (key == "rss" || key == "mapped_file" || key == "cache") {
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
		if (now > g_bottom_line_expire_time) {
			g_bottom_line = -1;
		}
	}
	bottom_line = g_bottom_line;
	g_bottom_line_lock.unlock();
	return bottom_line;
}

long atomic_update_bottom_line(long bottom_line, long ttl, bool extend_current)
{
	long new_bottom_line;

	g_bottom_line_lock.lock();
	if (bottom_line >= g_bottom_line) {
		g_bottom_line = bottom_line;
		g_bottom_line_expire_time = chrono::system_clock::now() + chrono::seconds(ttl);
	} else if (extend_current && g_bottom_line >= 0) {
		g_bottom_line_expire_time = chrono::system_clock::now() + chrono::seconds(ttl);
	}
	new_bottom_line = g_bottom_line;
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

void tswap_prefetch(long prefetch_size)
{
	ofstream file("/sys/kernel/tswap/tswap_prefetch");
	if (!file) {
		cout << "cannot open tswap prefetch file" << endl;
		exit(1);
	}
	file << (prefetch_size >> PAGE_SHIFT) << endl;
}

/*
 * Warrior Thread Function
 */

void warrior_thread_fn()
{
	for (this_thread::sleep_for(chrono::seconds(g_warrior_sleep_time));;
	     this_thread::sleep_for(chrono::seconds(g_warrior_sleep_time))) {
		long rss = get_cgroup_rss(g_cgroup_name);
		long promotion_rate = get_promotion_rate(g_promo_file_path);
		g_promo_rate.store(promotion_rate);
		long disk_promotion_rate = get_disk_promotion_rate(g_disk_promo_file_path);
		g_disk_promo_rate.store(disk_promotion_rate);
		long swap = get_cgroup_swap(g_cgroup_name);
		long tswap_mem = get_tswap_memory_size(g_tswap_stat_path);
		long bottom_line = atomic_get_bottom_line();

		/* issue prefetch */
		if (promotion_rate >= g_promo_rate_prefetch_threshold
		    || disk_promotion_rate >= g_disk_promo_rate_prefetch_threshold) {
			tswap_prefetch(g_promo_prefetch_size);

			cout << "INC LOOP | PREFETCH" << endl;
		}

		/* set bottom line */
		if (promotion_rate >= g_promo_rate_bottom_line_threshold
		    || disk_promotion_rate >= g_disk_promo_rate_bottom_line_threshold) {
			bottom_line = atomic_update_bottom_line(min(g_physical_memory_size, (long)(g_mi * rss)),
			                                        g_promo_bottom_line_ttl, true);

			cout << "INC LOOP | SET BOTTOM LINE" << endl;
		}

		if (promotion_rate >= g_promo_rate_mi_threshold
		    || disk_promotion_rate >= g_disk_promo_rate_mi_threshold
		    || tswap_mem - swap > g_overflow_threshold) {
			g_cgroup_limit_lock.lock();
			if (rss < g_cgroup_limit - g_mi_rss_threshold) {
				long bottom_line = atomic_get_bottom_line();

				cout << "INC LOOP | promotion rate: " << (promotion_rate >> 20)
				     << " MB, disk promotion rate: " << (disk_promotion_rate >> 10)
				     << " KB, rss: " << (rss >> 20)
				     << " MB, cgroup limit: " << (g_cgroup_limit >> 20)
				     << " MB, bottom line: " << (bottom_line >> 20)
				     << " MB, SKIP" << endl;

				g_cgroup_limit_lock.unlock();
				continue;
			}

			/* perform MI */
			g_cgroup_limit = min(g_physical_memory_size, (long) (g_mi * g_cgroup_limit));
			set_cgroup_limit(g_cgroup_name, g_cgroup_limit);

			cout << "INC LOOP | promotion rate: " << (promotion_rate >> 20)
			     << " MB, disk promotion rate: " << (disk_promotion_rate >> 10)
			     << " KB, rss: " << (rss >> 20)
			     << " MB, cgroup limit: " << (g_cgroup_limit >> 20)
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
		long promotion_rate = g_promo_rate.load();
		long disk_promotion_rate = g_disk_promo_rate.load();
		long cgroup_limit = g_cgroup_limit;
		long rss = get_cgroup_rss(g_cgroup_name);
		long swap = get_cgroup_swap(g_cgroup_name);
		long bottom_line = atomic_get_bottom_line();
		long tswap_memory_size = get_tswap_memory_size(g_tswap_stat_path);

		logging_file << promotion_rate << ","
		             << disk_promotion_rate << ","
		             << cgroup_limit << ","
		             << rss << ","
		             << swap << ","
		             << bottom_line << ","
		             << tswap_memory_size;

		if (g_perf_file_path != nullptr) {
			double performance = atomic_read_performance(g_perf_fd);
			logging_file << "," << performance << endl;
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
	if (argc != 7 && argc != 8) {
		cout << "usage: <cgroup name> <promotion rate file path> "
		        "<disk promotion rate file path> <initial cgroup size (MB)> "
		        "<logging file path> <tswap stat path> "
		        "<performance file path (optional)>" << endl;
		return -1;
	}
	g_cgroup_name = argv[1];
	g_promo_file_path = argv[2];
	g_disk_promo_file_path = argv[3];
	g_logging_file_path = argv[5];
	g_tswap_stat_path = argv[6];
	g_perf_file_path = (argc == 8) ? argv[7] : nullptr;

	/* initialization */
	g_physical_memory_size = get_memory_size();
	sscanf(argv[4], "%ld", &g_cgroup_limit);
	g_cgroup_limit <<= 20;
	g_cgroup_limit = min(g_physical_memory_size, g_cgroup_limit);
	set_cgroup_limit(g_cgroup_name, g_cgroup_limit);
	get_promotion_rate(g_promo_file_path);
	g_bottom_line = -1;

	/* get performance file */
	if (g_perf_file_path != nullptr) {
		g_perf_fd = open(g_perf_file_path, O_RDONLY | O_CREAT, 00777);
		if (g_perf_fd < 0) {
			cout << "cannot open performance file" << endl;
			return -1;
		}
	}

	/* start threads */
	thread warrior_thread = thread(warrior_thread_fn);
	thread logging_thread = thread(logging_thread_fn);

	/* start AD loop */
	for (this_thread::sleep_for(chrono::seconds(g_dec_sleep_time));;
	     this_thread::sleep_for(chrono::seconds(g_dec_sleep_time))) {
		/* update max performance */
		long promotion_rate = g_promo_rate.load();
		long disk_promotion_rate = g_disk_promo_rate.load();
		long rss = get_cgroup_rss(g_cgroup_name);
		long bottom_line = atomic_get_bottom_line();
		long swap = get_cgroup_swap(g_cgroup_name);
		long tswap_mem = get_tswap_memory_size(g_tswap_stat_path);

		if (promotion_rate >= g_promo_rate_mi_threshold
		    || disk_promotion_rate >= g_disk_promo_rate_mi_threshold
		    || tswap_mem - swap > g_overflow_threshold) {
			cout << "DEC LOOP | promotion rate: " << (promotion_rate >> 20)
			     << " MB, disk promotion rate: " << (disk_promotion_rate >> 10)
			     << " KB, rss: " << (rss >> 20)
			     << " MB, cgroup limit: " << (g_cgroup_limit >> 20)
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

		/* apply bottom line */
		if (bottom_line >= 0) {
			proposed_cgroup_limit = max(proposed_cgroup_limit, bottom_line);
			if (proposed_cgroup_limit >= g_cgroup_limit) {
				op = "SKIP";
			}
		}
		/* update bottom line */
		if (proposed_cgroup_limit < rss + g_touch_rss_bottom_line_threshold) {
			bottom_line = max(g_min_cgroup_limit, rss - g_ad);
			bottom_line = atomic_update_bottom_line(bottom_line, g_touch_rss_bottom_line_ttl, false);
			proposed_cgroup_limit = max(proposed_cgroup_limit, bottom_line);
		}

		g_cgroup_limit = proposed_cgroup_limit;
		cout << "DEC LOOP | promotion rate: " << (promotion_rate >> 20)
		     << " MB, disk promotion rate: " << (disk_promotion_rate >> 10)
		     << " KB, rss: " << (rss >> 20)
		     << " MB, cgroup limit: " << (g_cgroup_limit >> 20)
		     << " MB, bottom line: " << (bottom_line >> 20)
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
