// Client side C/C++ program to demonstrate Socket programming 
#include <stdio.h> 
#include <sys/socket.h> 
#include <stdlib.h> 
#include <netinet/in.h> 
#include <string.h>
#include <unistd.h>

#define BROKER_IP "128.105.144.197"
#define BROKER_PORT 9700 
#define PRODUCER_IP "128.105.144.197"
#define PRODUCER_PORT 9702 
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

struct consumer_info_t {
	char ip[200];
	int port;
	int id;
	int nslabs;
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
	int consumer_count;
	struct consumer_info_t consumer_list[MAX_CONSUMER + 2];
} producer;

int nslab, available_slab;

void portal_parser(char* msg) {
	//portal format 1,2,192.168.0.12:8000:10:1,192.168.0.11:9400:20
	//<msg_type>,<consumer_count>,<ip:port:slab_size:id>, ...

	char* ptr = msg, s[] = ",:", *addr;
	char* token = strtok(ptr, s);
	int token_count = 0, type, consumer_count = 0, port, size, id;
	
	while( token != NULL ) {
		if(token_count == 0) {
			type = atoi(token);
		}
		else if(token_count == 1) {
			consumer_count = atoi(token);
			//TODO: check whether same consumer is added multiple time
			producer.consumer_count = producer.consumer_count + consumer_count;
		}
		else if(token_count % 4 == 2) {
			while(*token)
				(*addr++)=(*token++);
			*addr = '\0';
		}
		else if(token_count % 4 == 3){
			port = atoi(token); 
		}
		else if(token_count % 4 == 0){
			size = atoi(token); 
		}
		else {
			id = atoi(token);
			producer.consumer_list[id].id = id;
			producer.consumer_list[id].port = port;
			producer.consumer_list[id].nslabs = producer.consumer_list[id].nslabs + size;
			memcpy(producer.consumer_list[id].ip, addr, sizeof(addr));
			printf("Msg type: %d, consumer ip: %s, port: %d, spot request: %d, id: %d\n", type, addr, port, size, id);
		}
		token = strtok(NULL, s);
		token_count++;
	}
}

void send_registration_msg() {
	char msg[200];
	sprintf(msg, "%d,%s,%d", PRODUCER_REG, producer.ip, producer.port);
	write(broker.sock, msg, sizeof(msg));
}

void send_producer_availability_msg(){
	char msg[200];
	sprintf(msg, "%d,%d,%d,%d",PRODUCER_AVAILABILITY, producer.id, available_slab, nslab);
	write(broker.sock, msg, sizeof(msg));
}

void send_producer_ready_msg(int consumer_id) {
	char msg[200];
	sprintf(msg, "%d,%d,%d", PRODUCER_READY, producer.id, consumer_id);
	write(broker.sock, msg, sizeof(msg));
}

void run_spot_manager(int consumer_id) {
	char *producer_cmd;

	if(producer.consumer_list[consumer_id].manager_state == RUNNING) {
		send_producer_ready_msg(consumer_id);
	}
	else {
		char redis_cmd[400];
		sprintf(redis_cmd, "ps -aux | grep redis-server | grep -v grep | awk '{ print $2 }' | xargs kill -9 && /newdir/spot/redis/src/redis-server --bind %s --port %d --save \"\"", producer.ip, producer.port);

		FILE* _pipe = popen(redis_cmd, "r");
		//TODO: check redis status from the _pipe
		producer.consumer_list[consumer_id].manager_state = RUNNING;
		send_producer_ready_msg(consumer_id);
	}
}

void handle_message(char* msg) {
	int type, i;
	
	sscanf(msg, "%d,", &type);
	printf("Message type: %d\n", type);
	switch (type) {
		case CONNECTION_ACK:
			send_registration_msg();
			break;
		case REGISTRATION_ACK:
			sscanf(msg, "%d,%d", &type, &producer.id);
			printf("Message type: %d, id at broker: %d\n", type, producer.id);
			send_producer_availability_msg();
			break;
		case SPOT_ASSIGNMENT_PRODUCER:
			portal_parser(msg);
			for(i = 0; i < producer.consumer_count; i++ ) {
				if(producer.consumer_list[i].nslabs != 0) {
					printf("#%d <ip:port:size> = <%s:%d:%d>\n", i, producer.consumer_list[i].ip, producer.consumer_list[i].port, producer.consumer_list[i].nslabs);
					run_spot_manager(producer.consumer_list[i].id);
				}
			}
		default:
			break;
	}
}

void usage() {
	printf("Usage ./producer [-b broker-ip] [-p broker-port] [-c producer-ip] [-q producer-port]\n");
	printf("Default broker ip:port is %s:%d, producer ip:port is %s:%d\n", BROKER_IP, BROKER_PORT, PRODUCER_IP, PRODUCER_PORT);
	printf("\n");
}

void init() {
	int i; 

	strcpy(broker.ip, BROKER_IP);
	broker.port = BROKER_PORT;
	strcpy(producer.ip, PRODUCER_IP);
	producer.port = PRODUCER_PORT;

	producer.consumer_count = MAX_CONSUMER;
	for(i=0; i<producer.consumer_count; i++) {
		producer.consumer_list[i].nslabs = 0;
		producer.consumer_list[i].manager_state = STOP;
	}
	nslab = 40;
	available_slab = 20;
}

int main(int argc, char *argv[]) { 
	struct sockaddr_in address; 
	int i, len, opt; 
	struct sockaddr_in serv_addr; 
	char buffer[BUFFER_SIZE] = {0}; 

	init();

	while ((opt = getopt(argc, argv, "hb:p:c:q")) != -1) {
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
			strcpy(producer.ip, optarg);
			break;
		case 'q':
			producer.port = atoi(optarg);
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
	printf("producer's connection to broker successful\n");

	while((len = recv(broker.sock , buffer , BUFFER_SIZE , 0)) > 0) {
		//message received from the server
		handle_message(buffer);
	}
	return 0; 
} 
