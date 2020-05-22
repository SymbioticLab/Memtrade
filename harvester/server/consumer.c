// Client side C/C++ program to demonstrate Socket programming 
#include <stdio.h> 
#include <sys/socket.h> 
#include <stdlib.h> 
#include <netinet/in.h> 
#include <string.h>
#include <unistd.h>

#define BROKER_IP "128.105.144.197"
#define BROKER_PORT 9700 
#define CONSUMER_IP "128.105.144.197"
#define CONSUMER_PORT 9704 
#define MAX_CLIENT 128
#define MAX_PRODUCER 128
#define PAGE_SIZE 4096
#define BUFFER_SIZE 8192
#define SPOT_SIZE 5 // interms of GB
#define LEASE_TIME 1 // interms of hour
#define REMOTE_RATIO 0 //all local; ranges from 0-10; multiple of 10%; eg, 3 means 30% in remote

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

struct producer_info_t {
	char ip[200];
	int port;
	int nslabs;
	int id;
	int manager_state;
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
	int spot_size;
	int lease_time;
	int remote_ratio;
	struct producer_info_t producer_list[MAX_PRODUCER + 2];
} consumer;

void run_consumer_redis();
void run_consumer_app(int producer_id);

void portal_parser(char* msg) {
	//portal format 1,2,192.168.0.12:8000:10,192.168.0.11:9400:20
	//<msg_type>,<producer_count>,<ip:port:slab_size:id>, ...

	char ptr[1024], s[] = ",:", addr[200];
	memcpy(ptr, msg, strlen(msg));
	char* token = strtok(ptr, s);
	int token_count = 0, type, producer_count = 0, port, size, id;
	
	while( token != NULL ) {
		if(token_count == 0) {
			type = atoi(token);
		}
		else if(token_count == 1) {
			producer_count = atoi(token);
		}
		else if(token_count % 4 == 2) {
			memcpy(addr, token, strlen(token));
		}
		else if(token_count % 4 == 3){
			port = atoi(token); 
		}
		else if(token_count % 4 == 0){
			size = atoi(token); 
		}
		else {
			id = atoi(token);
			consumer.producer_list[id].id = id;
			consumer.producer_list[id].port = port;
			consumer.producer_list[id].nslabs = consumer.producer_list[id].nslabs + size;
			memcpy(consumer.producer_list[id].ip, addr, strlen(addr));
			printf("Msg type: %d, producer ip: %s, port: %d, slab size: %d, id: %d\n", type, addr, port, size, id);
		}
		token = strtok(NULL, s);
		token_count++;
	}
}

void send_registration_msg() {
	char msg[200];
	sprintf(msg, "%d,%s,%d", CONSUMER_REG, consumer.ip, consumer.port);
	write(broker.sock, msg, sizeof(msg));
}

void send_spot_request() {
	char msg[200];
	sprintf(msg, "%d,%d,%d,%d", SPOT_REQUEST, consumer.id, consumer.spot_size, consumer.lease_time);
	write(broker.sock, msg, sizeof(msg));
}

void handle_message(char* msg) {
	int type, i, producer_id, consumer_id;
	
	sscanf(msg, "%d,", &type);
	printf("Message type: %d, %s\n", type, msg);
	switch (type) {
		case CONNECTION_ACK:
			send_registration_msg();
			break;
		case REGISTRATION_ACK:
			sscanf(msg, "%d,%d", &type, &consumer.id);
			printf("Message type: %d, id at broker: %d\n", type, consumer.id);
			send_spot_request();
			break;
		case SPOT_ASSIGNMENT_CONSUMER:
			portal_parser(msg);
			for(i = 0; i < MAX_PRODUCER; i++ ) {
				if(consumer.producer_list[i].nslabs != 0) {
					printf("#%d <ip:port:size> = <%s:%d:%d>\n", i, consumer.producer_list[i].ip, consumer.producer_list[i].port, consumer.producer_list[i].nslabs);
				}
			}
		case PRODUCER_READY:
            sscanf(msg, "%d,%d,%d", &type, &producer_id, &consumer_id);
			printf("Message type: %d, from producer: %d to consumer %d\n", type, producer_id, consumer_id);
			consumer.producer_list[producer_id].manager_state = RUNNING;
			//TODO: check for the correctness of the remote-local ratio; especially when multiple producers are mapped for a single request 
			run_consumer_app(producer_id);
			break;
		default:
			break;
	}
	
}

void run_consumer_app(int producer_id) {
	char consumer_cmd[400];
	sprintf(consumer_cmd, "cd /newdir/spot/YCSB && ./bin/ycsb load redis -s -P workloads/workloada -p \"redis.consumer_host=127.0.0.1\" -p \"redis.consumer_port=6379\"  -p \"redis.host=%s\" -p \"redis.port=%d\" 2>&1 | tee /newdir/ycsb.txt", consumer.producer_list[producer_id].ip, consumer.producer_list[producer_id].port);

	printf("%s\n", consumer_cmd);
	FILE* _pipe = popen(consumer_cmd, "r");
	//TODO: check the status of the application from the _pipe
}

void run_consumer_redis() {
	char *redis_cmd = "ps -aux | grep redis-server | grep -v grep | awk '{ print $2 }' | xargs kill -9 && /newdir/spot/redis/src/redis-server /newdir/spot/redis/redis.conf";
	printf("%s\n", redis_cmd);
	FILE* _pipe = popen(redis_cmd, "r");
	//TODO: check redis status from the _pipe
}

void init() {
	int i;

	strcpy(broker.ip, BROKER_IP);
	broker.port = BROKER_PORT;
	strcpy(consumer.ip, CONSUMER_IP);
	consumer.port = CONSUMER_PORT;
	consumer.spot_size = SPOT_SIZE;
	consumer.lease_time = LEASE_TIME;
	consumer.remote_ratio = REMOTE_RATIO;

	for(i=0; i<MAX_PRODUCER; i++) {
		consumer.producer_list[i].nslabs = 0;
		consumer.producer_list[i].manager_state = STOP;
	}

	run_consumer_redis();
}

void usage() {
	printf("Usage ./client [-b broker-ip] [-p broker-port] [-c consumer-ip] [-q consumer-port] [-s spot-size] [-t lease-time] [-r remote-ratio]\n");
	printf("Default broker ip:port is %s:%d, consumer ip:port is %s:%d\n", BROKER_IP, BROKER_PORT, CONSUMER_IP, CONSUMER_PORT);
	printf("\n");
}

int main(int argc, char *argv[]) { 
	struct sockaddr_in address; 
	int i, len, opt; 
	struct sockaddr_in serv_addr; 
	char buffer[BUFFER_SIZE] = {0}; 

	init();

	while ((opt = getopt(argc, argv, "hb:p:c:q:s:t:r")) != -1) {
		switch (opt) {
		case 'h':
			usage();
			return 0;
		case 'b':
			strcpy(broker.ip, optarg);
			break;
		case 'p':
			broker.port = atoi(optarg);
			break;
		case 'c':
			strcpy(consumer.ip, optarg);
			break;
		case 'q':
			consumer.port = atoi(optarg);
			break;
		case 's':
			consumer.spot_size = atoi(optarg);
			break;
		case 't':
			consumer.lease_time = atoi(optarg);
			break;
		case 'r':
			consumer.remote_ratio = atoi(optarg);
			break;
		}
	}

	if ((broker.sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) { 
		printf("\n Socket creation error \n"); 
		return -1; 
	} 
	memset(&serv_addr, '0', sizeof(serv_addr)); 

	serv_addr.sin_family = AF_INET; 
	serv_addr.sin_port = htons(broker.port); 
	
	// Convert IPv4 and IPv6 addresses from text to binary form 
	if(inet_pton(AF_INET, broker.ip, &serv_addr.sin_addr)<=0) { 
		printf("\nInvalid address/ Address not supported \n"); 
		return -1; 
	} 

	if (connect(broker.sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) { 
		printf("\nConnection Failed \n"); 
		return -1; 
	} 
	printf("connect done\n");

	while((len = recv(broker.sock , buffer , BUFFER_SIZE , 0)) > 0) {
		//message received from the server
		handle_message(buffer);
	}
	return 0; 
} 
