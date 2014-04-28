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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>


#define CHATTER_LIMIT 500
#define REQ_JOIN "join"
#define REQ_MESSAGE "message"
#define REQ_QUIT "quit"
/*********hear beat start*********/
#define REQ_BEAT "beat"
#define REQ_ELECTION "elect"
#define ANNOUNCE "newleader"
#define REQ_UPDATE "update"
/*********hear beat start*********/

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
int decode_chatters(const char *);
void decode_chatter(const char *, Chatter *);
void start_input();
void *(listening_for_broadcasts());
void handle_broadcast(const Request, Response *);
void handle_bc_join(const Request, Response *);
void handle_bc_message(const Request, Response *);
void handle_bc_quit(const Request, Response *);
/*********hear beat start*********/
void *HeartBeatProcessor();
int send_HeartBeat(const struct sockaddr_in addr, Request *req, int sock);
int handle_beat(const Request req, Response *resp);
void *(check_chatters());
/*********hear beat end***********/


Chatter chatters[CHATTER_LIMIT]; // Chatters array
int nchatters = 0;
struct sockaddr_in listen_addr; // Listening address
int listen_sock; // Listening socket
Request *broadcast_req; // Broadcast request
pthread_t listen_thread; // Listening thread
struct sockaddr_in *leader_addr; // Leader's address

char leader; //to see whether is leader
char *username;//the name of the user no matter client or leader


/*********hear beat start*********/
int sock_heartbeat;
Request heartBeat_msg;
int heartbeat_seq = 0;//define heartbeat msg sequence number
pthread_t check_chatter_thread; //for leader, start a thread checking the chatter.
pthread_t thread_HeartBeat;
pthread_t read_input;
/*********hear beat end***********/

/*********leader election*********/
int leader_down = 0;
int isnewleader = 0;
void handle_bc_election(const Request req, Response *resp);
void handle_bc_announce(const Request req, Response *resp);
void handle_bc_update(const Request req, Response *resp);
void *start_read_input();
int send_election(const int sock, const struct sockaddr_in addr, Request *req, Response *resp);

/*********leader election*********/


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

/***** Common functions *****/

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


/* Start monitoring input from console */
void *start_read_input() {
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
    if (leader) {
        pthread_cancel(check_chatter_thread);
    }
    else{
        pthread_cancel(thread_HeartBeat);
    }
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

/***** Functions for leader *****/

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
	pthread_create(&check_chatter_thread, NULL, check_chatters, NULL);
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

void *(check_chatters()) {
	int i;
	while(1){
		for(i = 0; i < nchatters; i++) {
			if (!chatters[i].leader){
				time_t now = time(0);
				time_t diff = now - chatters[i].last_hb;
				if(diff > 3){
					char *lost_chatter = chatters[i].name;
					remove_user(lost_chatter);
					add_broadcast(REQ_QUIT, 3, lost_chatter, "c", "0");
					process_broadcast();
				}
			}
		}
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
	} else if(strcmp(req.req, REQ_BEAT) == 0) { //chatter send beet signal
		handle_beat(req, resp);
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
            
			//join debug output
			//printf("%s is already in the list\n", name);
			//print_current_chatters();
		}
		return;
	}
	else
	{
		resp->status = 0;
		//printf("set status to 0\n");
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



int handle_beat(const Request req, Response *resp){
	char *name = req.param[0];
	int i;
	for(i = 0; i < nchatters; i++) {
		if(strcmp(name, chatters[i].name) == 0) {
			time_t now = time(0);
            chatters[i].last_hb = now;
			//printf("last heartbeat for %s : %ld\n", chatters[i].name, chatters[i].last_hb);
			return 1;
		}
	}
	return -1;
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

/***** Functions for clients *****/

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
	encap_param(&req, 4, username, LOOPBACK_STR, int_to_string(lis_port), "ok");
    
	if(send_request(*leader_addr, &req, &resp) < 0) {
		printf("Sorry, no chat is active on %s:%d, try again later.\n",
               ldr_addr_str, ldr_port);
		printf("Bye.\n");
		exit(EXIT_FAILURE);
	}
	//join debug output
	//printf("%d\n", resp.status);
	if(resp.status < 0) {
		printf("Failed to join chat on %s:%d.", ldr_addr_str, ldr_port);
		if(resp.status == -2) {
			printf("\nName already exists. Choose another name.");
		}
		printf("\n");
		exit(EXIT_FAILURE);
	}
	else if(resp.status == 5){
		string_to_int(resp.body, &ldr_port);
		leader_addr = make_sock_addr(ldr_addr_str, ldr_port);
		if(send_request(*leader_addr, &req, &resp) < 0) {
            printf("connect real leader %s:%d failed.\n", ldr_addr_str, ldr_port);
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
	}
    
	decode_chatters(resp.body);
	printf("Succeeded, current users:\n");
	print_current_chatters();
    
	// Start listening thread
	pthread_create(&listen_thread, NULL, listening_for_broadcasts, NULL);
    pthread_create(&thread_HeartBeat, NULL, HeartBeatProcessor, NULL);
    pthread_create(&read_input, NULL, start_read_input, NULL);
    //start_input();
    pthread_join( thread_HeartBeat, NULL);
    
    //when client become leader
	/******become new leader**********/
    
	//clear client related thread
	pthread_cancel(listen_thread);
	//pthread_cancel(thread_HeartBeat);
    
	//set the timestamp in Chatter List for the leader
	time_t present;
	int iterater;
	for (iterater = 0; iterater < nchatters; iterater++){
		present = time(0);
		chatters[iterater].last_hb = present;
	}
    
    
	//start leader related thread
	pthread_create(&listen_thread, NULL, listening_for_requests, NULL);
	pthread_create(&check_chatter_thread, NULL, check_chatters, NULL);
    
	//broadcast current chatter List to all clients
	//output new user info
    
	int newleader_port = get_port_number(listen_sock);
	printf("%s elected as new leader, listening on %s:%d\n",
		   username, LOOPBACK_STR, newleader_port);
	printf("Current users:\n");
	print_current_chatters();
    
	//reset the flag
	//isnewleader = 0;
    
	/******become new leader**********/
    
	while(1);
    
    
}

/* Decode chatters list from packet message */
int decode_chatters(const char *msg) {
	char *parse;
	char *tok;
	char *lines[CHATTER_LIMIT];
	int n, i;
	int len;
    
	len = strlen(msg);
	parse = (char *) malloc(sizeof(char) * (len + 1));
	strncpy(parse, msg, len);
	tok = strtok(parse, LF);
	n = 0;
	while(tok != NULL) {
		lines[n++] = tok;
		tok = strtok(NULL, LF);
	}
    
	nchatters = n;
	for(i = 0; i < n; i++) {
		decode_chatter(lines[i], &chatters[i]);
	}
    
	free(parse);
	parse = NULL;
	return 0;
}

/* Decode single chatter */
void decode_chatter(const char *line, Chatter *p_chatter) {
	char *parse;
	char *tok;
	int len = strlen(line);
    
	parse = (char *) malloc(sizeof(char) * (len + 1));
	strncpy(parse, line, len);
	p_chatter->name = strtok(parse, "\t");
	p_chatter->host = strtok(NULL, "\t");
	string_to_int(strtok(NULL, "\t"), &p_chatter->port);
	tok = strtok(NULL, "\t");
	if(strcmp(tok, "l") == 0) { // Is leader
		p_chatter->leader = 1;
	} else {
		p_chatter->leader = 0;
	}
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
	} else if(strcmp(req.req, REQ_ELECTION) == 0){
        handle_bc_election(req, resp);
    } else if(strcmp(req.req, ANNOUNCE) == 0){
        handle_bc_announce(req, resp);
    } else if(strcmp(req.req, REQ_UPDATE) == 0){
    	handle_bc_update(req, resp);
    }
}

/* Handle other chatter's joining event */
void handle_bc_join(const Request req, Response *resp) {
    
    
	//char *new_join_name;
	//new_join_name = req.param[0];
	if (req.paramc == 3){
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
		//printf("%d\n", req.paramc);
        
		printf("NOTICE %s joined on %s:%d\n", name, host, port);
	}
	else if (req.paramc == 4)
	{
		//printf("enter paramc ==0\n");
		int leader_port_newclient;
		char* msg_contact_leader;
		leader_port_newclient = ntohs(leader_addr->sin_port);
		msg_contact_leader = int_to_string(leader_port_newclient);
		resp->body = msg_contact_leader;
		resp->status = 5;
		//printf("contact leader\n");
        
	}
    
    
    //	Chatter *new_ctr;
    //	char *name = req.param[0];
    //	char *host = req.param[1];
    //	int port;
    //	if(string_to_int(req.param[2], &port) < 0) {
    //		perror("Invalid port number");
    //	}
    //
    //	if(leader == 0 && strcmp(name, username) != 0) {
    //		// Other new chatters
    //		new_ctr = &chatters[nchatters++];
    //		new_ctr->name = name;
    //		new_ctr->host = host;
    //		new_ctr->port = port;
    //		new_ctr->leader = 0;
    //		new_ctr->last_hb = 0;
    //	}
    //
    //	printf("NOTICE %s joined on %s:%d\n", name, host, port);
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
		//quit_chatroom();
        //		while(1){
        //
        //		}
	}
}

void handle_bc_election(const Request req, Response *resp){
    //printf("I got election message from %s\n", req.param[0]);
}

void handle_bc_announce(const Request req, Response *resp){
    //flag to close heartbeat thread and start check chatter thread
	printf("received new leader announce from: %s\n", req.param[0]);
    int newleader_port;
    //string_to_int(req.param[2], &newleader_port);
    int i_iter;
    
    for(i_iter = 0; i_iter < nchatters; i_iter++)
    	if(strcmp(chatters[i_iter].name, req.param[0]) == 0){
    		chatters[i_iter].leader = 1;
    		newleader_port = chatters[i_iter].port;
    	}
    
    leader_addr = make_sock_addr(LOOPBACK_STR, newleader_port);//set the leader_addr to newleader's address
    leader_down = 0;
    
    
}


void handle_bc_update(const Request req, Response *resp){
    
    
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

/***********heartBeat Function start************/
void *(HeartBeatProcessor())
{
    int hasBiggerRsp = 0;
    int myport = 0; // client's port number
    while(1){
        sock_heartbeat = make_req_socket();
        //define heartbeat msg to send
        heartBeat_msg.req = REQ_BEAT;
        encap_param(&heartBeat_msg, 1, username);
        int continues = 1;
        while (continues > 0) {
            continues = send_HeartBeat(*leader_addr, &heartBeat_msg, sock_heartbeat);
            if (continues > 0){
                sleep(1);
            }
        }
        if (continues == -2){
            printf("leader is down\n");
            leader_down = 1;
            int i;
            for(i = 0; i < nchatters; i++) {
                if(chatters[i].leader) {
                    //add_broadcast(REQ_QUIT, 3, chatters[i].name, "l", "0"); //for now, the message cannot be broadcast.
                    remove_user(chatters[i].name);
                    //process_broadcast();
                }
            }
        }
        
        /**************updated sorting**************/
        int sorted_portNumber[nchatters];
        int i_sort;
        
        int temp_selectionSort;
        for (i_sort = 0; i_sort < nchatters; i_sort++ ){
            sorted_portNumber[i_sort] = chatters[i_sort].port;
            if (strcmp(username, chatters[i_sort].name) == 0){
                myport = chatters[i_sort].port;
            }
        }
        
        for(i_sort = 0; i_sort < (nchatters - 1); i_sort++) {
            int maxIndx = i_sort;
            int j;
            for(j = i_sort + 1; j < nchatters; j++){
                if(sorted_portNumber[maxIndx] < sorted_portNumber[j]) {
                    maxIndx = j;
                }
            }
            temp_selectionSort = sorted_portNumber[i_sort];
            sorted_portNumber[i_sort] = sorted_portNumber[maxIndx];
            sorted_portNumber[maxIndx] = temp_selectionSort;
        }
        int c;
        for(c = 0; c < nchatters; c++){
            //printf("Port number: %d\n",sorted_portNumber[c]);
        }
        //printf("My port number: %d\n", myport);
        /**************updated sorting**************/
        
        while(leader_down){
        	if(myport < sorted_portNumber[0]){
				int num = 0;
				while(num < nchatters) {
					if(!leader_down){
						break;
					}
					if(hasBiggerRsp){ //and ALSO leader hasn't been announced.
						sleep(1);
						hasBiggerRsp = 0;
					}
					else if(myport < sorted_portNumber[num]){ //if the largest port number is not mine
						Request elect_req;
						Response elect_rsp;
						int election_sock = sock_heartbeat;
						int bigger = sorted_portNumber[num];
						struct sockaddr_in *elect_addr;
						elect_addr = make_sock_addr("localhost", bigger);
						elect_req.req = REQ_ELECTION;
						encap_param(&elect_req, 1, username);
						int result = send_election(election_sock, *elect_addr, &elect_req, &elect_rsp);//send election message here.
						if(result > 0){
							//printf("got the message response\n");
							hasBiggerRsp = 1;
						}
						else{
							//delete the unresponsed pre-leader;
							for (i_sort = 0; i_sort < nchatters; i_sort++ ){
								if(chatters[i_sort].port == bigger){
									remove_user(chatters[i_sort].name);
								}
							}
							print_current_chatters();
							//get the new sorted port number list
							for (i_sort = 0; i_sort < nchatters; i_sort++ ){
								sorted_portNumber[i_sort] = chatters[i_sort].port;
								if (strcmp(username, chatters[i_sort].name) == 0){
									myport = chatters[i_sort].port;
								}
							}
                            
							for(i_sort = 0; i_sort < (nchatters - 1); i_sort++) {
								int maxIndx = i_sort;
								int j;
								for(j = i_sort + 1; j < nchatters; j++){
									if(sorted_portNumber[maxIndx] < sorted_portNumber[j]) {
										maxIndx = j;
									}
								}
								temp_selectionSort = sorted_portNumber[i_sort];
								sorted_portNumber[i_sort] = sorted_portNumber[maxIndx];
								sorted_portNumber[maxIndx] = temp_selectionSort;
							}
							break;
						}
                        //                        num ++;
						//how to break this loop??
						//sleep(60); //for it to stay, haven't implement leader elected.
					}
					else{
						//the higher pre-leader responsed but didn't announce.
						//delete the unannounced pre-leader.
					}
				}
            }
            
            
            /*************I have the highest port number and ready to announce myself as leader******************/
            else {
                sleep(1);
                //add_broadcast(ANNOUNCE, 3, username, LOOPBACK_STR, myport);
                Request anounce_req;
				Response anounce_rsp;
				int anounce_sock = sock_heartbeat;
				int iter;
				int lower = 0;
                
				//printf("before sending announce\n");
				for (iter = 0; iter < nchatters; iter++){
					if (chatters[iter].port != myport){
						struct sockaddr_in *anounce_to_addr;
						lower = chatters[iter].port;
						anounce_to_addr = make_sock_addr("localhost", lower);
						anounce_req.req = ANNOUNCE;
						encap_param(&anounce_req, 1, username);
						//printf("just before send_election\n");
						int result = send_election(anounce_sock, *anounce_to_addr, &anounce_req, &anounce_rsp);//send election message here.
						//printf("after send_election\n");
						if(result > 0){
							//printf("got the anounce message response\n");
							hasBiggerRsp = 1;
						}
						else
							printf("send announce error\n");
					}
					else {
						chatters[iter].leader = 1;
						chatters[iter].last_hb = 0;
					}
				}
                
                
                isnewleader = 1;//set the isnewleader flag = 1;begin close and open threads in start_input()
                leader = 1;//set the leader flag
                //sleep(10);
                int x_return = 1;
                pthread_exit(&x_return);
                /*************I have the highest port number and ready to announce myself as leader******************/
            }
        }//end of while (leader down)
    }
}

//void announce_leader(){ //undeclared
//
//}

int send_election(const int sock, const struct sockaddr_in addr, Request *req, Response *resp){
    //struct sockaddr_in r_addr;
	char *msg;
	int len;
	int nbytes;
	int seq = 0;
    int resend = 0;
    int temp_recv;
    
    char* resp_body = NULL;
    
    set_recv_timeout(sock, RECV_TIMEOUT);
    
	//seq ++;
	req->seq = seq;
	len = compose_req_msg(*req, &msg);
    
    
	//printf("Request:\n----\n%s\n----\n", msg);
	if(len < 0) {
		perror("compose_req_msg");
		return -1;
	}
    
    
    // Send message
    while (1) {
        nbytes = sendto(sock, msg, len, 0, (struct sockaddr *) &addr,
                        (socklen_t) sizeof(struct sockaddr_in));
        if(nbytes < 0) {
            return -1;
        }
        // Wait for response or ack
        temp_recv = recv_packet(sock, &addr, &resp_body);
        if( temp_recv < 0 && resend < 2) {
            //printf("Timout reached. Send election\n");
        }
        else if (temp_recv < 0 && resend >= 2){
            //printf("return -2\n");
            return -2;
        }
        else
        {
            //printf("break\n");
            break;
        }
        //printf("resend: %d\n", resend);
        resend++;
    }
    //printf("return 1");
	return 1;
}

int send_HeartBeat(const struct sockaddr_in addr, Request *req, int sock) {
	//printf("send heart beat\n");
    
	//struct sockaddr_in r_addr;
	char *msg;
	int len;
	int nbytes;
	int seq;
    int resend = 0;
    int temp_recv;
    
    char* resp_body = NULL;
    
    set_recv_timeout(sock, RECV_TIMEOUT);
    
	seq = heartbeat_seq++;
	req->seq = seq;
	len = compose_req_msg(*req, &msg);
    
    
	//printf("Request:\n----\n%s\n----\n", msg);
	if(len < 0) {
		perror("compose_req_msg");
		return -1;
	}
    
    
    // Send message
    while (1) {
        nbytes = sendto(sock, msg, len, 0, (struct sockaddr *) &addr,
                        (socklen_t) sizeof(struct sockaddr_in));
        if(nbytes < 0) {
            return -1;
        }
        // Wait for response or ack
        temp_recv = recv_packet(sock, &addr, &resp_body);
        if( temp_recv < 0 && resend < 2) {
            //printf("Timout reached. Resending beat\n");
        }
        else if (temp_recv < 0 && resend >= 2){
            //printf("return -2\n");
            return -2;
        }
        else
        {
            //printf("break\n");
            break;
        }
        //printf("resend: %d\n", resend);
        resend++;
    }
    //printf("return 1");
	return 1;
}
/***********heartBeat Function end************/
