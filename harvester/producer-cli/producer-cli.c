#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <hiredis.h>
#include <async.h>
#include <aes.h>
#include <adapters/libevent.h>

#include "producer-cli.h"

redisContext* connect_server(const char *addr, int port, struct timeval timeout) {
    redisContext *c = redisConnectWithTimeout(addr, port, timeout);

    if (c == NULL || c->err) {
        if (c) {
            printf("Connection error: %s\n", c->errstr);
            redisFree(c);
        } else {
            printf("Connection error: can't allocate redis context\n");
        }
        exit(1);
    }
    else
        printf("client connected\n");

    return c;
}

void check_server(redisContext* server){
    redisReply *reply = redisCommand(server,"PING");
    printf("PING: %s\n", reply->str);
    freeReplyObject(reply);
}

int set(char* key, char* data, redisContext *server) {
    redisReply *reply = redisCommand(server,"SET %s %s", key, data);
    printf("SET: %s\n", reply->str);
    freeReplyObject(reply);
    return 1;
}

int get(char* key, char* data, redisContext *server) {
    redisReply *reply = redisCommand(server,"GET %s", key);
    memcpy(data, reply->str, sizeof(reply->str));
    printf("GET: %s\n", data);
    freeReplyObject(reply);
    return 1;
}

int set_spot_size(int size, redisContext *server) {
    redisReply *reply = redisCommand(server,"CONFIG SET maxmemory %d", size);
    printf("CONFIG SET maxmemory: %s\n", reply->str);
    freeReplyObject(reply);
    return 1;
}

long get_spot_size(redisContext *server) {
    redisReply *reply = redisCommand(server,"CONFIG GET maxmemory");
    printf("CONFIG GET maxmemory: %s\n", reply->element[1]->str);
    freeReplyObject(reply);
    return atol(reply->element[1]->str);
}

int main (int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc != 2) {
        printf("Usage: <cgroup name>\n");
        exit(1);
    }
/*
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    producer_server = connect_server(PRODUCER_SERVER_IP, PRODUCER_SERVER_PORT, timeout);
    check_server(producer_server);
    
    char key[4] = "KEY1";
    char data[32] = "This a sample text, Length eq 32";
    set(key, data, producer_server);
    get(key, data, producer_server);

    long size;

    while(scanf("%ld", &size) != EOF) {
        set_spot_size(size, producer_server);
        get_spot_size(producer_server);
    }
    redisFree(producer_server);
*/
    g_cgroup_name = argv[1];
    printf("available memory :%ld\n", get_available_memory());

    long available_memory, cur_est_available_memory, evict_count, diff;

    while(1) {
        available_memory = get_available_memory();
        cur_est_available_memory = update_est_available_memory(available_memory);

        if (cur_est_available_memory - g_harvested_memory < g_evict_threshold) {
            diff = g_evict_threshold - (cur_est_available_memory - g_harvested_memory);
            evict_count = 0;

            while (diff > 0 && g_harvested_memory > MIN_SPOT_SIZE) {
                g_harvested_memory -= g_node_size;
                diff = g_evict_threshold - (cur_est_available_memory - g_harvested_memory);
                ++evict_count;
            }

            g_harvested_memory = MAX(g_harvested_memory, MIN_SPOT_SIZE);
            set_spot_size(g_harvested_memory, producer_server);

            printf("EVICT | available memory: %ld MB ", (available_memory >> 20));
            printf("estimated available memory: %ld MB ", (cur_est_available_memory >> 20));
            printf("allocated memory: %ld MB ", (g_harvested_memory >> 20));
            printf("evicted: %ld MB\n", ((evict_count * g_node_size) >> 20));
        }
        else if (cur_est_available_memory - g_harvested_memory > g_alloc_threshold) {
            g_harvested_memory += g_node_size;
            set_spot_size(g_harvested_memory, producer_server);

            printf("ALLOC | available memory: %ld MB ", (available_memory >> 20));
            printf("estimated available memory: %ld MB ", (cur_est_available_memory >> 20));
            printf("allocated memory: %ld MB ", (g_harvested_memory >> 20));
        } 
        else {
            printf("SKIP  | available memory: %ld MB ", (available_memory >> 20));
            printf("estimated available memory: %ld MB ", (cur_est_available_memory >> 20));
            printf("allocated memory: %ld MB\n", (g_harvested_memory >> 20));
        }
    }
    return 0;
}
