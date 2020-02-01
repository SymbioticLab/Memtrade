/*
 * wss.c	Estimate the working set size (WSS) for a process on Linux.
 *
 * This is a proof of concept that uses idle page tracking from Linux 4.3+, for
 * a page-based WSS estimation. This version snapshots the entire system's idle
 * page flags, which is efficient for analyzing large processes, but not tiny
 * processes. For those, see wss-v1.c. There is also wss.pl, which uses can be
 * over 10x faster and works on older Linux, however, uses the referenced page
 * flag and has its own caveats. These tools can be found here:
 *
 * http://www.brendangregg.com/wss.pl
 *
 * Currently written for x86_64 and default page size only. 
 *
 * COMPILE: gcc -o wss wss.c
 *
 * REQUIREMENTS: Linux 4.3+
 *
 * USAGE: wss PID duration(s)
 *
 * COLUMNS:
 *	- Est(s):  		Estimated WSS measurement duration: this accounts for delays
 *	           		with setting and reading pagemap data, which inflates the
 *	           		intended sleep duration.
 *	- Ref(MB): 		Referenced (Mbytes) during the specified duration.
 *	           		This is the working set size metric.
 *  - Active_Pages:	Number of pages active within the specified duration. 
 *  - epoch: 		Epoch elapsed after the tool runs
 * 	- used  free  buff  cach: These four columns represent the memory used by the
 * 					application during that epoch
 *  - used  free: 	These two columns indicates the swap usage during that epoch
 * 	- in   out:		The swap in and out rate; if paging happens
 * 	- memory process:The name of teh process that consumes tha maximum memoru usage
 * 					during that epoch 
 * 
 * On 13-Jan-2018 Brendan Gregg Created the initial version of this tool with 
 * the following Copyright and Licence:
 * 
 * Copyright 2018 Netflix, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License")
 *
 * The tool has been further modified and updated by Hasan Al Maruf
 * Copyright 2020 University of Michigan, Ann Arbor
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <string.h>

// see Documentation/vm/pagemap.txt:
#define PFN_MASK		(~(0x1ffLLU << 55))

#define PATHSIZE		128
#define LINESIZE		256
#define PAGEMAP_CHUNK_SIZE	8
#define IDLEMAP_CHUNK_SIZE	8
#define IDLEMAP_BUF_SIZE	4096

// application status related globalss
#define APPLICATION_ACTIVE_THRESHOLD 10
enum state {
	WAITING,
	STARTED,
	ENDED
};

// big enough to span 740 Gbytes:
#define MAX_IDLEMAP_SIZE	(20 * 1024 * 1024)

// from mm/page_idle.c:
#ifndef BITMAP_CHUNK_SIZE
#define BITMAP_CHUNK_SIZE	8
#endif

#ifndef PAGE_OFFSET
#define PAGE_OFFSET		0xffff880000000000LLU
#endif

// globals
int g_debug = 0;		// 1 == some, 2 == verbose
int g_activepages = 0;
int g_walkedpages = 0;
char *g_idlepath = "/sys/kernel/mm/page_idle/bitmap";
unsigned long long *g_idlebuf;
unsigned long long g_idlebufsize;

// dstat variables
FILE* _pipe;
char* DSTAT_CMD = "dstat -Tmsg --top-mem";
const char s[4] = "| \t";

int histogram[1024] = {0}, max_histogram_idx = 0;

/*
 * This code must operate on bits in the pageidle bitmap and process pagemap.
 * Doing this one by one via syscall read/write on a large process can take too
 * long, eg, 7 minutes for a 130 Gbyte process. Instead, I copy (snapshot) the
 * idle bitmap and pagemap into our memory with the fewest syscalls allowed,
 * and then process them with load/stores. Much faster, at the cost of some memory.
 */

int mapidle(pid_t pid, unsigned long long mapstart, unsigned long long mapend) {
	char pagepath[PATHSIZE];
	int pagefd;
	char *line;
	unsigned long long offset, i, pagemapp, pfn, idlemapp, idlebits;
	int pagesize;
	int err = 0;
	unsigned long long *pagebuf, *p;
	unsigned long long pagebufsize;
	ssize_t len;
	
	// XXX: handle huge pages
	pagesize = getpagesize();

	pagebufsize = (PAGEMAP_CHUNK_SIZE * (mapend - mapstart)) / pagesize;
	if ((pagebuf = malloc(pagebufsize)) == NULL) {
		printf("Can't allocate memory for pagemap buf (%lld bytes)",
		    pagebufsize);
		return 1;
	}

	// open pagemap for virtual to PFN translation
	if (sprintf(pagepath, "/proc/%d/pagemap", pid) < 0) {
		printf("Can't allocate memory.");
		return 1;
	}
	if ((pagefd = open(pagepath, O_RDONLY)) < 0) {
		perror("Can't read pagemap file");
		return 2;
	}

	// cache pagemap to get PFN, then operate on PFN from idlemap
	offset = PAGEMAP_CHUNK_SIZE * mapstart / pagesize;
	if (lseek(pagefd, offset, SEEK_SET) < 0) {
		printf("Can't seek pagemap file\n");
		err = 1;
		goto out;
	}
	p = pagebuf;

	// optimized: read this in one syscall
	if (read(pagefd, p, pagebufsize) < 0) {
		perror("Read page map failed.");
		err = 1;
		goto out;
	}

	for (i = 0; i < pagebufsize / sizeof (unsigned long long); i++) {
		// convert virtual address p to physical PFN
		pfn = p[i] & PFN_MASK;
		if (pfn == 0)
			continue;

		// read idle bit
		idlemapp = (pfn / 64) * BITMAP_CHUNK_SIZE;
		if (idlemapp > g_idlebufsize) {
			printf("ERROR: bad PFN read from page map.\n");
			err = 1;
			goto out;
		}
		idlebits = g_idlebuf[idlemapp];
		if (g_debug > 1) {
			printf("R: p %llx pfn %llx idlebits %llx\n", p[i], pfn, idlebits);
		}

		if (!(idlebits & (1ULL << (pfn % 64)))) {
			g_activepages++;
		}
		g_walkedpages++;
	}

out:
	close(pagefd);

	return err;
}

int walkmaps(pid_t pid) {
	FILE *mapsfile;
	char mapspath[PATHSIZE];
	char line[LINESIZE];
	size_t len = 0;
	unsigned long long mapstart, mapend;

	// read virtual mappings
	if (sprintf(mapspath, "/proc/%d/maps", pid) < 0) {
		printf("Can't allocate memory. Exiting.");
		exit(1);
	}
	if ((mapsfile = fopen(mapspath, "r")) == NULL) {
		perror("Can't read maps file");
		exit(2);
	}

	while (fgets(line, sizeof (line), mapsfile) != NULL) {
		sscanf(line, "%llx-%llx", &mapstart, &mapend);
		if (g_debug)
			printf("MAP %llx-%llx\n", mapstart, mapend);
		if (mapstart > PAGE_OFFSET)
			continue;	// page idle tracking is user mem only
		if (mapidle(pid, mapstart, mapend)) {
			printf("Error setting map %llx-%llx. Exiting.\n", mapstart, mapend);
		}
	}

	fclose(mapsfile);

	return 0;
}

int setidlemap() {
	char *p;
	int idlefd, i;
	// optimized: large writes allowed here:
	char buf[IDLEMAP_BUF_SIZE];

	for (i = 0; i < sizeof (buf); i++)
		buf[i] = 0xff;

	// set entire idlemap flags
	if ((idlefd = open(g_idlepath, O_WRONLY)) < 0) {
		perror("Can't write idlemap file");
		exit(2);
	}
	// only sets user memory bits; kernel is silently ignored
	while (write(idlefd, &buf, sizeof(buf)) > 0) {;}

	close(idlefd);

	return 0;
}

int loadidlemap() {
	unsigned long long *p;
	int idlefd;
	ssize_t len;

	if ((g_idlebuf = malloc(MAX_IDLEMAP_SIZE)) == NULL) {
		printf("Can't allocate memory for idlemap buf (%d bytes)", MAX_IDLEMAP_SIZE);
		exit(1);
	}

	// copy (snapshot) idlemap to memory
	if ((idlefd = open(g_idlepath, O_RDONLY)) < 0) {
		perror("Can't read idlemap file");
		exit(2);
	}
	p = g_idlebuf;
	// unfortunately, larger reads do not seem supported
	while ((len = read(idlefd, p, IDLEMAP_CHUNK_SIZE)) > 0) {
		p += IDLEMAP_CHUNK_SIZE;
		g_idlebufsize += len;
	}
	close(idlefd);

	return 0;
}

int exec_cmd( char* cmd ) {
	_pipe = popen(cmd, "r");
	if(!_pipe){
		printf("Failed to execute the command\n");
		return 0;
	}
	return 1;
}

void tokenize_buffer ( char* buffer ) {
	char *token = strtok(buffer, s);
	while( token != NULL ) {
		if(!(token[0] == ' ' || token[0] == '\t' || token[0] == '\n'))
			printf( "%s ", token );
		token = strtok(NULL, s);
	}
}

void close_pipe(){
	pclose(_pipe);
}

int get_pipe_data( char* buffer, int buffer_size, int do_print ) {
	if(feof(_pipe)){
		close_pipe();
		return 0;
	}

	if(fgets(buffer, buffer_size, _pipe) != NULL) {
		if(do_print == 1)
			printf("%s", buffer);
		return 1;
	}
	return 0;
}

int run_dstat() {
    return exec_cmd(DSTAT_CMD);
}

void calculate_histogram() {
	int idx;
	for(idx = 1; idx <= max_histogram_idx; idx++)
		histogram[idx] += histogram[idx-1];
}

void print_histogram() {
	int idx;
	printf("\n\nhistogram\n=========\n%10s\t%10s\n", "MemSize(MB)", "Percentage(%)");
        for(idx = 0; idx <= max_histogram_idx; idx++) {
		printf("%10d\t%10.2lf\n", (idx+1)*100, ((histogram[idx]*100.0)/histogram[max_histogram_idx]));
	}
}

int main(int argc, char *argv[]) {
	pid_t pid;
	double duration, mbytes;
	static struct timeval ts1, ts2, ts3, ts4;
	unsigned long long set_us, read_us, dur_us, slp_us, est_us;
	double epoch = 0;
	int page_size = getpagesize(), app_stat = WAITING;
	char buffer[1024];

	// options
	if (argc < 3) {
		printf("USAGE: wss PID duration(s)\n");
		exit(0);
	}	
	pid = atoi(argv[1]);
	duration = atof(argv[2]);
	if (duration < 0.01) {
		printf("Interval too short. Exiting.\n");
		return 1;
	}
	printf("Watching PID %d page references during %.2f seconds...\n", pid, duration);

	run_dstat();
	get_pipe_data(buffer, 1024, 0);
	
	while(1) {
		if(app_stat == ENDED) {
			calculate_histogram();
			print_histogram();
			close_pipe();
			break;
		}
		g_activepages = 0;
		g_walkedpages = 0;
		
		// set idle flags
		gettimeofday(&ts1, NULL);
		setidlemap();

		// sleep
		gettimeofday(&ts2, NULL);
		//using 97% of duration is sleeping to avoid lagging behind due to sleep-wake delay
		usleep((int)(duration * 970000));
		gettimeofday(&ts3, NULL);

		// read idle flags
		loadidlemap();
		walkmaps(pid);
		gettimeofday(&ts4, NULL);

		// calculate times
		set_us = 1000000 * (ts2.tv_sec - ts1.tv_sec) + (ts2.tv_usec - ts1.tv_usec);
		slp_us = 1000000 * (ts3.tv_sec - ts2.tv_sec) + (ts3.tv_usec - ts2.tv_usec);
		read_us = 1000000 * (ts4.tv_sec - ts3.tv_sec) + (ts4.tv_usec - ts3.tv_usec);
		dur_us = 1000000 * (ts4.tv_sec - ts1.tv_sec) + (ts4.tv_usec - ts1.tv_usec);
		est_us = dur_us - (set_us / 2) - (read_us / 2);
		if (g_debug) {
			printf("set time  : %.3f s\n", (double)set_us / 1000000);
			printf("sleep time: %.3f s\n", (double)slp_us / 1000000);
			printf("read time : %.3f s\n", (double)read_us / 1000000);
			printf("dur time  : %.3f s\n", (double)dur_us / 1000000);
			// assume getpagesize() sized pages:
			printf("referenced: %d pages, %d Kbytes\n", g_activepages, g_activepages * page_size);
			printf("walked    : %d pages, %d Kbytes\n", g_walkedpages, g_walkedpages * page_size);
		}

		// assume getpagesize() sized pages:
		mbytes = ((g_activepages * 1.0) / (1024*1024)) * page_size;
		
		if(app_stat == STARTED && (unsigned long long) mbytes < APPLICATION_ACTIVE_THRESHOLD)
			app_stat = ENDED;
			
		if(app_stat == WAITING && (unsigned long long) mbytes > APPLICATION_ACTIVE_THRESHOLD)
			app_stat = STARTED;

		if(app_stat == STARTED) {
			int idx = (unsigned long long) (mbytes/100);
			histogram[idx]++;
			if(idx > max_histogram_idx)
				max_histogram_idx = idx;
		}

		if(epoch == 0) {
			printf("%-7s %10s %8s\t", "Est(s)", "Ref(MB)", "Active_Pages");
			get_pipe_data(buffer, 1024, 1);
		}

		epoch += (double) est_us/ 1000000;
		printf("%-7.3f %10.2f %8d\t", epoch, mbytes, g_activepages);
		get_pipe_data(buffer, 1024, 1);
	}
	return 0;
}