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
#define GRAB_SIZE 1024 * ONE_MB
#define MAX_GRAB 180 * ONE_GB
#define MAX_NODE 200

char* mem_node[MAX_NODE];
char* addr;
long position;

int init_mem_zone() {
	int i = 0;
	for(i=0; i<MAX_NODE; i++) {
		mem_node[i] = NULL;
	}
	position = -1;

        return 0;
}

void grab_memory() {
	if(position >= MAX_NODE || ((position+1)*GRAB_SIZE) >= MAX_GRAB){
		return;
	}
	position++;
	mem_node[position] = (char*) malloc(GRAB_SIZE * sizeof(char));
	mlock(mem_node[position], GRAB_SIZE);
	return;
}

void release_memory() {
	if(position < 0 || mem_node[position] == NULL) {
		return;
	}
	munlock(mem_node[position], GRAB_SIZE);
	free(mem_node[position]);
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

int main() {
	int status, cmd, size, i;
	status = init_mem_zone();
	
	if(status != 0)
		return status;
	
	while(scanf("%d %d", &cmd, &size) != EOF) {
		if(cmd == 1) {
			for( i = 0; i < size; i++)
				grab_memory();
		}
		else if (cmd == 2) {
			for( i = 0; i < size; i++)
				release_memory();
		}
	}
	release_all();
	return 0;
}

