#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include "dir.h"
#include "usage.h"
#include "Thread.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>

// Here is an example of how to use the above function. It also shows
// one how to get the arguments passed on the command line.

#define BACKLOG 4		 // pending connections queue hold 4 connections
#define MAXDATASIZE 1024 // max number of bytes we can get at once
int cached_ports[10] = {0,0,0,0,0}; // caches ports being used


void sigchld_handler(int s)
{
	// waitpid() might overwrite errno, so we save and restore it:
	int saved_errno = errno;
	while(waitpid(-1, NULL, WNOHANG) > 0);
	errno = saved_errno;
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

// split a buf useing " " as the delimiter and store into args
// only the first two words in buf are parsed out
void split_buf(char buf[], int numbytes, char* args[]) {
	char * pch;
	int j;
	if (numbytes>1){
		buf[numbytes-2] = '\0';
		pch = strtok(buf, " ");
		args[0] = pch;
		for (j = 1; pch != NULL && j <= 2; j++){
			pch = strtok(0, " ");
			args[j] = pch;
		}
	}
	
}

struct listener_and_port{
	int listener;
	int port_number;
	int cached_ports_index;
};

struct listener_and_port bind_passive_socket(){
	struct listener_and_port passive_struct;
	int listener_sock, new_fd;	// listen on sock_fd, new connection on new_fd
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr;
	socklen_t sin_size;
	struct sigaction sa;
	int yes=1;
	char s[INET6_ADDRSTRLEN];
	int rv;
	int passive_port = 0;
	char passive_port_char[10];
	int i;
	int cached_ports_index = -1;
	int passive_port_usable = 0;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	// generate random number for a Passive port
	srand(time(NULL));
	do {
		memset(&passive_port_char[0], 0, sizeof(passive_port_char));
		passive_port = rand();
		sprintf(passive_port_char, "%d", passive_port);

		for (i=0; i<5; i++){
			if (cached_ports[i] == passive_port){
				passive_port_usable = 0;
			}
			if (cached_ports[i] == 0){
				cached_ports_index = i;
				passive_port_usable = 1;
			}
		}
	} while (!((1024 <= passive_port) && (passive_port <= 65535) && (cached_ports_index != -1) && (passive_port_usable)));

	// cache the Port number being used
	cached_ports[cached_ports_index] = passive_port;

	// argv[1] is the port number
	if ((rv = getaddrinfo(NULL, passive_port_char, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
	}

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		// create socket
		if ((listener_sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
			perror("server: socket");
			continue;
		}
		// set options
		if (setsockopt(listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
			perror("setsockopt");
			break;
		}
		// bind socket
		if (bind(listener_sock, p->ai_addr, p->ai_addrlen) == -1) {
			close(listener_sock);
			perror("server: bind");
			continue;
		}
		break;
	}
	freeaddrinfo(servinfo); 

	if (p == NULL)	{
		fprintf(stderr, "server: failed to bind\n");
		passive_struct.listener = -1;
		return passive_struct;
	}
	// listen to socket
	if (listen(listener_sock, BACKLOG) == -1) {
		perror("listen");
		passive_struct.listener = -1;
		return passive_struct;
	}
	// setup termination handler
	sa.sa_handler = sigchld_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		perror("sigaction");
		passive_struct.listener = -1;
		return passive_struct;
	}

	passive_struct.listener = listener_sock;
	passive_struct.port_number = passive_port;
	passive_struct.cached_ports_index = cached_ports_index;

	return passive_struct;
}

// verify if user is logged in, if not, send client a not logged in message
int verify_conn(int ctrl_sock, int conn_flag) {
	if (conn_flag == 1) {
		return 1;
	}
	send(ctrl_sock, "530 Not logged in\n", 18, 0);
	return 0;
}

// a client gets to interact with the FTP server
void service_client(int ctrl_sock) {
	char buf[MAXDATASIZE];
	int numbytes, i;
	int ind = -1;
	int conn_flag = 0;
	struct sockaddr_in server_addr;
	int server_addr_len = sizeof(server_addr);
	getsockname(ctrl_sock, (struct sockaddr*)&server_addr, &server_addr_len);
	struct listener_and_port passive_listener_sock;
	struct sockaddr_storage their_addr; 
	socklen_t sin_size;
	int data_fd;
	int passive_available = 0;
	char* cwd;
	char buff[PATH_MAX + 1];
	int filefd;
	char buffer[BUFSIZ];
	ssize_t read_return;

	send(ctrl_sock, "220 FTP server ready\n", 21, 0);
	while (1) {
		// MSG_PEEK peeks into the queue without removing that data from the queue
		if ((numbytes = recv(ctrl_sock, buf, MAXDATASIZE, 0)) == -1) {
			perror("recv");
			break;
		}
		else if (numbytes == 0) {
			fprintf(stderr, "server: failed to receive from client input\n");
			break;
		}
		// split client input into 1 or 2 arguments
		char* args[2];
		split_buf(buf, numbytes, args);

		if (args[0] == NULL){
			continue;
		}
		else if (strcmp(args[0], "USER") == 0) {
			// only accept "cs317"
			if (args[1] == NULL){
				send(ctrl_sock, "501 Syntax error in parameters or arguments\n", 44, 0);
			}
			else if (strcmp(args[1], "cs317") == 0) {
				send(ctrl_sock, "230 User cs317 logged in\n", 25, 0);
				conn_flag = 1;
			}
			else {
				send(ctrl_sock, "530 login incorrect\n", 20, 0);
			}
		}
		else if (strcmp(args[0], "QUIT") == 0) {
			if (args[1] != NULL){
				send(ctrl_sock, "501 Syntax error in parameters or arguments\n", 44, 0);
				continue;
			}
			send(ctrl_sock, "221 service closing control connection\n", 39, 0);
			conn_flag = 0;
			break;
		}
		else if (strcmp(args[0], "TYPE") == 0) {
			if (!verify_conn(ctrl_sock, conn_flag)) continue;
			if (args[1] == NULL){
				send(ctrl_sock, "501 Syntax error in parameters or arguments\n", 44, 0);
			}
			else if (strcmp(args[1], "I") == 0) {
				send(ctrl_sock, "200 Type set to Image\n", 22, 0);
			}
			else if (strcmp(args[1], "A") == 0) {
				send(ctrl_sock, "200 Type set to ASCII\n", 22, 0);
			}
			else {
				send(ctrl_sock, "504 Only Image and ASCII types are supported\n", 45, 0);
			}
		}
		else if (strcmp(args[0], "MODE") == 0) {
			if (!verify_conn(ctrl_sock, conn_flag)) continue;
			if (args[1] == NULL){
				send(ctrl_sock, "501 Syntax error in parameters or arguments\n", 44, 0);
			}
			else if (strcmp(args[1], "S") == 0) {
				send(ctrl_sock, "200 Mode set to Stream\n", 24, 0);
			}
			else {
				send(ctrl_sock, "504 Only Stream mode is supported\n", 34, 0);
			}
		}
		else if (strcmp(args[0], "STRU") == 0) {
			if (!verify_conn(ctrl_sock, conn_flag)) continue;
			if (args[1] == NULL){
				send(ctrl_sock, "501 Syntax error in parameters or arguments\n", 44, 0);
			}
			else if (strcmp(args[1], "F") == 0) {
				send(ctrl_sock, "200 Structure set to File\n", 26, 0);
			}
			else {
				send(ctrl_sock, "504 Only File structure is supported\n", 37, 0);
			}
		}
		else if (strcmp(args[0], "RETR") == 0) {
			if (!verify_conn(ctrl_sock, conn_flag)) continue;

			if (args[1] == NULL){
				send(ctrl_sock, "501 Syntax error in parameters or arguments\n", 44, 0);
				continue;
			}
			if (passive_available == 0){
				send(ctrl_sock, "425 Can't open data connection\n", 31, 0);
				continue;
			}

			// accept a client
			sin_size = sizeof their_addr;
			data_fd = accept(passive_listener_sock.listener, (struct sockaddr *)&their_addr, &sin_size);
			if (data_fd == -1) {
				send(ctrl_sock, "425 Can't open data connection\n", 31, 0);
				continue;
			}

			// open file
			filefd = open(args[1], O_RDONLY);
			if (filefd == -1) {
				send(ctrl_sock, "451 Requested action aborted: local error in processing\n", 56, 0);
				close(data_fd);
				close(passive_listener_sock.listener);
				cached_ports[passive_listener_sock.cached_ports_index] = 0;
				passive_available = 0;
				continue;
			}

			// read and send
			while(1){
				read_return = read(filefd, buffer, BUFSIZ);
				if (read_return == 0){
					send(ctrl_sock, "226 Closing data connection. Requested file action successful\n", 62, 0);
					break;
				}
				if (read_return == -1) {
					send(ctrl_sock, "451 Requested action aborted: local error in processing\n", 56, 0); 
					break;
				}
				if (send(data_fd, buffer, read_return, 0) == -1) {
					send(ctrl_sock, "426 Connection closed; transfer aborted\n", 40, 0);
					break;
				}
			}
			close(data_fd);
			close(passive_listener_sock.listener);
			cached_ports[passive_listener_sock.cached_ports_index] = 0;
			passive_available = 0;
		}
		else if (strcmp(args[0], "PASV") == 0) {
			if (!verify_conn(ctrl_sock, conn_flag)) continue;
			if (args[1] != NULL){
				send(ctrl_sock, "501 Syntax error in parameters or arguments\n", 44, 0);
				continue;
			}

			// close previous passive socket if duplicate call comes in
			if (passive_available){
				close(passive_listener_sock.listener);
				cached_ports[passive_listener_sock.cached_ports_index] = 0;
			}
			passive_listener_sock = bind_passive_socket();

			if (passive_listener_sock.listener == -1){
				send(ctrl_sock, "425 Can't open data connection\n", 31, 0);
				continue;
			}

			// generates passive socket information
			uint32_t ip_num = server_addr.sin_addr.s_addr;
			unsigned char ip_dot[4];
			char ip_string[60];
			ip_dot[0] = ip_num & 0xFF;
			ip_dot[1] = (ip_num >> 8) & 0xFF;
			ip_dot[2] = (ip_num >> 16) & 0xFF;
			ip_dot[3] = (ip_num >> 24) & 0xFF;
			sprintf(ip_string, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)\n", 
				    ip_dot[0], ip_dot[1], ip_dot[2], ip_dot[3], 
				    passive_listener_sock.port_number>>8, 
				    passive_listener_sock.port_number & 0xff);
			printf("port number : %d\n", passive_listener_sock.port_number);
			size_t len = strlen(ip_string);
			send(ctrl_sock, ip_string, len+1, 0);
			passive_available = 1;
		}
		else if (strcmp(args[0], "NLST") == 0) {
			if (!verify_conn(ctrl_sock, conn_flag)) continue;
			if (args[1] != NULL){
				send(ctrl_sock, "501 Syntax error in parameters or arguments\n", 44, 0);
				continue;
			}

			if (passive_available == 0){
				send(ctrl_sock, "425 Can't open data connection\n", 31, 0);
				continue;
			}

			// accept a client
			sin_size = sizeof their_addr;
			data_fd = accept(passive_listener_sock.listener, (struct sockaddr *)&their_addr, &sin_size);
			if (data_fd == -1) {
				send(ctrl_sock, "425 Can't open data connection\n", 31, 0);
				continue;
			}

			// get current directory
			cwd = getcwd( buff, PATH_MAX + 1 );
			if( cwd != NULL ) {
				printf( "My working directory is %s.\n", cwd );
				listFiles(data_fd, cwd);
				send(ctrl_sock, "226 Closing data connection. Requested file action successful\n", 62, 0);
			}

			close(data_fd);
			close(passive_listener_sock.listener);
			cached_ports[passive_listener_sock.cached_ports_index] = 0;
			passive_available = 0;
		}
		else {
			send(ctrl_sock, "500 command not supported\n", 26, 0);
		}
	}

	// close all interacting sockets for a client
	close(ctrl_sock);
	if (passive_available != 0){
		close(passive_listener_sock.listener);
		cached_ports[passive_listener_sock.cached_ports_index] = 0;
	}	
}

// threading function to service a single client
void* connect_client(void* args) {
	int new_fd;
	int listener_sock = *(int*)args;
	struct sockaddr_storage their_addr;
	socklen_t sin_size;
	char s[INET6_ADDRSTRLEN];

	// continues to listen and provide connection for a single client
	while(1){
		// accept a client
		sin_size = sizeof their_addr;
		new_fd = accept(listener_sock, (struct sockaddr *)&their_addr, &sin_size);

		// continue to listen if accept fails
		if (new_fd == -1) {
			perror("accept");
			continue;
		}

		// service client
		inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), s, sizeof s);
		printf("server: got connection from %s\n", s);
		service_client(new_fd);
		printf("server: disconnected from %s\n", s);
	}
}

int main(int argc, char **argv) {
	int listener_sock;
	struct addrinfo hints, *servinfo, *p;
	struct sigaction sa;
	int yes=1;
	int rv;

	// Check the command line arguments
	if (argc != 2) {
		usage(argv[0]);
		return -1;
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	// argv[1] is the port number
	if ((rv = getaddrinfo(NULL, argv[1], &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}
	cached_ports[0] = atoi(argv[1]);

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		// create socket
		if ((listener_sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
			perror("server: socket");
			continue;
		}
		// set options
		if (setsockopt(listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
			perror("setsockopt");
			exit(1);
		}
		// bind socket
		if (bind(listener_sock, p->ai_addr, p->ai_addrlen) == -1) {
			close(listener_sock);
			perror("server: bind");
			continue;
		}
		break;
	}
	freeaddrinfo(servinfo); // all done with this structure

	if (p == NULL)	{
		fprintf(stderr, "server: failed to bind\n");
		exit(1);
	}
	// listen to socket
	if (listen(listener_sock, BACKLOG) == -1) {
		perror("listen");
		exit(1);
	}
	// setup termination handler
	sa.sa_handler = sigchld_handler; // reap all dead processes
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		perror("sigaction");
		exit(1);
	}

	// service maximum 4 clients concurrently
	void* thread1 = createThread(&connect_client, &listener_sock);
	int status1 = runThread(thread1, NULL);

	void* thread2 = createThread(&connect_client, &listener_sock);
	int status2 = runThread(thread2, NULL);

	void* thread3 = createThread(&connect_client, &listener_sock);
	int status3 = runThread(thread3, NULL);

	void* thread4 = createThread(&connect_client, &listener_sock);
	int status4 = runThread(thread4, NULL);

	joinThread(thread1, NULL);
	joinThread(thread2, NULL);
	joinThread(thread3, NULL);
	joinThread(thread4, NULL);

	return 0;
}
