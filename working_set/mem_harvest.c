#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#define ONE_GB (long long) 1024 * 1024 * 1024
#define ONE_MB (long long) 1024 * 1024
#define PAGE_SIZE 4096
#define GRAB_SIZE 256 * ONE_MB
#define MAX_OPS 615202
#define SOFT 0.9
#define HARD 0.75
#define MAX_WAIT 5
#define MAX_GRAB 2000 * ONE_MB
#define MAX_NODE 100

char* mem_node[MAX_NODE];
char* addr;
long position, mapped_mem_size = 3 * ONE_GB;
static struct timeval ts1, ts2, ts3;
unsigned long long dur_s;
int harvester_inited = 0;

char* DSTAT_CMD = "dstat -Tmsg --top-mem";
char* MEMTIER_CMD = "/somedir/memtier_benchmark/memtier_benchmark -t 10 -n 400000 --ratio 1:1 -c 20 -x 1 --key-pattern R:R --hide-histogram --distinct-client-seed -d 300 --pipeline=1000 2>&1 | tee ./out.txt";

const char s[10] = "#()[]:, \t";

char *ltrim(char *s) {
	while(isspace(*s)) s++;
	return s;
}

char *rtrim(char *s) {
	char* back;
	int len = strlen(s);

	if(len == 0)
		return(s);

	back = s + len;
	while(isspace(*--back));
	*(back+1) = '\0';
	return s;
}

char *trim(char *s) {
	return rtrim(ltrim(s));
}

int init_mem_zone() {
	int i = 0;
	for(i=0; i<MAX_NODE; i++) {
		mem_node[i] = NULL;
	}
	position = -1;
	harvester_inited = 1;
        gettimeofday(&ts1, NULL);

        return 0;
}

void grab_memory() {
	if(position >= MAX_NODE || ((position+1)*GRAB_SIZE) >= MAX_GRAB){
		return;
	}
	position++;
	mem_node[position] = (char*) malloc(GRAB_SIZE * sizeof(char));
	mlock(mem_node[position], GRAB_SIZE);
	gettimeofday(&ts1, NULL);
	return;
}

void release_memory() {
	if(position < 0 || mem_node[position] == NULL) {
		return;
	}
	munlock(mem_node[position], GRAB_SIZE);
	free(mem_node[position]);
	gettimeofday(&ts2, NULL);
	position--;
	return;
}

void release_all() {
	int i = 0;
	printf("releasing all mapped memory\n");
	
	for(i=0; i< MAX_NODE; i++) {
		if(mem_node[i] != NULL) {
			munlock(mem_node[i], GRAB_SIZE);
			free(mem_node[i]);
		}
	}
	return;
}

/*
int init_mem_zone() {
        if(posix_memalign((void **) &addr, PAGE_SIZE, mapped_mem_size)) {
                perror("failed to initialize page memory\n");
                return -1;
        }
        position = 0;
	harvester_inited = 1;
	gettimeofday(&ts1, NULL);

        return 0;
}

void grab_memory() {
        if(position > mapped_mem_size - GRAB_SIZE || position >= MAX_GRAB) {
     //           printf("Can't grab more memory, reached to the max limit\n");
                return;
        }
        mlock(addr + position, GRAB_SIZE);
        position += GRAB_SIZE;
        if(position > mapped_mem_size - GRAB_SIZE)
                position = mapped_mem_size - GRAB_SIZE;
	gettimeofday(&ts1, NULL);
	//printf("mlocked memory %0.1lf MB\n", ((1.0*position)/ONE_MB));
	return;
}

void release_memory() {
        if(position <= 0) {
       //         printf("nothing to release, grab something first to release\n");
                return;
        }
        position -= GRAB_SIZE;
        if( position <= 0 )
                position = 0;
        munlock(addr + position, GRAB_SIZE);
	gettimeofday(&ts2, NULL);
        return;
}

void release_all() {
        printf("releasing all mapped memory\n");
        munlock(addr, mapped_mem_size);
}
*/

long get_val ( char* buffer, char* comp ) {
	char temp_buf [1024];
	memcpy(temp_buf, buffer, 1024);
	char *token = strtok(temp_buf, s);
	char *prev_token = token;
	long val = -1;
	while( token != NULL ) {
		token = trim(token);
		if( strcmp(token, comp) == 0 ) {
			val = atol(prev_token);
			return val;
		}
		prev_token = token;
		token = strtok(NULL, s);
	}
	return val;
}


int exec_cmd( char* cmd ) {
	FILE* _pipe = popen(cmd, "r");
	char buffer[1024];
       	int buffer_size = 1024, d, r;
	long ops, max_ops = 0, epoch, run;

	if(_pipe == NULL) {
		perror("Error opening file");
		return(-1);
	}
	printf("command executed\n");
	d = fileno(_pipe);
	fcntl(d, F_SETFL, O_NONBLOCK);	
	
	while(1) {
		r = read(d, buffer, buffer_size);
		if (r == -1 && errno == EAGAIN)
			continue;
		else if (r > 0){
			ops = get_val(buffer, "avg");
			epoch = get_val(buffer, "secs");
			//run = get_val(buffer, "1%");

			//if(run == 2 && epoch == 0)
			//	harvester_inited = 1;
			if(ops >  max_ops)
				max_ops = ops;
			if(ops != -1 && harvester_inited == 1) {
				if(ops >= MAX_OPS * SOFT) {
					gettimeofday(&ts3, NULL);
					dur_s = (ts3.tv_sec - ts1.tv_sec); //* 1000000 + (ts3.tv_usec - ts1.tv_usec);
					if(dur_s > MAX_WAIT) {
						//printf("ops %ld is %0.2lf %% of max ops %d, grab memory\n", 
						//	ops, ((100.0*ops)/(1.0*MAX_OPS)), MAX_OPS);
						grab_memory();
					}
					else {
						//printf("ops %ld is %0.2lf %% of max ops %d, but only %lld sec elapsed\n", ops, ((100.0*ops)/(1.0*MAX_OPS)) , MAX_OPS, dur_s);
					}
				}
				else if(ops <= MAX_OPS * HARD) {
					gettimeofday(&ts3, NULL);
                                        dur_s = (ts3.tv_sec - ts2.tv_sec);

					if(dur_s > MAX_WAIT) {
						//printf("ops %ld is smaller than %0.1lf %% of max ops %d, release memory\n", ops, (100 * HARD), MAX_OPS);
						release_memory();
					}
					else {
						//printf("ops %ld is smaller than %0.1lf %% of max ops %d, but only %lld sec elapsed\n", ops, (100 * HARD), MAX_OPS, dur_s);
					}
				}
				//printf("%8ld\t%8ld\t%8.1lf\n", epoch, ops, (position/(1024.0 * 1024.0)));
				printf("%8ld\t%8ld\t%8.1lf\n", epoch, ops, (position+1)*256.0);
			}
		}
		else 
			break;
	}
	printf("Max Ops: %ld\n", max_ops);
	pclose(_pipe);
	return 0;
}

int main() {
	int status, cmd;
	//exec_cmd(MEMTIER_CMD);
	status = init_mem_zone();
	
	if(status != 0)
		return status;
	
	while(scanf("%d", &cmd) != EOF) {
		if(cmd == 1) {
			grab_memory();
		}
		else if (cmd == 2) {
			release_memory();
		}
		else if (cmd == 3) {
			exec_cmd(MEMTIER_CMD);
		}
	}
	release_all();
	//free(addr);
	return 0;
}

