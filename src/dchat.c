#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include "conn-protocol.h"

#define CHATTER_LIMIT 500

typedef struct chatter {
	char *name;
	char *host;
	int port;
	char leader;
	time_t last_hb;
} Chatter;

void help();
void start_leader();
void start_client(char *);
void *listening_on_socket();

Chatter chatters[CHATTER_LIMIT]; // Chatters array
int nchatters = 0;
struct sockaddr_in listen_addr; // Listening address
int listen_sock; // Listening socket
pthread_t listen_thread; // Listening thread
struct sockaddr_in *leader_addr; // Leader's address

char leader;
char *username;

int main(int argc, char *argv[]) {
	char* addrport;
	if(argc < 2) { // Show help
		help();
		return 0;
	} else {
		username = argv[1];
		if(argc == 3) { // Joining
			leader = 0;
			addrport = argv[2];
		} else { // Creating
			leader = 1;
		}
	}
	// Create listening socket
	listen_addr = *make_sock_addr(LOOPBACK_STR, 0);
	listen_sock = create_socket(listen_addr);

	if(leader) {
		start_leader();
//		pthread_exit(NULL);
	} else {
		start_client(addrport);
	}
	return 0;
}

void help() {
	printf("dchat USER [ADDR:PORT]\n");
}

//void send_msg(int sock, struct sockaddr_in addr, char *msg) {
//	int nbytes;
//	int len = strlen(msg);
//	int size = sizeof(addr);
//
//	nbytes = sendto(sock, msg, len, 0, (struct sockaddr *) &addr, size);
//	if (nbytes < 0) {
//		perror("sendto (server)");
//		exit(EXIT_FAILURE);
//	}
//}
//
//char * receive_msg(int sock, char *buffer, int limit, struct sockaddr_in *addr) {
//	int nbytes;
//	socklen_t size = sizeof(struct sockaddr_in);
//	char *msg;
//
//	nbytes = recvfrom(sock, buffer, limit, 0, (struct sockaddr *) addr, &size);
//	if (nbytes < 0) {
////		perror("recfrom");
////		exit(EXIT_FAILURE);
//		return NULL;
//	}
//
//	msg = (char *) malloc(sizeof(char) * (nbytes + 1));
//	strncpy(msg, buffer, nbytes);
//	msg[nbytes] = '\0';
//
//	return msg;
//}

void start_leader() {
	Chatter leader;
	// Get listening port
	int l_port = get_port_number(listen_sock);
	char * ip_addr =

	printf("%s started a new chat, listening on %s:%d\n",
					username, LOOPBACK_STR, l_port);
	pthread_create(&listen_thread, NULL, listening_on_socket, NULL);
	pthread_join(listen_thread, NULL);
}

void encap_params(Request *req, int argn, ...) {
	va_list ap;
	int i;

	if(req == NULL) {
		return;
	}
	va_start(ap, argn);
	req->paramsn = argn;
	req->params = (char **) malloc(sizeof(char *) * argn);
	for(i = 0; i < argn; i++) {
		req->params[i] = (char *) va_arg(ap, char *);
	}
}

void start_client(char *addrport) {
	// Get listening port
	int lis_port = get_port_number(listen_sock);
	char *ldr_addr_str = strtok(addrport, ":");
	char *ldr_port_str = strtok(NULL, ":");
	int ldr_port;
	char *response;
	struct sockaddr_in ldr_addr;
	Request req;
	Response resp;

	if(ldr_port_str == NULL) {
		ldr_port = 80;
	} else {
		if(string_to_int(ldr_port_str, &ldr_port) < 0) {
			perror("Invalid port number");
			exit(EXIT_FAILURE);
		}
	}

	printf("%s joining a new chat on %s:%d, listening on %s:%d\n",
			username, ldr_addr_str, ldr_port, LOOPBACK_STR, lis_port);
	req.host = ldr_addr_str;
	req.port = ldr_port;
	req.req = "join";
	encap_params(&req, 2, LOOPBACK_STR, int_to_string(lis_port));

	if(send_request(&req, &resp) < 0) {
		printf("Sorry, no chat is active on %s:%d, try again later.\n",
				ldr_addr_str, ldr_port);
		printf("Bye.\n");
		exit(EXIT_FAILURE);
	}
}

void *(listening_on_socket()) {
	char *body = NULL;
	Request req;
	struct sockaddr_in r_addr;
	while(1) {
		if(recv_packet(listen_sock, &r_addr, &body) > 0) {
			parse_req_packet(body, &req);
			printf("Request: %s\n", req.req);
		}
	}
}
