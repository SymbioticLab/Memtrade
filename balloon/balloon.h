#ifndef BALLOON_H
#define BALLOON_H

#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

#define MB_SHIFT 20
#define MB_SIZE (1 << MB_SHIFT)
/* ALL SIZE ARE REPRESENTED AS MB */

using namespace std;

/*
 * Input Variables
 */
extern const char *g_cgroup_name;

/*
 * Control Variables
 */
extern atomic_long g_swap_size;
extern atomic_long g_grab_size_lower_bound;
extern atomic_long g_grab_size_upper_bound;

/*
 * Status Variables
 */
extern atomic_long g_cur_grab_size;
extern atomic_int g_cur_status;
extern long g_total_free_size;

enum status {
	NORMAL,
	DEFLATE,
	FORCE_DEFLATE,
	EXIT,
};

/*
 * Estimation Parameters & Variables
 */
constexpr float g_alpha_rss_diff = 0.8;
constexpr float g_alpha_swap_margin = 0.8;
constexpr long g_init_rss_diff = 0;
constexpr long g_init_swap_margin = 16;
extern atomic_long g_est_rss_diff;
extern atomic_long g_est_swap_margin;

/*
 * Grabbed Memory Nodes
 */
// TODO: might consider using priority queue based on the eviction policy
extern vector<void *> g_node_vec;

/*
 * Mutexes for Balloon Operations
 */
extern mutex g_inflate_deflate_lock;
extern mutex g_swap_lock;
extern mutex g_force_deflate_lock;

/*
 * Mutexes for Logging
 */
extern mutex log_lock;

/*
 * Thread Variables
 */
extern thread inflate_thread;
extern thread deflate_thread;
extern thread swap_out_thread;

extern thread sigterm_thread;
extern thread sigint_thread;
extern thread mem_pressure_thread;

extern thread curses_stat_thread;
extern thread curses_op_thread;

/*
 * Balloon Constants
 */
constexpr int g_main_loop_sleep_time = 1;
constexpr int g_inflate_sleep_time = 1;
constexpr int g_swap_sleep_time = 1;

constexpr long g_swap_unit_size = 64;
constexpr long g_swap_threshold = 32;
constexpr long g_swap_limit_lower_bound = 1024;
constexpr long g_free_margin_size = 1024;

constexpr long g_node_shift = 26 - MB_SHIFT;
constexpr long g_node_size = (1 << g_node_shift);

/*
 * Prototypes for Balloon Operations
 */
void inflate(long nr_nodes);

void deflate(long nr_nodes);

void swap_out();

void force_deflate_all(bool exit);

/*
 * Prototypes for Event Handlers
 */
void sigterm_handler();

void sigint_handler();

void mem_pressure_handler();

/*
 * Prototypes for Command-Line Interface
 */
void init_curses();

void exit_curses();

void curses_stat();

void curses_op();

/*
 * Prototypes for Helper Functions
 */
long get_cgroup_stat(const char *cgroup_name, const char *target_key);

long get_cgroup_rss(const char *cgroup_name);

long get_cgroup_swap(const char *cgroup_name);

long get_memory_size();

void set_cgroup_mp_to_local(const char * cgroup_name);

inline long convert_to_nr_node(long size);

inline long convert_to_size(long nr_node);

inline void block_signals();

#endif
