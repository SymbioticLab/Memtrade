/*
    C socket server example, handles multiple clients using threads
    Compile
    gcc server.c -lpthread -o server
*/
 
#include<stdio.h>
#include<string.h>    //strlen
#include<stdlib.h>    //strlen
#include<sys/socket.h>
#include<sys/time.h>
#include<arpa/inet.h> //inet_addr
#include<unistd.h>    //write
#include<pthread.h> //for threading , link with lpthread
#include <stdatomic.h>
#define PORT 9700 
#define MAX_PRODUCER 128
#define MAX_CONSUMER 128
#define MAX_CLIENT (MAX_PRODUCER + MAX_CONSUMER)
#define BUFFER_SIZE 4096
#define MAX_ID 4

enum role {
    PRODUCER = 0,
    CONSUMER = 1
};

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

struct producer_info_t {
	char ip[200];
	int port;
	int nslabs;
	int available_slabs;
	int id;
    int sock;
};

struct consumer_info_t {
	char ip[200];
	int port;
    int id;
    int assigned_slabs;
    char producer_map[4096]; //TODO: Update the max size or change it to a list
    int sock;
};

struct timeval ts[MAX_ID][4];

struct producer_info_t producer_list[MAX_PRODUCER + 2];
struct consumer_info_t consumer_list[MAX_CONSUMER + 2];

atomic_int client_id = ATOMIC_VAR_INIT(0);
atomic_int consumer_id = ATOMIC_VAR_INIT(0);
atomic_int producer_id = ATOMIC_VAR_INIT(0);


void time_stamp(int id, int i){
	gettimeofday(&ts[id][i], NULL);
	//clock_gettime(CLOCK_MONOTONIC, ts + i);
}
 
uint32_t time_cal(int id, int start, int end) {
    return (uint32_t)((long)(ts[id][end].tv_sec - ts[id][start].tv_sec) * 1000000 + ts[id][end].tv_usec - ts[id][start].tv_usec);
}

int cmp(const void *a,const void *b) {
    return(*(uint32_t *)a-*(uint32_t *)b);
}

//the thread function
void *connection_handler(void *);

void ip_parser(char* msg, char *ip, int* port) {
	//portal format 1,192.168.0.12:8000
	char* ptr = msg, s[] = ",:";
	char* token = strtok(ptr, s);
	int token_count = 0, msg_type;
	
	while( token != NULL ) {
		if(token_count == 0) {
			msg_type = atoi(token);
		}
		else if(token_count == 1) {
			while(*token)
				(*ip++)=(*token++);
			*ip = '\0';
		}
		else if(token_count == 2){
			*port = atoi(token);
		}
		token = strtok(NULL, s);
		token_count++;
	}
}

/* message process related functions */

void send_connection_ack(int sock) {
	char ack_msg[100];
	sprintf(ack_msg, "%d", CONNECTION_ACK);
	write(sock, ack_msg, sizeof(ack_msg));
}

void send_register_ack(int sock, int id) { 
	char ack_msg[100];
	sprintf(ack_msg, "%d,%d", REGISTRATION_ACK, id);
	write(sock, ack_msg, sizeof(ack_msg));
}

void send_producer_ready_msg(int producer_id, int consumer_id) {
	char msg[200];
	sprintf(msg, "%d,%d,%d", PRODUCER_READY, producer_id, consumer_id);
	write(consumer_list[consumer_id].sock, msg, sizeof(msg));
}

void register_client(char* ip, int port, int role, int sock) {
    if(role == PRODUCER) {
        int p_id = atomic_fetch_add_explicit(&producer_id, 1, memory_order_acquire);
        memcpy(producer_list[p_id].ip, ip, sizeof(ip));
        producer_list[p_id].port = port;
        producer_list[p_id].id = p_id;
        producer_list[p_id].sock = sock;
        send_register_ack(sock, p_id);
    }
    else if (role == CONSUMER) {
        int c_id = atomic_fetch_add_explicit(&consumer_id, 1, memory_order_acquire);
        memcpy(consumer_list[c_id].ip, ip, sizeof(ip));
        consumer_list[c_id].port = port;
        consumer_list[c_id].id = c_id;
        consumer_list[c_id].assigned_slabs = 0;
        consumer_list[c_id].sock = sock;
        send_register_ack(sock, c_id);
    }
}

void handle_message(char* msg, int sock) {
	int type, port, spot_size, lease_time, client_id;
    int producer_id, consumer_id;
	char ip[200], role[10];
	
	sscanf(msg, "%d,", &type);
	printf("Message type: %d\n", type);
	switch (type) {
		case PRODUCER_REG:
			ip_parser(msg, ip, &port);
			printf("Message type: %d, ip: %s, port: %d\n", type, ip, port);
			break;
		case CONSUMER_REG:
			ip_parser(msg, ip, &port);
			printf("Message type: %d, ip: %s, port: %d\n", type, ip, port);
			register_client(ip, port, CONSUMER, sock);
			break;
		case SPOT_REQUEST:
			sscanf(msg, "%d,%d,%d,%d", &type, &client_id, &spot_size, &lease_time);
			printf("Message type: %d, client id: %d, spot size: %d, lease time:%d\n", type, client_id, spot_size, lease_time);
            //TODO: find producer map
			break;
        case PRODUCER_READY:
            sscanf(msg, "%d,%d,%d,%d", &type, &producer_id, &consumer_id);
			printf("Message type: %d, from producer: %d to consumer %d\n", type, producer_id, consumer_id);
			send_producer_ready_msg(producer_id, consumer_id);
			break;
        default:
            break;
	}
}
 
int main(int argc , char *argv[]){
    int socket_desc, client_sock, c;
    struct sockaddr_in server, client;
     
    //Create socket
    socket_desc = socket(AF_INET , SOCK_STREAM , 0);
    if (socket_desc == -1){
        printf("Could not create socket");
    }
    puts("Socket created");
     
    //Prepare the sockaddr_in structure
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons( PORT );
     
    //Bind
    if( bind(socket_desc,(struct sockaddr *)&server , sizeof(server)) < 0) {
        //print the error message
        perror("bind failed. Error");
        return 1;
    }
    puts("bind done");
     
    //Listen
    listen(socket_desc , 3);
     
    //Accept and incoming connection
    puts("Waiting for incoming connections...");
    c = sizeof(struct sockaddr_in);
     
     
    //Accept and incoming connection
    puts("Waiting for incoming connections...");
    c = sizeof(struct sockaddr_in);
    pthread_t thread_id;
	
    while( (client_sock = accept(socket_desc, (struct sockaddr *)&client, (socklen_t*)&c)) ){
        puts("Connection accepted");
         
        if( pthread_create( &thread_id , NULL,  connection_handler , (void*) &client_sock) < 0){
            perror("could not create thread");
            return 1;
        }
         
        //Now join the thread , so that we dont terminate before the thread
//        pthread_join( thread_id , NULL);
        puts("Handler assigned");
    }
     
    if (client_sock < 0){
        perror("accept failed");
        return 1;
    }
     
    return 0;
}
 
/*
 * This will handle connection for each client
 * */
void *connection_handler(void *socket_desc) {
    //Get the socket descriptor
    int sock = *(int*)socket_desc;
    int read_size;
    char client_message[BUFFER_SIZE];

    send_connection_ack(sock);
    
    //Receive a message from client
    while( (read_size = recv(sock , client_message , BUFFER_SIZE , 0)) > 0 ){
	    handle_message(client_message, sock);
    } 

    return 0;
} 
