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
#define MIN_FREE 1 //Minimum of 1GB free in producer

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

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

atomic_int consumer_id = ATOMIC_VAR_INIT(0);
atomic_int producer_id = ATOMIC_VAR_INIT(0);

pthread_mutex_t lock; 

void find_placement(int client_id, int spot_size, int lease_time);

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

int comparator(const void* p, const void* q) { 
	return ((struct producer_info_t*)p)->available_slabs < ((struct producer_info_t*)q)->available_slabs; 
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

void send_assignment_msg(int id, char* msg, int role) {
    if(role == PRODUCER){
    	write(producer_list[id].sock, msg, strlen(msg));
    }
    else if(role == CONSUMER){
        write(consumer_list[id].sock, msg, strlen(msg));
    }
}

void send_producer_ready_msg(int producer_id, int consumer_id) {
	char msg[200];
	sprintf(msg, "%d,%d,%d", PRODUCER_READY, producer_id, consumer_id);
	write(consumer_list[consumer_id].sock, msg, sizeof(msg));
}

void register_client(char* ip, int port, int role, int sock) {
    if(role == PRODUCER) {
        int p_id = atomic_fetch_add_explicit(&producer_id, 1, memory_order_acquire);
        memcpy(producer_list[p_id].ip, ip, strlen(ip));
        producer_list[p_id].port = port;
        producer_list[p_id].id = p_id;
        producer_list[p_id].nslabs = 0;
        producer_list[p_id].available_slabs = 0;
        producer_list[p_id].sock = sock;
	printf("producer registered with ip:port %s:%d\n", producer_list[p_id].ip, producer_list[p_id].port = port);
        send_register_ack(sock, p_id);
    }
    else if (role == CONSUMER) {
        int c_id = atomic_fetch_add_explicit(&consumer_id, 1, memory_order_acquire);
        memcpy(consumer_list[c_id].ip, ip, strlen(ip));
        consumer_list[c_id].port = port;
        consumer_list[c_id].id = c_id;
        consumer_list[c_id].assigned_slabs = 0;
        consumer_list[c_id].sock = sock;
	printf("consumer registered with ip:port %s:%d\n", consumer_list[c_id].ip, consumer_list[c_id].port);
        send_register_ack(sock, c_id);
    }
}

void handle_message(char* msg, int sock) {
	int type, port, spot_size, lease_time, client_id, nslab, available_slab;
    int producer_id, consumer_id;
	char ip[200], role[10];
	
	sscanf(msg, "%d,", &type);
	printf("Message type: %d, %s\n", type, msg);
	switch (type) {
		case PRODUCER_REG:
			ip_parser(msg, ip, &port);
			printf("Message type: %d, ip: %s, port: %d\n", type, ip, port);
            register_client(ip, port, PRODUCER, sock);
			break;
		case CONSUMER_REG:
			ip_parser(msg, ip, &port);
			printf("Message type: %d, ip: %s, port: %d\n", type, ip, port);
			register_client(ip, port, CONSUMER, sock);
			break;
        case PRODUCER_AVAILABILITY:
            sscanf(msg, "%d,%d,%d,%d", &type, &producer_id, &available_slab, &nslab);
            producer_list[producer_id].nslabs = nslab;
            producer_list[producer_id].available_slabs = available_slab;
            break;
		case SPOT_REQUEST:
			sscanf(msg, "%d,%d,%d,%d", &type, &client_id, &spot_size, &lease_time);
			printf("Message type: %d, client id: %d, spot size: %d, lease time:%d\n", type, client_id, spot_size, lease_time);
            find_placement(client_id, spot_size, lease_time);
			break;
        case PRODUCER_READY:
            sscanf(msg, "%d,%d,%d", &type, &producer_id, &consumer_id);
			printf("Message type: %d, from producer: %d to consumer %d\n", type, producer_id, consumer_id);
			send_producer_ready_msg(producer_id, consumer_id);
			break;
        default:
            break;
	}
}

void find_placement(int consumer_id, int spot_size, int lease_time) {
    //TODO: make thread-safe
    pthread_mutex_lock(&lock);
    
    int i, count = 0, allocated = 0, p_id, p_count = 0;
    struct producer_info_t temp_producers[MAX_PRODUCER + 2];

    for(i = 0, count =0; i<MAX_PRODUCER; i++) {
        if(producer_list[i].available_slabs != 0) {
            memcpy(&temp_producers[count], &producer_list[i], sizeof(struct producer_info_t));
            count++;
        }
    }

    qsort(temp_producers, count, sizeof(struct producer_info_t), comparator);

    for(i=0; i< count && allocated < spot_size; i++) {
        char producer_assignment[1024], consumer_assignment[1024];
        int has_picked = 0, p_alloc = 0;

        p_id = temp_producers[i].id;
        if(temp_producers[i].available_slabs >= (spot_size-allocated)) {
            p_alloc = spot_size-allocated;
            producer_list[p_id].available_slabs = MAX(0, temp_producers[i].available_slabs - p_alloc);
            allocated += p_alloc;
            has_picked = 1;
        }
        else {
            p_alloc = spot_size - (temp_producers[i].available_slabs - MIN_FREE);
            if(p_alloc >= 0) {
                producer_list[p_id].available_slabs = MAX(0, temp_producers[i].available_slabs - p_alloc);
                allocated += p_alloc;
                has_picked = 1;
            }
        }

        if(has_picked == 1) {
            p_count++;
            //<msg_type>,<consumer_count>,<ip:port:slab_size:id>, ...
            sprintf(producer_assignment, "%d,%d,%s:%d:%d:%d\n", SPOT_ASSIGNMENT_PRODUCER, 1, consumer_list[consumer_id].ip, consumer_list[consumer_id].port, p_alloc, consumer_list[consumer_id].id);
            sprintf(consumer_assignment, "%d,%d,%s:%d:%d:%d\n", SPOT_ASSIGNMENT_CONSUMER, 1, producer_list[p_id].ip, producer_list[p_id].port, p_alloc, producer_list[p_id].id);
            send_assignment_msg(p_id, producer_assignment, PRODUCER);
            send_assignment_msg(consumer_id, consumer_assignment, CONSUMER);
		printf("producer: %s consumer: %s\n", producer_assignment, consumer_assignment);
        }
    }
    pthread_mutex_unlock(&lock);
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
