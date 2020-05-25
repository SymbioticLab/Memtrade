#ifndef __CONSUMERCLI_H_
#define __CONSUMERCLI_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <assert.h>
#include <stdatomic.h>
#include <arpa/inet.h>
// #include <openssl/rand.h>
// #include <openssl/sha.h>
// #include <openssl/evp.h>
// #include <openssl/err.h>
// #include <openssl/engine.h>
// #include <sys/types.h>

// #include <hiredis.h>
// #include <async.h>
// #include <aes.h>
// #include <adapters/libevent.h>

#define BROKER_IP "192.168.122.91"
#define BROKER_PORT 9700 
#define PRODUCER_IP "192.168.122.141"
#define PRODUCER_PORT 9702 
#define MANAGER_PORT_INIT 9704 
#define MAX_CONSUMER 128
#define PAGE_SIZE 4096
#define BUFFER_SIZE 4096
#define SPOT_SIZE 5 // interms of GB
#define LEASE_TIME 1 // interms of hour

enum msg_type {
	CONNECTION_ACK = 0,
	PRODUCER_REG = 1,
	CONSUMER_REG = 2,
	REGISTRATION_ACK = 3,
    PRODUCER_AVAILABILITY = 4,
	SPOT_REQUEST = 5,
	SPOT_ASSIGNMENT_CONSUMER = 6,
    SPOT_ASSIGNMENT_PRODUCER = 7,
	PRODUCER_READY = 8
};

enum manager_state {
	STOP = 0,
	RUNNING = 1
};

enum producer_state {
    INIT = 0,
	CONNECTED = 1,
	REGISTERED = 2
};

struct consumer_info_t {
	char ip[200];
	int port;
	int id;
	int nslabs;
	int manager_state;
    int manager_port;
};

struct {
	char ip[200];
	int port;
	int sock;
} broker;

struct {
	char ip[200];
	int port;
	int id;
	int consumer_count;
	char cgroup_name[20];
    long long total_memory;
    long long harvested_memory;
    long long est_available_memory;
    int status;
	struct consumer_info_t consumer_list[MAX_CONSUMER + 2];
} producer;

#define MAX_HARVESTED_LEN 512
#define CGROUP_PATH_MAX_LEN 256
#define PAGE_SHIFT 12
#define MIN_SPOT_SIZE (50l << 20)
#define SLEEP_TIME 60000000 // in microseconds 1sec = 1000000 us
#define UNUSED(x) ((void)(x))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

// redisContext* producer_server;
// redisContext* coordinator_server;
// redisContext* connect_server(const char *addr, int port, struct timeval timeout);

const float g_ewma_beta = 0.2;
const long long g_alloc_threshold = (512l << 20);
const long long g_evict_threshold = (512l << 20);
const long long g_node_size = (64l << 20);

long long get_total_memory_size() {
    FILE* mem_info = fopen("/proc/meminfo", "r");
    if (!mem_info) {
        printf("cannot open meminfo file\n");
        exit(1);
    }
    char key[512], unit[4];
    long shift;
    long long size;
    while( fscanf(mem_info, "%s %lld %s", key, &size, unit) != EOF ) {
//        printf("%s %ld %s\n", key, size, unit);
        if(strcmp(key, "MemTotal:") == 0 ) {
            switch (unit[0]) {
            case 'k':
            case 'K':
                shift = 10;
                break;
            case 'm':
            case 'M':
                shift = 20;
                break;
            case 'g':
            case 'G':
                shift = 30;
                break;
            default:
                shift = 1;
            }
//          printf("total memory size: %lld bytes\n", size * shift);
            goto out;
        }
    }
    printf("cannot read meminfo file\n");
    size = 0;
out:
    fclose(mem_info);
    return size << shift;
}

long long get_free_memory_size() {
    FILE* mem_info = fopen("/proc/meminfo", "r");
    if (!mem_info) {
        printf("cannot open meminfo file\n");
        exit(1);
    }
    char key[512], unit[4];
    long size, shift;
    while( fscanf(mem_info, "%s %ld %s", key, &size, unit) != EOF ) {
//        printf("%s %ld %s\n", key, size, unit);
        if(strcmp(key, "MemFree:") == 0 ) {
            switch (unit[0]) {
            case 'k':
            case 'K':
                shift = 10;
                break;
            case 'm':
            case 'M':
                shift = 20;
                break;
            case 'g':
            case 'G':
                shift = 30;
                break;
            default:
                shift = 1;
            }
//            printf("total memory size: %lld bytes\n", size * shift);
            goto out;
        }
    }
    printf("cannot read meminfo file\n");
    size = 0;
out:
    fclose(mem_info);
    return size << shift;
}

long long get_cgroup_rss() {
    char cgroup_path[CGROUP_PATH_MAX_LEN];
#if 1
    sprintf(cgroup_path, "/sys/fs/cgroup/memory/%s/memory.stat", producer.cgroup_name);

    FILE* cgroup_stat = fopen(cgroup_path, "r");

    if (!cgroup_stat) {
        printf("cannot open cgroup stat file\n");
        exit(1);
    }

    char key[1024];
    long value, rss = 0;

    while( fscanf(cgroup_stat, "%s %ld", key, &value) != EOF) {
        if( (strcmp(key, "total_rss") == 0) || (strcmp(key, "total_mapped_file") == 0) || (strcmp(key, "total_cache") == 0) ) {
            rss += value;
        }
    }
//    printf("rss for %s cgroup: %ld\n", cgroup_name, rss);
    return rss;
#endif
#if 0
    return 0;
#endif
}

long long get_tswap_memory_size() {
    FILE* tswap_stat = fopen("/sys/kernel/tswap/tswap_stat", "r");
    if (!tswap_stat) {
        printf("cannot open tswap stat file\n");
        exit(1);
    }

    long nr_memory_page = 0;
    char key[1024];
    long value;

    while( fscanf(tswap_stat, "%s %ld", key, &value) != EOF) {
        if( (strcmp(key, "nr_zombie_page:") == 0) || (strcmp(key, "nr_in_memory_page:") == 0) || (strcmp(key, "nr_in_memory_zombie_page:") == 0) || (strcmp(key, "nr_in_flight_page:") == 0)) {
            nr_memory_page += value;
        }
    }
//    printf("tswap page size: %lld\n", nr_memory_page << PAGE_SHIFT);
    return nr_memory_page << PAGE_SHIFT;
}

long long get_available_memory() {
    return get_free_memory_size() + get_cgroup_rss();
}

long update_est_available_memory(long available_memory) {
    producer.est_available_memory = (long) (g_ewma_beta * available_memory + (1 - g_ewma_beta) * producer.est_available_memory);
    return MIN(producer.est_available_memory, available_memory);
}
#endif
