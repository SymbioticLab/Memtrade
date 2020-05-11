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
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/engine.h>
#include <sys/types.h>

#include <hiredis.h>
#include <async.h>
#include <aes.h>
#include <adapters/libevent.h>

#define COORDINATOR_SERVER_IP   "127.0.0.1"
#define COORDINATOR_SERVER_PORT 6379

#define PRODUCER_SERVER_IP   "127.0.0.1"
#define PRODUCER_SERVER_PORT 6379

#define MAX_HARVESTED_LEN 512
#define CGROUP_PATH_MAX_LEN 256
#define PAGE_SHIFT 12
#define MIN_SPOT_SIZE (50l << 20)
#define SLEEP_TIME 1000000 // in microseconds 1sec = 1000000 us
#define UNUSED(x) ((void)(x))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

redisContext* producer_server;
redisContext* coordinator_server;
redisContext* connect_server(const char *addr, int port, struct timeval timeout);

long long g_total_memory;
long long g_est_available_memory;
long long g_harvested_memory;

char *g_cgroup_name;

const float g_ewma_beta = 0.2;
const long long g_alloc_threshold = (8l << 30);
const long long g_evict_threshold = (1l << 30);
const long long g_node_size = (512l << 20);

long long get_total_memory_size() {
    FILE* mem_info = fopen("/proc/meminfo", "r");
    if (!mem_info) {
        printf("cannot open meminfo file\n");
        exit(1);
    }
    char key[512], unit[4];
    long size, shift;
    while( fscanf(mem_info, "%s %ld %s", key, &size, unit) != EOF ) {
//        printf("%s %ld %s\n", key, size, unit);
        if(strcmp(key, "MemTotal:") == 0 ) {
            switch (unit[0]) {
            case 'k':
            case 'K':
                shift = 1000;
                break;
            case 'm':
            case 'M':
                shift = 1000000;
                break;
            case 'g':
            case 'G':
                shift = 1000000000;
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
    return size * shift;
}

long long get_cgroup_rss(char* cgroup_name) {
    char cgroup_path[CGROUP_PATH_MAX_LEN];
    sprintf(cgroup_path, "/sys/fs/cgroup/memory/%s/memory.stat", cgroup_name);

    FILE* cgroup_stat = fopen(cgroup_path, "r");

    if (!cgroup_stat) {
        printf("cannot open cgroup stat file\n");
        exit(1);
    }

    char key[1024];
    long value, rss = 0;

    while( fscanf(cgroup_stat, "%s %ld", key, &value) != EOF) {
        if( (strcmp(key, "rss") == 0) || (strcmp(key, "mapped_file") == 0) || (strcmp(key, "cache") == 0) ) {
            rss += value;
        }
    }
//    printf("rss for %s cgroup: %ld\n", cgroup_name, rss);
    return rss;
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
//    printf("total memory: %lld, rss: %lld, tswap: %lld, available: %lld\n",g_total_memory, get_cgroup_rss(g_cgroup_name), get_tswap_memory_size());
    return g_total_memory - get_cgroup_rss(g_cgroup_name) - get_tswap_memory_size();
}

long update_est_available_memory(long available_memory) {
    g_est_available_memory = (long) (g_ewma_beta * available_memory + (1 - g_ewma_beta) * g_est_available_memory);
    return MIN(g_est_available_memory, available_memory);
}
#endif
