#include <iostream>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>
#include <string>
#include <curses.h>

#include "balloon.h"

#define MAX_LINE_LEN 1024
#define MAX_NUM_LEN 5

#define STAT_NR_LINE 13
#define STAT_START_LN 2

#define CGROUP_NAME_LN 0
#define STATUS_LN 1
#define SWAP_SIZE_LN 3
#define GRAB_SIZE_LB_LN 4
#define GRAB_SIZE_UB_LN 5
#define FREE_MEM_LN 6
#define CUR_GRAB_SIZE_LN 8
#define CUR_SWAP_SIZE_LN 9
#define CUR_RSS_LN 10
#define EST_RSS_DIFF_LN 11
#define EST_SWAP_MARGIN_LN 12

#define CONTROL_INFO_LN 15
#define CONTROL_NR_LINE 5
#define CONTROL_START_LN 16

#define CONTROL_SWAP_LN 0
#define CONTROL_GRAB_LB_LN 1
#define CONTROL_GRAB_UB_LN 2
#define CONTROL_CONFIRM_LN 3
#define CONTROL_EXIT_LN 4


static void center_str(char *buffer, const char *src, char blank)
{
	size_t src_len = strlen(src);
	size_t left_len = (COLS - src_len) / 2;
	size_t right_len = COLS - src_len - left_len;
	memset(buffer, blank, left_len);
	memcpy(buffer + left_len, src, src_len);
	memset(buffer + left_len + src_len, blank, right_len);
	buffer[left_len + right_len + src_len] = 0;
}

static void fill_blanks(char *buffer)
{
	size_t src_len = strlen(buffer);
	memset(buffer + src_len, ' ', COLS - src_len);
	buffer[COLS] = 0;
}

void init_curses()
{
	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);

	curs_set(0);

	char buffer[MAX_LINE_LEN];
	center_str(buffer, "[BALLOON CONTROLLER]", ' ');
	mvaddstr(0, 0, buffer);

	center_str(buffer, "System Statistics", '-');
	mvaddstr(1, 0, buffer);

	center_str(buffer, "Control Panel", '-');
	mvaddstr(CONTROL_INFO_LN, 0, buffer);
	refresh();
}

void exit_curses()
{
	endwin();
}

void curses_stat()
{
	char buffer[MAX_LINE_LEN];
	WINDOW *win = newwin(STAT_NR_LINE, COLS, STAT_START_LN, 0);

	while (g_cur_status.load() != EXIT) {
		sprintf(buffer, "cgroup name:                  %s", g_cgroup_name);
		mvwaddstr(win, CGROUP_NAME_LN, 0, buffer);

		switch (g_cur_status.load()) {
			case NORMAL:
				sprintf(buffer, "status:                       NORMAL");
				fill_blanks(buffer);
				mvwaddstr(win, STATUS_LN, 0, buffer);
				break;
			case DEFLATE:
				sprintf(buffer, "status:                       DEFLATE");
				fill_blanks(buffer);
				mvwaddstr(win, STATUS_LN, 0, buffer);
				break;
			case FORCE_DEFLATE:
				sprintf(buffer, "status:                       FORCE DEFLATE");
				fill_blanks(buffer);
				mvwaddstr(win, STATUS_LN, 0, buffer);
				break;
			case EXIT:
				sprintf(buffer, "status:                       EXIT");
				fill_blanks(buffer);
				mvwaddstr(win, STATUS_LN, 0, buffer);
				break;
		}

		sprintf(buffer, "target swap size:             %ld MB", g_swap_size.load());
		fill_blanks(buffer);
		mvwaddstr(win, SWAP_SIZE_LN, 0, buffer);

		sprintf(buffer, "target grab size lower bound: %ld MB", g_grab_size_lower_bound.load());
		fill_blanks(buffer);
		mvwaddstr(win, GRAB_SIZE_LB_LN, 0, buffer);

		sprintf(buffer, "target grab size upper bound: %ld MB", g_grab_size_upper_bound.load());
		fill_blanks(buffer);
		mvwaddstr(win, GRAB_SIZE_UB_LN, 0, buffer);

		sprintf(buffer, "current grab size:            %ld MB", g_cur_grab_size.load());
		fill_blanks(buffer);
		mvwaddstr(win, CUR_GRAB_SIZE_LN, 0, buffer);

		sprintf(buffer, "total free memory:            %ld MB", g_total_free_size);
		fill_blanks(buffer);
		mvwaddstr(win, FREE_MEM_LN, 0, buffer);

		sprintf(buffer, "estimated RSS difference:     %ld MB", g_est_rss_diff.load());
		fill_blanks(buffer);
		mvwaddstr(win, EST_RSS_DIFF_LN, 0, buffer);

		sprintf(buffer, "estimated swap margin:        %ld MB", g_est_swap_margin.load());
		fill_blanks(buffer);
		mvwaddstr(win, EST_SWAP_MARGIN_LN, 0, buffer);

		sprintf(buffer, "current RSS:                  %ld MB", get_cgroup_rss(g_cgroup_name));
		fill_blanks(buffer);
		mvwaddstr(win, CUR_RSS_LN, 0, buffer);

		sprintf(buffer, "current swap size:            %ld MB", get_cgroup_swap(g_cgroup_name));
		fill_blanks(buffer);
		mvwaddstr(win, CUR_SWAP_SIZE_LN, 0, buffer);

		wrefresh(win);
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

static long convert_to_number(char *input, int nr_digit)
{
	long result = 0;
	for (int index = MAX_NUM_LEN - nr_digit; index < MAX_NUM_LEN; ++index) {
		result *= 10;
		result += input[index] - '0';
	}
	return result;
}

void curses_op()
{
	char buffer[MAX_LINE_LEN];
	WINDOW *win = newwin(CONTROL_NR_LINE, COLS, CONTROL_START_LN, 0);

	int index = 0;

	char swap_input[MAX_NUM_LEN + 1];
	char grab_lower_bound_input[MAX_NUM_LEN + 1];
	char grab_upper_bound_input[MAX_NUM_LEN + 1];
	int swap_input_nr = 0;
	int grab_lower_bound_input_nr = 0;
	int grab_upper_bound_input_nr = 0;
	memset(swap_input, ' ', MAX_NUM_LEN);
	memset(grab_lower_bound_input, ' ', MAX_NUM_LEN);
	memset(grab_upper_bound_input, ' ', MAX_NUM_LEN);
	swap_input[MAX_NUM_LEN] = 0;
	grab_lower_bound_input[MAX_NUM_LEN] = 0;
	grab_upper_bound_input[MAX_NUM_LEN] = 0;

	while (g_cur_status.load() != EXIT) {
		sprintf(buffer, "%c swap size:             [%s] MB", (index == CONTROL_SWAP_LN) ? '*' : ' ',
		        swap_input);
		mvwaddstr(win, CONTROL_SWAP_LN, 0, buffer);

		sprintf(buffer, "%c grab size lower bound: [%s] MB", (index == CONTROL_GRAB_LB_LN) ? '*' : ' ',
		        grab_lower_bound_input);
		mvwaddstr(win, CONTROL_GRAB_LB_LN, 0, buffer);

		sprintf(buffer, "%c grab size upper bound: [%s] MB", (index == CONTROL_GRAB_UB_LN) ? '*' : ' ',
		        grab_upper_bound_input);
		mvwaddstr(win, CONTROL_GRAB_UB_LN, 0, buffer);

		sprintf(buffer, "%c [CONFIRM]", (index == CONTROL_CONFIRM_LN) ? '*' : ' ');
		mvwaddstr(win, CONTROL_CONFIRM_LN, 0, buffer);

		sprintf(buffer, "%c [EXIT]", (index == CONTROL_EXIT_LN) ? '*' : ' ');
		mvwaddstr(win, CONTROL_EXIT_LN, 0, buffer);

		wrefresh(win);

		int ch = getch();
		if (ch >= '0' && ch <= '9') {
			switch (index) {
				case CONTROL_SWAP_LN:
					if (swap_input_nr < MAX_NUM_LEN) {
						memmove(swap_input + MAX_NUM_LEN - swap_input_nr - 1,
						        swap_input + MAX_NUM_LEN - swap_input_nr, swap_input_nr);
						swap_input[MAX_NUM_LEN - 1] = ch;
						++swap_input_nr;
					}
					break;
				case CONTROL_GRAB_LB_LN:
					if (grab_lower_bound_input_nr < MAX_NUM_LEN) {
						memmove(grab_lower_bound_input + MAX_NUM_LEN - grab_lower_bound_input_nr - 1,
						        grab_lower_bound_input + MAX_NUM_LEN - grab_lower_bound_input_nr,
						        grab_lower_bound_input_nr);
						grab_lower_bound_input[MAX_NUM_LEN - 1] = ch;
						++grab_lower_bound_input_nr;
					}
					break;
				case CONTROL_GRAB_UB_LN:
					if (grab_upper_bound_input_nr < MAX_NUM_LEN) {
						memmove(grab_upper_bound_input + MAX_NUM_LEN - grab_upper_bound_input_nr - 1,
						        grab_upper_bound_input + MAX_NUM_LEN - grab_upper_bound_input_nr,
						        grab_upper_bound_input_nr);
						grab_upper_bound_input[MAX_NUM_LEN - 1] = ch;
						++grab_upper_bound_input_nr;
					}
					break;
			}
		} else if (ch == KEY_UP) {
			if (index == 0) {
				index = CONTROL_NR_LINE - 1;
			} else {
				--index;
			}
		} else if (ch == KEY_DOWN) {
			index = (index + 1) % CONTROL_NR_LINE;
		} else if (ch == KEY_BACKSPACE) {
			switch (index) {
				case CONTROL_SWAP_LN:
					if (swap_input_nr > 0) {
						memmove(swap_input + MAX_NUM_LEN - swap_input_nr + 1,
						        swap_input + MAX_NUM_LEN - swap_input_nr, swap_input_nr - 1);
						swap_input[MAX_NUM_LEN - swap_input_nr] = ' ';
						--swap_input_nr;
					}
					break;
				case CONTROL_GRAB_LB_LN:
					if (grab_lower_bound_input_nr > 0) {
						memmove(grab_lower_bound_input + MAX_NUM_LEN - grab_lower_bound_input_nr + 1,
						        grab_lower_bound_input + MAX_NUM_LEN - grab_lower_bound_input_nr,
						        grab_lower_bound_input_nr - 1);
						grab_lower_bound_input[MAX_NUM_LEN - grab_lower_bound_input_nr] = ' ';
						--grab_lower_bound_input_nr;
					}
					break;
				case CONTROL_GRAB_UB_LN:
					if (grab_upper_bound_input_nr > 0) {
						memmove(grab_upper_bound_input + MAX_NUM_LEN - grab_upper_bound_input_nr + 1,
						        grab_upper_bound_input + MAX_NUM_LEN - grab_upper_bound_input_nr,
						        grab_upper_bound_input_nr - 1);
						grab_upper_bound_input[MAX_NUM_LEN - grab_upper_bound_input_nr] = ' ';
						--grab_upper_bound_input_nr;
					}
					break;
			}
		} else if (ch == '\n') {
			switch (index) {
				case CONTROL_CONFIRM_LN:
					g_swap_size.store(convert_to_number(swap_input, swap_input_nr));
					g_grab_size_lower_bound.store(convert_to_number(grab_lower_bound_input,
					                                                grab_lower_bound_input_nr));
					g_grab_size_upper_bound.store(convert_to_number(grab_upper_bound_input,
					                                                grab_upper_bound_input_nr));
					break;
				case CONTROL_EXIT_LN:
					g_cur_status.store(EXIT);
					break;
			}
		}
	}
}
