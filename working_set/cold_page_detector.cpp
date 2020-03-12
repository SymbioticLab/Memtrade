#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <map>

using namespace std;

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
const char *g_idlepath = "/sys/kernel/mm/page_idle/bitmap";
unsigned long long *g_idlebuf;
unsigned long long g_idlebufsize;

int histogram[1024] = {0}, max_histogram_idx = 0;

map<unsigned long long, int> age_map;
map<long, long> age_histogram;

void add_to_age_histogram( long age ) {
	age_histogram[age] = (age_histogram[age] ? (age_histogram[age] + 1) : 1);
}

void remove_from_age_histogram( long age ) {
	age_histogram[age] = (age_histogram[age] ? (age_histogram[age] - 1) : 0);
}

void print_age_histogram() {
	map<long,long>::iterator it;
	long cum = 0;

	for (it=age_histogram.begin(); it!=age_histogram.end(); ++it)
		cum += it->second;

	printf("\n\n====\nColdage Distribution (total pages %ld)\n====\n", age_map.size());
	printf("%8s\t%8s\t%8s\n", "Age", "Count", "%");

	for (it=age_histogram.begin(); it!=age_histogram.end(); ++it) {
		printf("%8ld\t%8ld\t%8.2lf\n", it->first, it->second, ((it->second/(cum*1.0))*100.0));
	}
}

void insert_page(unsigned long long pfn) {
	if (age_map[pfn]) {
		remove_from_age_histogram(age_map[pfn]);	
	}

	age_map[pfn] = (age_map[pfn] ? (age_map[pfn] + 1) : 1);
	add_to_age_histogram(age_map[pfn]);
}

void remove_page(unsigned long long pfn) {
	age_map[pfn] = 0;
	//age_map.erase(pfn);
}

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
	if ((pagebuf = (long long unsigned int*)malloc(pagebufsize)) == NULL) {
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
			remove_page(p[i]);
		}
		else {
			insert_page(p[i]);
		}
		g_walkedpages++;
	}

out:
	free(pagebuf);
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

	if ((g_idlebuf = (long long unsigned int*) malloc(MAX_IDLEMAP_SIZE)) == NULL) {
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

	while(1) {
		if(app_stat == ENDED) {
			calculate_histogram();
			print_histogram();
			print_age_histogram();
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
		free(g_idlebuf);

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
			printf("%-7s %10s %8s\t\n", "Est(s)", "Ref(MB)", "Active_Pages");
		}

		epoch += (double) est_us/ 1000000;
		printf("%-7.3f %10.2f %8d\t\n", epoch, mbytes, g_activepages);
	}
	return 0;
}
