#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "event.h"
#include "rudp.h"
#include "rudp_api.h"

typedef struct 
{
	struct rudp_hdr	header;
	char 	data[RUDP_MAXPKTSIZE];
}__attribute__((packed)) rudp_packet_t;


struct rudp_socket_type
{
	int rsocket_fd;
	int port;
	struct sockaddr_in	rsock_addr;		// the socket address of the destination(to/from)
	int (*recvfrom_handler_callback)(rudp_socket_t, struct sockaddr_in *, char *, int);
	int (*event_handler_callback)(rudp_socket_t, rudp_event_t, struct sockaddr_in *);
	struct rudp_socket_type *next_node;
};

typedef struct rudp_socket_type rudp_socket_node;

rudp_socket_node *rudp_list_head = NULL;

/*===============================================================================
 * Funtion prototypes
 *==============================================================================*/
struct rudp_socket_type *rdup_create_socket(int fd, int port, struct sockaddr_in addr);
int rudp_receive_data(int fd, void *arg);
int rudp_process_received_packet(void *buf, rudp_socket_node *rsocket, int len);


struct rudp_socket_type *rdup_create_socket(int fd, int port, struct sockaddr_in addr)
{
	rudp_socket_node *node = NULL;

	node = (rudp_socket_node*)malloc(sizeof(rudp_socket_node));
	
	if(node == NULL)
	{
		fprintf(stderr, "rudp: rudp_socket_node allocation failed.\n");
		return NULL;
	}
	
	node->rsocket_fd = fd;
	node->port = port;
	node->rsock_addr = (struct sockaddr_in)addr;
	node->recvfrom_handler_callback = NULL;
	node->event_handler_callback = NULL;
	node->next_node = rudp_list_head;
	rudp_list_head = node;
	
	return node;
}

/*===================================================================
 * Handle receiving packet from Network
 * call "recvfrom()" for UDP
 *===================================================================*/
int rudp_receive_data(int fd, void *arg)
{
	rudp_socket_node *rsocket = (rudp_socket_node*)arg;
	struct sockaddr_in dest_addr;
	rudp_packet_t rudp_data;
	int addr_size;
	int bytes=0;

	memset(&rudp_data, 0x0, sizeof(rudp_packet_t));
	
	addr_size = sizeof(dest_addr);
	bytes = recvfrom((int)fd, (void*)&rudp_data, sizeof(rudp_data), 
					0, (struct sockaddr*)&dest_addr, (socklen_t*)&addr_size);

	printf("rudp: recvfrom (%d bytes/ hdr: %d)\n", bytes, sizeof(struct rudp_hdr));
	bytes -= sizeof(struct rudp_hdr);		// Only Data size
	
	rsocket->rsock_addr = dest_addr;

	rudp_process_received_packet((void*)&rudp_data, rsocket, bytes);
}

/*===================================================================
 * Analyzing Packet type
 * - RUDP_DATA, RUDP_ACK, RUDP_SYN, RUDP_FIN
 *===================================================================*/
int rudp_process_received_packet(void *buf, rudp_socket_node *rsocket, int len)
{
	rudp_packet_t *rudp_data = NULL;
	struct sockaddr_in from;
	int seq_num, type;
	
	rudp_data = (rudp_packet_t*)buf;
	from = rsocket->rsock_addr;
	type = rudp_data->header.type;
	
	switch(type)
	{
	case RUDP_DATA:
		// Send RUDP_ACK
		seq_num = rudp_data->header.seqno;
		printf("RUDP_DATA: (seqno= %d)\n", seq_num);
		rsocket->recvfrom_handler_callback(rsocket, &from, rudp_data->data, len);
		break;
	case RUDP_ACK:
		printf("RUDP_ACK\n");
		// Send RUDP_DATA
		break;
	case RUDP_SYN:
		printf("RUDP_SYN\n");
		// Send RUDP_ACK
		break;
	case RUDP_FIN:
		printf("RUDP_FIN\n");
		// Send RUDP_ACK
		break;
	default:
		break;
	}
	
}
/*===================================================================
 * 
 *===================================================================*/

// Add linked list

// Delete linked list

// recvfrom_handler_callback function

// timeout event callbacck function
 

/*==================================================================*/
/* 
 * rudp_socket: Create a RUDP socket. 
 * May use a random port by setting port to zero. 
 */

rudp_socket_t rudp_socket(int port) 
{
	rudp_socket_node *rudp_socket = NULL;
	int sockfd = -1;
	struct sockaddr_in in;	
	int ret = 0;
	
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if(sockfd < 0)
	{
		fprintf(stderr, "rudp: socket error : ");
		return NULL;
	}

	bzero(&in, sizeof(in));

	in.sin_family = AF_INET;
	in.sin_addr.s_addr = htonl(INADDR_ANY);
	in.sin_port = htons(port);


	if(bind(sockfd, (struct sockaddr *)&in, sizeof(in)) == -1)
	{
		fprintf(stderr, "rudp: bind error\n");
		return NULL;
	}

	// create rudp_socket
	rudp_socket = rdup_create_socket(sockfd, port, in);
	if(rudp_socket == NULL)
	{
		fprintf(stderr, "rudp: create_rudp_socket failed.\n");
		return NULL;
	}

	// Registeration a function for receiving socket data from Network
	ret = event_fd((int)sockfd, rudp_receive_data, 
					(void*)rudp_socket, "rudp_receive_data");
					
	return (rudp_socket_t*)rudp_socket;
}

/* 
 *rudp_close: Close socket 
 */ 

int rudp_close(rudp_socket_t rsocket) {
	return 0;
}

/* 
 *rudp_recvfrom_handler: Register receive callback function 
 */ 

int rudp_recvfrom_handler(rudp_socket_t rsocket, 
			  int (*handler)(rudp_socket_t, struct sockaddr_in *, 
					 char *, int)) 
{
	rudp_socket_node *rudp_socket;

	printf("rudp_recvfrom_handler\n");
	rudp_socket = (rudp_socket_node*)rsocket;
	rudp_socket->recvfrom_handler_callback = handler;		// rudp_receiver

	return 0;
}

/* 
 *rudp_event_handler: Register event handler callback function 
 */ 
int rudp_event_handler(rudp_socket_t rsocket, 
		       int (*handler)(rudp_socket_t, rudp_event_t, 
				      struct sockaddr_in *)) 
{
	rudp_socket_node *rudp_socket;

	printf("rudp_event_handler\n");
	rudp_socket = (rudp_socket_node*)rsocket;
	rudp_socket->event_handler_callback = handler;
	
	return 0;
}


/* 
 * rudp_sendto: Send a block of data to the receiver. 
 */

int rudp_sendto(rudp_socket_t rsocket, void* data, int len, struct sockaddr_in* to) {
	return 0;
}

