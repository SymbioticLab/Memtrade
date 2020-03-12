#include <iostream>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/eventfd.h>

#include "balloon.h"

#define MAX_LINE_LEN 256

using namespace std;

void sigterm_handler()
{
	sigset_t sig_set;
	int sig;

	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGTERM);

	sigwait(&sig_set, &sig);
	g_force_deflate_lock.lock();
	force_deflate_all(true);
}

void sigint_handler()
{
	sigset_t sig_set;
	int sig;

	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGINT);

	sigwait(&sig_set, &sig);
	g_force_deflate_lock.lock();
	force_deflate_all(true);
}

void mem_pressure_handler()
{
	int efd = eventfd(0, 0);
	if (efd == -1) {
		return;
	}

	/*
	 * Application will be notified through eventfd when memory pressure is at
	 * the specific level (or higher)
	 */
	int mem_pressure = open("/sys/fs/cgroup/memory/memory.pressure_level", O_RDONLY);
	if (mem_pressure == -1) {
		return;
	}

	int event_control = open("/sys/fs/cgroup/memory/cgroup.event_control", O_WRONLY);
	if (event_control == -1) {
		return;
	}

	char buffer[MAX_LINE_LEN];
	sprintf(buffer, "%d %d medium", efd, mem_pressure);
	int ret = write(event_control, buffer, strlen(buffer) + 1);
	if (ret < 0) {
		return;
	}

	close(mem_pressure);
	close(event_control);

	uint64_t data;

	while (true) {
		ret = read(efd, &data, sizeof(data));
		if (ret == 0) {
			break;
		} else if (ret == -1 && (errno == EINTR || errno == EAGAIN)) {
			continue;
		} else if (ret != sizeof(data)) {
			break;
		}

		if (g_force_deflate_lock.try_lock()) {
			force_deflate_all(false);
		}
	}
}
