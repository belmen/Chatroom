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
void send_quit();
void quit_chatroom();
void remove_user(const char *);
void start_leader();
void start_client(char *);
void *(listening_for_requests());
void print_current_chatters();
void process_broadcast();
void add_broadcast(const char *req, const int paramc, ...);
void handle_request(const Request, Response *);
void handle_join(const Request, Response *);
void handle_message(const Request, Response *);
void handle_quit(const Request, Response *);
int encode_chatters(char **);
int decode_chatters(char *);
Chatter * decode_chatter(char *);
void start_input();
void *(listening_for_broadcasts());
void handle_broadcast(const Request, Response *);
void handle_bc_join(const Request, Response *);
void handle_bc_message(const Request, Response *);
void handle_bc_quit(const Request, Response *);

Chatter chatters[CHATTER_LIMIT]; // Chatters array
int nchatters = 0;
struct sockaddr_in listen_addr; // Listening address
int listen_sock; // Listening socket
Request *broadcast_req; // Broadcast request
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

	printf("--- Welcome to Distributed Chatroom! ---\n");
	printf("Help: Type \":users\" to list current chatters,");
	printf(" press Ctrl+D to quit.\n\n");

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

/* Encapsule parameters into request */
void encap_param(Request *req, int argn, ...) {
	va_list ap;
	int i;

	if(req == NULL) {
		return;
	}
	va_start(ap, argn);
	req->paramc = argn;
	req->param = (char **) malloc(sizeof(char *) * argn);
	for(i = 0; i < argn; i++) {
		req->param[i] = (char *) va_arg(ap, char *);
	}
}

/* Start monitoring input from console */
void start_input() {
	char *input = NULL;
	size_t size;
	Request req;
	Response resp;
	int len;

	while(1) {
		if(getline(&input, &size, stdin) < 0) { // EOF
			break;
		}
		len = strlen(input);
		if(len > 0) {
			input[len - 1] = '\0'; // Remove new line
		}
		if(strlen(input) == 0) {
			continue;
		}
		if(strcmp(input, ":users") == 0) { // List users
			printf("Current users:\n");
			print_current_chatters();
		} else if(strcmp(input, ":quit") == 0) {
			break;
		} else if(leader) {
			add_broadcast(REQ_MESSAGE, 2, username, input);
			process_broadcast();
		} else {
			req.req = REQ_MESSAGE;
			encap_param(&req, 2, username, input);
			send_request(*leader_addr, &req, &resp);
		}
	}

	// Send exit request
	send_quit();

	quit_chatroom();
}

/* Quit chatroom */
void quit_chatroom() {
	// Cancel running threads
	pthread_cancel(listen_thread);
	exit(EXIT_SUCCESS);
}

/* Send quit message to others */
void send_quit() {
	Request req;
	Response resp;

	/*
	 * Param1: name
	 * Param2: leader : "l", client: "c"
	 * Param3: Normal exit: "1", crashed: "0"
	 */
	if(leader) {
		add_broadcast(REQ_QUIT, 3, username, "l", "1");
		process_broadcast();
	} else {
		// No param 3
		req.req = REQ_QUIT;
		encap_param(&req, 1, username);
		send_request(*leader_addr, &req, &resp);
	}
}

/* Start the leader chatter */
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

	start_input();
}

/* Print current chatters to the console */
void print_current_chatters() {
	int i;
	Chatter chatter;

	for(i = 0; i < nchatters; i++) {
		chatter = chatters[i];
		printf("%s %s:%d%s\n", chatter.name, chatter.host,
				chatter.port, chatter.leader ? " (Leader)" : "");
	}
}

/* Thread handler for leader to listen for incoming requests */
void *(listening_for_requests()) {
	char *body = NULL;
	Request req;
	Response resp;
	struct sockaddr_in r_addr;

	resp.status = 0;
	resp.body = NULL;
	while(1) {
		if(recv_packet(listen_sock, &r_addr, &body) > 0) {
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

/* Handle incoming requests */
void handle_request(const Request req, Response *resp) {
	if(strcmp(req.req, REQ_JOIN) == 0) { // New chatter joining
		handle_join(req, resp);
	} else if(strcmp(req.req, REQ_MESSAGE) == 0) { // Sending message
		handle_message(req, resp);
	} else if(strcmp(req.req, REQ_QUIT) == 0) { // Quit
		handle_quit(req, resp);
	}
}

/* Handle join request */
void handle_join(const Request req, Response *resp) {
	Chatter *new_ctr = &chatters[nchatters];
	time_t t;
	char *msg;
	char *name;

	name = req.param[0];
	if(check_duplicate_name(name)) {
		if(resp != NULL) {
			resp->status = -2;
			resp->body = NULL;
		}
		return;
	}

	new_ctr->name = name;
	new_ctr->host = req.param[1];
	if(string_to_int(req.param[2], &new_ctr->port) < 0) {
		perror("Invalid port");
	}
	new_ctr->leader = 0;
	time(&t);
	new_ctr->last_hb = t;
	nchatters++;
	encode_chatters(&msg);
	if(resp != NULL) {
		resp->body = msg;
	}

	add_broadcast(REQ_JOIN, 3, new_ctr->name,
			new_ctr->host, int_to_string(new_ctr->port));
}

/* Handle message request */
void handle_message(const Request req, Response *resp) {
	add_broadcast(REQ_MESSAGE, 2, req.param[0], req.param[1]);
}

/* Handle quit request */
void handle_quit(const Request req, Response *resp) {
	char *name = req.param[0];

	remove_user(name);
	add_broadcast(REQ_QUIT, 3, name, "c", "1");
}

/* Check if a name already exists in chatroom */
char check_duplicate_name(const char *name) {
	int i;
	for(i = 0; i < nchatters; i++) {
		if(strcmp(name, chatters[i].name) == 0) {
			return 1;
		}
	}
	return 0;
}

/* Add broadcast message to process */
void add_broadcast(const char *req, const int paramc, ...) {
	va_list ap;
	int i;

	va_start(ap, paramc);
	broadcast_req = (Request *) malloc(sizeof(Request));
	broadcast_req->req = (char *) malloc(sizeof(char) * (strlen(req) + 1));
	strncpy(broadcast_req->req, req, strlen(req));
	broadcast_req->paramc = paramc;
	broadcast_req->param = (char **) malloc(sizeof(char *) * paramc);
	for(i = 0; i < paramc; i++) {
		broadcast_req->param[i] = (char *) va_arg(ap, char *);
	}
}

/* Send broadcast message to all clients */
void process_broadcast() {
	struct sockaddr_in addr;
	Response resp;
	Chatter chatter;
	int i;

	if(broadcast_req == NULL) {
		return;
	}

	for(i = 0; i < nchatters; i++) {
		chatter = chatters[i];
		if(chatter.leader) { // Print message directly
			handle_broadcast(*broadcast_req, NULL);
		} else {
			addr = *make_sock_addr(chatter.host, chatter.port);
			send_request(addr, broadcast_req, &resp);
		}
	}

	broadcast_req = NULL;
}

/* Encode chatters list into packet format */
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

/* Start client chatter */
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
	encap_param(&req, 3, username, LOOPBACK_STR, int_to_string(lis_port));

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

	start_input();
}

/* Decode chatters list from packet message */
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

/* Decode single chatter */
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

/* Thread handler for clients to listen for broadcast message */
void *(listening_for_broadcasts()) {
	char *body;
	Request req;
	Response resp;
	struct sockaddr_in r_addr;

	resp.status = 0;
	resp.body = NULL;
	while(1) {
		if(recv_packet(listen_sock, &r_addr, &body) > 0) {
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

/* Handle received broadcast message */
void handle_broadcast(const Request req, Response *resp) {
	if(strcmp(req.req, REQ_JOIN) == 0) { // Someone joined
		handle_bc_join(req, resp);
	} else if(strcmp(req.req, REQ_MESSAGE) == 0) { // Someone send msg
		handle_bc_message(req, resp);
	} else if(strcmp(req.req, REQ_QUIT) == 0) { // Someone quit
		handle_bc_quit(req, resp);
	}
}

/* Handle other chatter's joining event */
void handle_bc_join(const Request req, Response *resp) {
	Chatter *new_ctr;
	char *name = req.param[0];
	char *host = req.param[1];
	int port;
	if(string_to_int(req.param[2], &port) < 0) {
		perror("Invalid port number");
	}

	if(leader == 0 && strcmp(name, username) != 0) {
		// Other new chatters
		new_ctr = &chatters[nchatters++];
		new_ctr->name = name;
		new_ctr->host = host;
		new_ctr->port = port;
		new_ctr->leader = 0;
		new_ctr->last_hb = 0;
	}

	printf("NOTICE %s joined on %s:%d\n", name, host, port);
}

/* Handle message broadcast */
void handle_bc_message(const Request req, Response *resp) {
	printf("%s: %s\n", req.param[0], req.param[1]);
}

/* Handle other chatter's quit broadcast */
void handle_bc_quit(const Request req, Response *resp) {
	char *name = req.param[0]; // username
	int is_leader = strcmp("l", req.param[1]) == 0 ? 1 : 0; // Leader: "l", others: "c"
	int normal = strcmp("1", req.param[2]) == 0 ? 1 : 0;

	if(leader == 0) {
		remove_user(name);
	}
	printf("NOTICE %s%s left the chat%s\n", name, is_leader ? " (Leader)" : "",
			normal ? "" : " or crashed");

	if(is_leader && leader == 0) {
		// TODO: Leader election
		// Now we just quit
		quit_chatroom();
	}
}

/* Remove a chatter from chatter list */
void remove_user(const char *name) {
	int i;
	int index = -1;
	for(i = 0; i < nchatters; i++) {
		if(strcmp(name, chatters[i].name) == 0) {
			index = i;
			break;
		}
	}
	if(index == -1) {
		return;
	}
	for(i = index; i + 1 < nchatters; i++) {
		chatters[i] = chatters[i + 1];
	}
	nchatters--;
}
