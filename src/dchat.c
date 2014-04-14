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
#define REQ_JOIN "join"
#define REQ_BROADCAST "broadcast"
#define REQ_MESSAGE "message"
#define REQ_QUIT "quit"

typedef struct chatter {
	char *name;
	char *host;
	int port;
	char leader;
	time_t last_hb;
} Chatter;

void help();
char check_duplicate_name(const char *);
void start_leader();
void start_client(char *);
void *(listening_for_requests());
void print_current_chatters();
void process_broadcast();
void add_broadcast(char *);
void handle_request(const Request, Response *);
void handle_join(const Request, Response *);
void handle_message(const Request, Response *);
int encode_chatters(char **);
int decode_chatters(char *);
Chatter * decode_chatter(char *);
void start_input();
void *(listening_for_broadcasts());
void handle_broadcast(const Request, Response *);

Chatter chatters[CHATTER_LIMIT]; // Chatters array
int nchatters = 0;
struct sockaddr_in listen_addr; // Listening address
int listen_sock; // Listening socket
char *broadcast_msg;// Broadcast message
pthread_t listen_thread; // Listening thread
struct sockaddr_in *leader_addr; // Leader's address

char leader;
char *username;

int main(int argc, char *argv[]) {
	char* addrport = NULL;
	int l_port = 0;
	int i;
	char *arg;
	char f_port = 0;
	if(argc < 2) { // Show help
		help();
		return 0;
	} else {
		for(i = 1; i < argc; i++) {
			arg = argv[i];
			if(f_port) {
				f_port = 0;
				if(string_to_int(arg, &l_port) < 0) {
					perror("Invalid port number");
					exit(EXIT_FAILURE);
				}
			} else if(strcmp(arg, "-p") == 0) {
				f_port = 1;
			} else if(strstr(arg, ":") != NULL) {
				addrport = arg;
			} else {
				username = arg;
			}
		}
		leader = (addrport == NULL) ? 1 : 0;
	}
	// Create listening socket
	listen_addr = *make_sock_addr(LOOPBACK_STR, l_port);
	listen_sock = create_socket(listen_addr);

	if(leader) {
		start_leader();
	} else {
		start_client(addrport);
	}
	return 0;
}

void help() {
	printf("dchat NAME [ADDR:PORT]\n");
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

char * get_message_string(const char *name, const char *msg) {
	char *res;
	int len = 0;
	len += strlen(name);
	len += strlen(": ");
	len += strlen(msg);
	res = (char *) malloc(sizeof(char) * (len + 1));
	strcat(res, name);
	strcat(res, ": ");
	strcat(res, msg);
	res[len] = '\0';
	return res;
}

void start_input() {
	char *input = NULL;
	size_t size;
	Request req;
	Response resp;
	int len;
	while(1) {
		getline(&input, &size, stdin);
		len = strlen(input);
		if(len > 0) {
			input[len - 1] = '\0'; // Remove new line
		}
		if(strlen(input) == 0) {
			continue;
		}
		if(leader) {
			add_broadcast(get_message_string(username, input));
			process_broadcast();
		} else {
			req.req = REQ_MESSAGE;
			encap_params(&req, 2, username, input);
			send_request(*leader_addr, &req, &resp);
		}
	}
}

void start_leader() {
	Chatter leader;
	// Get listening port
	int l_port = get_port_number(listen_sock);
	char *ip_addr = get_host_addr(listen_addr);
	// Add leader into chatters
	leader.name = username;
	leader.host = ip_addr;
	leader.port = l_port;
	leader.leader = 1;
	leader.last_hb = 0;
	chatters[nchatters++] = leader;

	printf("%s started a new chat, listening on %s:%d\n",
					username, LOOPBACK_STR, l_port);
	printf("Succeeded, current users:\n");
	print_current_chatters();
	printf("Waiting for others to join...\n");
	// Start listening thread
	pthread_create(&listen_thread, NULL, listening_for_requests, NULL);
//	pthread_join(listen_thread, NULL);

	start_input();
}

void print_current_chatters() {
	int i;
	Chatter chatter;

	for(i = 0; i < nchatters; i++) {
		chatter = chatters[i];
		printf("%s %s:%d%s\n", chatter.name, chatter.host,
				chatter.port, chatter.leader ? " (Leader)" : "");
	}
}

void *(listening_for_requests()) {
	char *body = NULL;
	Request req;
	Response resp;
	struct sockaddr_in r_addr;
	while(1) {
		if(recv_packet(listen_sock, &r_addr, &body) > 0) {
//			printf("Body: %s\n", body);
			if(parse_req_packet(body, &req) < 0) {
				printf("Cannot parse packet body: %s\n", body);
				continue;
			}
			handle_request(req, &resp);
			resp.ack = req.seq; // Set ack to request's seq
			send_response(r_addr, &resp); // Send response
			process_broadcast(); // Process broadcast messages
		} else {
			perror("recv_packet");
		}
	}
}

void handle_request(const Request req, Response *resp) {
	if(strcmp(req.req, REQ_JOIN) == 0) { // New chatter joining
		handle_join(req, resp);
	} else if(strcmp(req.req, REQ_MESSAGE) == 0) { // Sending message
		handle_message(req, resp);
	}
}

void handle_join(const Request req, Response *resp) {
	Chatter new_ctr;
	time_t t;
	char *msg;
	char *name;
	char bc_msg[100];

	name = req.params[0];
	if(check_duplicate_name(name)) {
		resp->status = -2;
		resp->body = NULL;
		return;
	}

	new_ctr.name = name;
	new_ctr.host = req.params[1];
	if(string_to_int(req.params[2], &new_ctr.port) < 0) {
		perror("Invalid port");
		return;
	}
	new_ctr.leader = 0;
	time(&t);
	new_ctr.last_hb = t;
	chatters[nchatters++] = new_ctr;
	encode_chatters(&msg);

	resp->status = 0;
	resp->body = msg;

	sprintf(bc_msg, "NOTICE %s joined on %s:%d",
			new_ctr.name, new_ctr.host, new_ctr.port);
	add_broadcast(bc_msg);
}

void handle_message(const Request req, Response *resp) {
	char *msg;

	resp->status = 0;
	resp->body = NULL;
	msg = get_message_string(req.params[0], req.params[1]);
	add_broadcast(msg);
}

char check_duplicate_name(const char *name) {
	int i;
	for(i = 0; i < nchatters; i++) {
		if(strcmp(name, chatters[i].name) == 0) {
			return 1;
		}
	}
	return 0;
}

void add_broadcast(char *bc) {
	int len = strlen(bc);
	broadcast_msg = (char *) malloc(sizeof(char) * (len + 1));
	strncpy(broadcast_msg, bc, len);
}

void process_broadcast() {
	struct sockaddr_in addr;
	Request req;
	Response resp;
	Chatter chatter;
	int i;

	if(broadcast_msg == NULL) {
		return;
	}
	req.req = REQ_BROADCAST;
	encap_params(&req, 1, broadcast_msg);

	for(i = 0; i < nchatters; i++) {
		chatter = chatters[i];
		if(chatter.leader) { // Print message directly
			printf("%s\n", broadcast_msg);
		} else {
			addr = *make_sock_addr(chatter.host, chatter.port);
			send_request(addr, &req, &resp);
		}
	}

	broadcast_msg = NULL;
}

int encode_chatters(char **res) {
	char line[100];
	char *buf = (char *) malloc(sizeof(char) * (nchatters * 100 + 1));
	int i;
	int len = 0;
	Chatter chatter;

	if(nchatters == 0) {
		*res = "";
		return 0;
	}
	for(i = 0; i < nchatters; i++) {
		chatter = chatters[i];
		/* Format: NAME\tHOST\tPORT\t[l|c] */
		sprintf(line, "%s\t%s\t%d\t%s", chatter.name, chatter.host,
				chatter.port, chatter.leader ? "l" : "c");
		strcat(buf, line);
		len += strlen(line);
		if(i != nchatters - 1) {
			strcat(buf, LF);
			len += strlen(LF);
		}
	}
	buf[len] = '\0';
	*res = buf;
	return len;
}

void start_client(char *addrport) {
	// Get listening port
	int lis_port = get_port_number(listen_sock);
	char *ldr_addr_str = strtok(addrport, ":");
	char *ldr_port_str = strtok(NULL, ":");
	int ldr_port;
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
	// Send join request
	leader_addr = make_sock_addr(ldr_addr_str, ldr_port);
	req.req = REQ_JOIN;
	encap_params(&req, 3, username, LOOPBACK_STR, int_to_string(lis_port));

	if(send_request(*leader_addr, &req, &resp) < 0) {
		printf("Sorry, no chat is active on %s:%d, try again later.\n",
				ldr_addr_str, ldr_port);
		printf("Bye.\n");
		exit(EXIT_FAILURE);
	}
	if(resp.status < 0) {

		printf("Failed to join chat on %s:%d.", ldr_addr_str, ldr_port);
		if(resp.status == -2) {
			printf("\nName already exists. Choose another name.");
		}
		printf("\n");
		exit(EXIT_FAILURE);
	}

	decode_chatters(resp.body);
	printf("Succeeded, current users:\n");
	print_current_chatters();

	// Start listening thread
	pthread_create(&listen_thread, NULL, listening_for_broadcasts, NULL);
//	pthread_join(listen_thread, NULL);

	start_input();
}

int decode_chatters(char *msg) {
	char *buf = msg;
	char *tok;
	char *lines[CHATTER_LIMIT];
	char *line;
	int n, i;
	Chatter *chatter;

	tok = strtok(buf, LF);
	while(tok != NULL) {
		lines[n++] = tok;
		tok = strtok(NULL, LF);
	}

	nchatters = n;
	for(i = 0; i < n; i++) {
		line = lines[i];
		chatter = decode_chatter(line);
		chatters[i] = *chatter;
	}
	return 0;
}

Chatter * decode_chatter(char *line) {
	char *buf = line;
	char *tok;
	Chatter *chatter = (Chatter *) malloc(sizeof(Chatter));

	chatter->name = strtok(buf, "\t");
	chatter->host = strtok(NULL, "\t");
	string_to_int(strtok(NULL, "\t"), &chatter->port);
	tok = strtok(NULL, "\t");
	if(strcmp(tok, "l") == 0) { // Is leader
		chatter->leader = 1;
	} else {
		chatter->leader = 0;
	}

	return chatter;
}

void *(listening_for_broadcasts()) {
	char *body;
	Request req;
	Response resp;
	struct sockaddr_in r_addr;
	while(1) {
		if(recv_packet(listen_sock, &r_addr, &body) > 0) {
//			printf("Body: %s\n", body);
			if(parse_req_packet(body, &req) < 0) {
				printf("Cannot parse packet body: %s\n", body);
				continue;
			}
			handle_broadcast(req, &resp);
			resp.ack = req.seq; // Set ack to request's seq
			send_response(r_addr, &resp); // Send response
		} else {
			perror("recv_packet");
		}
	}
}

void handle_broadcast(const Request req, Response *resp) {
	char *msg;
	if(strcmp(req.req, REQ_BROADCAST) != 0) {
		perror("Not broadcast request");
		return;
	}
	msg = req.params[0];
	printf("%s\n", msg); // Print broadcast message

	resp->status = 0;
	resp->body = NULL;
}
