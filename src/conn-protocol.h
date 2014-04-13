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
	char *host;
	int port;
	char *req;
	char **params;
	int paramsn;
} Request;

typedef struct cli_response {
	int seq;
	int ack;
	char *body;
} Response;

char * int_to_string(int i) {
	char *buf = (char *) malloc(sizeof(char) * 11);
	sprintf(buf, "%d", i);
	return buf;
}

int string_to_int(char *str, int *val) {
	char *end_ptr;
	char err = 0;
	*val = strtol(str, &end_ptr, 10);
	if(end_ptr == str || *end_ptr != '\0') {
		err = 1;
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

struct sockaddr_in * make_sock_addr(const char *addr_name,
		const uint16_t port) {
	struct sockaddr_in *addr;
	addr = (struct sockaddr_in *) malloc(sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	if(addr_name == NULL) {
		addr->sin_addr.s_addr = htonl(INADDR_ANY);
	} else {
		if(inet_aton(addr_name, &addr->sin_addr) < 0) {
			perror("Invalid address name");
			exit(EXIT_FAILURE);
		}
	}
	return addr;
}

int get_port_number(const int sock) {
	struct sockaddr_in l_addr;
	socklen_t size = (socklen_t) sizeof(l_addr);
	if(getsockname(sock, (struct sockaddr *) &l_addr, &size)) { // Get port
		perror("getsockname");
		exit(EXIT_FAILURE);
	}
	return ntohs(l_addr.sin_port);
}

char * get_host_addr(struct sockaddr_in addr) {
}

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

int make_req_socket() {
	struct sockaddr_in addr = *make_sock_addr(NULL, 0);
	return create_socket(addr);
}

char * compose_req_msg(Request *request, int *msg_len) {
	int len = 0;
	int i;
	char *seq_str;
	char *msg;

	// Calculate msg length
	len += strlen(PROTOCOL_NAME) + strlen(LF);
	seq_str = int_to_string(request->seq);
	len += strlen(seq_str) + strlen(LF);
	len += strlen(LF);
	len += strlen(request->req);
	for(i = 0; i < request->paramsn; i++) {
		len += strlen(LF) + strlen(request->params[i]);
	}
	*msg_len = len;

	msg = (char *) malloc(sizeof(char) * (len + 1));
	strcat(msg, PROTOCOL_NAME);
	strcat(msg, LF);
	strcat(msg, seq_str);
	strcat(msg, LF);
	strcat(msg, LF);
	strcat(msg, request->req);
	for(i = 0; i < request->paramsn; i++) {
		strcat(msg, LF);
		strcat(msg, request->params[i]);
	}
	msg[len] = '\0';
	return msg;
}

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
//	len = strlen(body) - start;
//	if(len > 0) {
//		msg = (char *) malloc(sizeof(char) * (len + 1));
//		strncpy(msg, body + start, len);
//		msg[len] = '\0';
//	}
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
	start++;

	len = strlen(body) - start;
	if(len > 0) {
		msg = (char *) malloc(sizeof(char) * (len + 1));
		strncpy(msg, body + start, len);
		resp->body = msg;
	} else {
		resp->body = NULL;
	}
	return 0;
}

int send_request(Request *req, Response *resp) {
	int sock;
	struct sockaddr_in *addr;
	struct sockaddr_in r_addr;
	char *msg;
	char *resp_body = NULL;
	int len;
	int nbytes;

	if(req == NULL) {
		return -1;
	}
	addr = make_sock_addr(req->host, req->port);
	sock = make_req_socket();
//	set_recv_timeout(sock, RECV_TIMEOUT);
	req->seq = local_seq;
	msg = compose_req_msg(req, &len);
	if(len < 0) {
		perror("compose_req_msg");
		exit(EXIT_FAILURE);
	}
	// Send message
	nbytes = sendto(sock, msg, len, 0, (struct sockaddr *) addr,
			(socklen_t) sizeof(struct sockaddr_in));
	if(nbytes < 0) {
		return -1;
	}
	// Wait for response or ack
	if(recv_packet(sock, &r_addr, resp_body) < 0) {
		return -2;
	}
	if(parse_resp_packet(resp_body, resp) < 0) {
		return -3;
	}
	if(local_seq != resp->ack) {
		perror("Ack number does not match");
		return -4;
	}
	local_seq++;
	return 0;
}

