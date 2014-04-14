/*
 * Chatroom connection protocol
 */
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define LOOPBACK_STR "127.0.0.1"
#define RECV_TIMEOUT 3
#define BUFFER_SIZE 8192
#define PROTOCOL_NAME "chatroom-0.1"
#define LF "\n"

char buffer[BUFFER_SIZE];
int local_seq = 0;
int remote_seq;

typedef struct cli_request {
	int seq;
	char *req;
	char **params;
	int paramsn;
} Request;

typedef struct cli_response {
	int seq;
	int ack;
	int status;
	char *body;
} Response;

/* Convert an integer to string using sprintf */
char * int_to_string(const int i) {
	char *buf = (char *) malloc(sizeof(char) * 11);
	sprintf(buf, "%d", i);
	return buf;
}

/* Convert a string to an integer using strtol.
 * Returns -1 if fails */
int string_to_int(const char *str, int *val) {
	char *end_ptr;
	char err = 0;
	*val = strtol(str, &end_ptr, 10);
	if(end_ptr == str || *end_ptr != '\0') {
		err = -1;
	}
	return err;
}

/* Set socket receiving timeout in seconds */
void set_recv_timeout(int sock, int to_sec) {
	struct timeval timeout;
	timeout.tv_sec = to_sec;
	timeout.tv_usec = 0;

	if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
		(char *) &timeout, sizeof(timeout)) < 0) {
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}
}

/* Create a socket address with givin host and port */
struct sockaddr_in * make_sock_addr(const char *addr_name,
		const uint16_t port) {
	struct sockaddr_in *addr;
	addr = (struct sockaddr_in *) malloc(sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	if(addr_name == NULL) {
		addr->sin_addr.s_addr = htonl(INADDR_ANY);
	} else {
		if(inet_pton(AF_INET, addr_name, &addr->sin_addr) < 0) {
			perror("Invalid address name");
			return NULL;
		}
	}
	return addr;
}

/* Get bound port number of a socket */
int get_port_number(const int sock) {
	struct sockaddr_in l_addr;
	socklen_t size = (socklen_t) sizeof(l_addr);
	if(getsockname(sock, (struct sockaddr *) &l_addr, &size)) { // Get port
		perror("getsockname");
		exit(EXIT_FAILURE);
	}
	return ntohs(l_addr.sin_port);
}

/* Get host address from a socket address */
char * get_host_addr(struct sockaddr_in addr) {
	char *buf = (char *) malloc(sizeof(char) * 20);
	inet_ntop(AF_INET, &addr.sin_addr, buf,
		(socklen_t) sizeof(struct sockaddr_in));
	return buf;
}

/* Create a socket and bind it with given address */
int create_socket(const struct sockaddr_in addr) {
	int sock = socket(PF_INET, SOCK_DGRAM, 0);
	if(sock < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	if(bind(sock, (struct sockaddr *) &addr,
			(socklen_t) sizeof(addr)) < 0) {
		perror("bind");
		exit(EXIT_FAILURE);
	}
	return sock;
}

/* Create a socket listening on an available port */
int make_req_socket() {
	struct sockaddr_in addr = *make_sock_addr(NULL, 0);
	return create_socket(addr);
}

/* Compose given request into package message.
 * msg_o outputs the composed message. Returns length
 * of the message */
int compose_req_msg(const Request req, char **msg_o) {
	int len = 0;
	int i;
	char *seq_str;
	char *msg;

	// Calculate msg length
	len += strlen(PROTOCOL_NAME) + strlen(LF);
	seq_str = int_to_string(req.seq);
	len += strlen(seq_str) + strlen(LF);
	len += strlen(LF);
	len += strlen(req.req);
	for(i = 0; i < req.paramsn; i++) {
		len += strlen(LF) + strlen(req.params[i]);
	}

	msg = (char *) malloc(sizeof(char) * (len + 1));
	strcat(msg, PROTOCOL_NAME);
	strcat(msg, LF);
	strcat(msg, seq_str);
	strcat(msg, LF);
	strcat(msg, LF);
	strcat(msg, req.req);
	for(i = 0; i < req.paramsn; i++) {
		strcat(msg, LF);
		strcat(msg, req.params[i]);
	}
	msg[len] = '\0';
	*msg_o = msg;
	return len;
}

int compose_resp_msg(const Response resp, char **msg_o) {
	int len = 0;
	char *seq_str;
	char *ack_str;
	char *status_str;
	char *msg;

	// Calculate msg length
	len += strlen(PROTOCOL_NAME) + strlen(LF);
	seq_str = int_to_string(resp.seq);
	len += strlen(seq_str) + strlen(LF);
	ack_str = int_to_string(resp.ack);
	len += strlen(ack_str) + strlen(LF);
	len += strlen(LF);
	status_str = int_to_string(resp.status);
	len += strlen(status_str);
	if(resp.body != NULL) {
		len += strlen(LF);
		len += strlen(resp.body);
	}

	msg = (char *) malloc(sizeof(char) * (len + 1));
	strcat(msg, PROTOCOL_NAME);
	strcat(msg, LF);
	strcat(msg, seq_str);
	strcat(msg, LF);
	strcat(msg, ack_str);
	strcat(msg, LF);
	strcat(msg, LF);
	strcat(msg, status_str);
	if(resp.body != NULL) {
		strcat(msg, LF);
		strcat(msg, resp.body);
	}
	msg[len] = '\0';
	*msg_o = msg;
	return len;
}

/* Receive message from given socket. addr outputs the remote address,
 * body_p outputs the packet body. Returns the number of bytes read */
int recv_packet(int sock, struct sockaddr_in *addr, char **body_p) {
	int nbytes;
	char *body;
	socklen_t size = (socklen_t) sizeof(struct sockaddr_in);
	nbytes = recvfrom(sock, buffer, BUFFER_SIZE, 0,
			(struct sockaddr *) addr, &size);
	if(nbytes > 0) {
		body = (char *) malloc(sizeof(char) * (nbytes + 1));
		strncpy(body, buffer, nbytes);
		body[nbytes] = '\0';
		*body_p = body;
	}
	return nbytes;
}

/* Parse the packet body into a request structure.
 * Returns -1 if fails */
int parse_req_packet(char *body, Request *req) {
	int len = strlen(body);
	char *tok;
	char *msg;
	char *parse;
	int seq;
	int start = 0;
	int i = 0;
	char c;
	int lines = 0;

	if(req == NULL) {
		return -1;
	}
	parse = (char *) malloc(sizeof(char) * (len + 1));
	strncpy(parse, body, strlen(body));
	parse[len] = '\0';
	tok = strtok(parse, LF);
	start += strlen(tok) + strlen(LF);
	if(strcmp(tok, PROTOCOL_NAME) != 0) {
		perror("Wrong protocol");
		return -1;
	}
	tok = strtok(NULL, LF);
	start += strlen(tok) + strlen(LF);
	if(string_to_int(tok, &seq) < 0) {
		perror("Wrong sequence number");
		return -1;
	}
	req->seq = seq;
	start += strlen(LF);

	msg = body + start;
	if(*msg) {
		req->req = strtok(NULL, LF);
		// Count number of lines
		while((c = msg[i++])) {
			if(c == '\n') {
				lines++;
			}
		}
		req->paramsn = lines;
		req->params = (char **) malloc(sizeof(char *) * lines);
		for(i = 0; i < lines; i++) {
			req->params[i] = strtok(NULL, LF);
		}
	}
	return 0;
}

/* Parse the packet body into response structure.
 * Returns -1 if fails */
int parse_resp_packet(char *body, Response *resp) {
	char *tok;
	char *msg;
	int seq, ack;
	int start = 0;
	int len;

	if(resp == NULL) {
		return -1;
	}
	tok = strtok(body, LF);
	start += strlen(tok) + strlen(LF);
	if(strcmp(tok, PROTOCOL_NAME) != 0) {
		perror("Wrong protocol");
		return -1;
	}
	tok = strtok(NULL, LF);
	start += strlen(tok) + strlen(LF);
	if(string_to_int(tok, &seq) < 0) {
		perror("Wrong sequence number");
		return -1;
	}
	resp->seq = seq;

	tok = strtok(NULL, LF);
	start += strlen(tok) + strlen(LF);
	if(string_to_int(tok, &ack) < 0) {
		perror("Wrong ACK number");
		return -1;
	}
	resp->ack = ack;
	start += strlen(LF);

	msg = body + start;
	if(*msg) {
		tok = strtok(NULL, LF);
		start += strlen(tok) + strlen(LF);
		if(string_to_int(tok, &resp->status) < 0) {
			perror("Invalid status code");
			return -1;
		}

		len = strlen(body) - start;
		if(len > 0) {
			msg = (char *) malloc(sizeof(char) * (len + 1));
			strncpy(msg, body + start, len);
			resp->body = msg;
		} else {
			resp->body = NULL;
		}
	}
	return 0;
}

/* Send a request with available socket.
 *  */
int send_request(const struct sockaddr_in addr, Request *req, Response *resp) {
	int sock;
	struct sockaddr_in r_addr;
	char *msg;
	char *resp_body = NULL;
	int len;
	int nbytes;

	sock = make_req_socket();
//	set_recv_timeout(sock, RECV_TIMEOUT);
	req->seq = local_seq;
	len = compose_req_msg(*req, &msg);
	if(len < 0) {
		perror("compose_req_msg");
		return -1;
	}
	// Send message
	nbytes = sendto(sock, msg, len, 0, (struct sockaddr *) &addr,
			(socklen_t) sizeof(struct sockaddr_in));
	if(nbytes < 0) {
		return -1;
	}
	// Wait for response or ack
	if(recv_packet(sock, &r_addr, &resp_body) < 0) {
		return -2;
	}
	if(parse_resp_packet(resp_body, resp) < 0) {
		return -3;
	}
	if(local_seq != resp->ack) {
		perror("Ack number does not match");
		return -4;
	}
	shutdown(sock, 0); // Close socket
	local_seq++;
	return 0;
}

int send_response(const struct sockaddr_in addr, Response *resp) {
	int sock;
	int len;
	int nbytes;
	char *msg;

	sock = make_req_socket();
	//	set_recv_timeout(sock, RECV_TIMEOUT);
	resp->seq = local_seq;
	len = compose_resp_msg(*resp, &msg);
	if(len < 0) {
		perror("Cannot compose response");
		return -1;
	}
	// Send message
	nbytes = sendto(sock, msg, len, 0, (struct sockaddr *) &addr,
				(socklen_t) sizeof(struct sockaddr_in));
	if(nbytes < 0) {
		return -1;
	}
	shutdown(sock, 0); // Close socket
	local_seq++;
	return 0;
}
