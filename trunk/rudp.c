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
#include <time.h>

#include "event.h"
#include "rudp.h"
#include "rudp_api.h"

/*===============================================================================
 * Structures
 *==============================================================================*/
typedef struct 
{
	struct rudp_hdr	header;
	char 	data[RUDP_MAXPKTSIZE];
}__attribute__((packed)) rudp_packet_t;

/* Structure for outgoing packet buffer */
struct send_data_buffer
{
        int send_flag;                          // 1: packet sent, 0: not yet sent
        rudp_packet_t rudp_packet;              // data to be sent
        int len;				// data length
        int transcnt;			// transmission counter
        struct send_data_buffer *next_buff;	// pointer to the next buffer area
};

/* Structure for send window */
struct send_data_window
{
        int send_flag;                          // 1: packet sent, 0: not yet sent
        rudp_packet_t *rudp_packet;             // data to be sent
        int len;				// data length
        struct send_data_window *next_buff;     // pointer to the next buffer area
};

/* Structure for list of peers to send data to */
struct rudp_send_peer
{
	int status;                             // 1:initial, 2:sending DATA, 3:closing socket
	struct sockaddr_in rsock_addr;		// the socket address of the destination
	int seq;                                // sequence number
	struct send_data_buffer *queue_buff;	// outgoing data buffer
	struct send_data_window *window;        // window buffer
	struct rudp_send_peer *next_peer;
};

/* Structure for list of peers to receive data from */
struct rudp_recv_peer
{
	int status;             	//
	struct sockaddr_in rsock_addr;	// the socket address of the incoming peer
	int seq;                	// sequence number
	struct rudp_recv_peer *next_recv_peer;
};

/* Structure for list of open sockets */
struct rudp_socket_type
{
	int rsocket_fd;				// socket file descriptor
	struct sockaddr_in rsock_addr;		// the socket name
	struct rudp_send_peer *outgoing_peer;	// list of send peer
	struct rudp_recv_peer *incoming_peer;	// list of receive peer
	int (*recvfrom_handler_callback)(rudp_socket_t, struct sockaddr_in *, char *, int);
	int (*event_handler_callback)(rudp_socket_t, rudp_event_t, struct sockaddr_in *);
	struct rudp_socket_type *next_node;
};

typedef struct rudp_socket_type rudp_socket_node;

typedef enum 
{ 
	FALSE=0,
	TRUE=1,
} bool;

/*===============================================================================
 * Global variables
 *==============================================================================*/

rudp_socket_node *rudp_list_head = NULL;
bool rudp_fin_received = FALSE;

/*===============================================================================
 * Funtion prototypes
 *==============================================================================*/
struct rudp_socket_type *rdup_add_socket(int fd, int port, struct sockaddr_in addr);
void rdup_add_send(struct rudp_socket_type *rsocket, struct sockaddr_in *to);
int rudp_receive_data(int fd, void *arg);
void rudp_process_received_packet(void *buf, rudp_socket_node *rsocket, int len);
int rudp_send_ack_packet(rudp_socket_node *rsocket, struct sockaddr_in *from, int seq_num);
int transmit(struct rudp_send_peer *send_peer, int seqno);
int rudp_send_data(rudp_socket_node *rsocket, struct sockaddr_in *from, int seq);
void remove_send_peer(struct rudp_socket_type *rsock, struct rudp_send_peer *node);
int retransmit(int fd, void *arg);

/*==================================================================
 * Add RUDP socket to the node list
 *==================================================================*/
struct rudp_socket_type *rdup_add_socket(int fd, int port, struct sockaddr_in addr)
{
	rudp_socket_node *node = NULL;

	node = (rudp_socket_node*)malloc(sizeof(rudp_socket_node));
	
	if(node == NULL)
	{
		fprintf(stderr, "rudp: rudp_socket_node allocation failed.\n");
		return NULL;
	}
	
	node->rsocket_fd = fd;
	node->rsock_addr = (struct sockaddr_in)addr;
	node->outgoing_peer = NULL;
	node->incoming_peer = NULL;
	node->recvfrom_handler_callback = NULL;
	node->event_handler_callback = NULL;
	node->next_node = rudp_list_head;
	rudp_list_head = node;
	
	return node;
}

/*==================================================================
 * Add a destination address to the send peer list
 *==================================================================*/
void rdup_add_send(struct rudp_socket_type *rsocket, struct sockaddr_in *to)
{
	struct rudp_send_peer *node = NULL;

	node = (struct rudp_send_peer*)malloc(sizeof(struct rudp_send_peer));
	
	if(node == NULL)
	{
		fprintf(stderr, "rudp: rudp_add_send allocation failed.\n");
		return;
	}
	
	node->status = INITIAL;
	node->rsock_addr = *to;
	node->seq = -1;
	node->queue_buff = NULL;
	node->window = NULL;
	node->next_peer= rsocket->outgoing_peer;
	rsocket->outgoing_peer = node;
	
	return;
}

/*==================================================================
 * Add RUDP packet to the buffer
 *==================================================================*/
void rudp_add_packet(struct send_data_buffer **head, struct send_data_buffer *packet)
{
	struct send_data_buffer *current, *prev;

	current = *head;
	prev = current;

	while(current != NULL)
	{
		prev = current;
		current = current->next_buff;
	}

	current = (struct send_data_buffer *)malloc(sizeof(struct send_data_buffer));
	current = packet;
	current->send_flag = 0;
	if(prev != NULL)
	{
		prev->next_buff = current;
	}
	else
	{
		*head = current;
	}
}

/*==================================================================
 * Add RUDP packet to the send window
 *==================================================================*/
void rudp_add_window(struct send_data_window **window_head, struct send_data_buffer *buffer)
{
	struct send_data_window *current, *prev;

	current = *window_head;
	prev = current;

	while(current != NULL)
	{
		prev = current;
		current = current->next_buff;
	}

	current = (struct send_data_window *)malloc(sizeof(struct send_data_window));

	current->send_flag = 0;
	current->rudp_packet = &buffer->rudp_packet;
	current->len = buffer->len;
	current->next_buff = NULL;
	if(prev != NULL)
	{
		prev->next_buff = current;
	}
	else
	{
		*window_head = current;
	}

}

/*==================================================================
 * Finding RUDP socket in the open sockets list
 *==================================================================*/
struct rudp_socket_type *find_rudp_socket(rudp_socket_t rudp_socket) 
{
	struct rudp_socket_type *current = rudp_list_head;
	while(current != NULL)
	{
		if(current == rudp_socket)
		{
			return current;
		}
		current = current->next_node;
	}
	return NULL;
}

/*==================================================================
 * Set up RUDP packet
 *==================================================================*/
struct send_data_buffer *set_rudp_packet(int type, int seq, void *data, int len) 
{
	struct send_data_buffer *buffer = NULL;
	buffer = (struct send_data_buffer *)malloc(sizeof(struct send_data_buffer));
	memset(buffer, 0x0, sizeof(struct send_data_buffer));

	buffer->send_flag = 0;
	buffer->rudp_packet.header.version = RUDP_VERSION;
	buffer->rudp_packet.header.type = type;
	buffer->rudp_packet.header.seqno = seq;
	buffer->transcnt = 0;

	memcpy(buffer->rudp_packet.data, data, len);
	buffer->len = len;


	buffer->next_buff = NULL;

	return buffer; 

}

/*==================================================================
 * Remove peer from outgoing peer list
 *==================================================================*/
void remove_send_peer(struct rudp_socket_type *rsock, struct rudp_send_peer *node)
{
	struct rudp_send_peer *send_peer = NULL;
	struct rudp_send_peer *prev = NULL;

	send_peer = rsock->outgoing_peer;
	while(send_peer != NULL)
	{
		if(send_peer == node)
		{
			break;
		}
		prev = send_peer;
		send_peer = send_peer->next_peer;
	}

	if(prev != NULL && send_peer != NULL)
	{
		prev->next_peer = send_peer->next_peer;
		printf("rudp: Freeing memory to peer %s:%d\n",
				inet_ntoa(send_peer->rsock_addr.sin_addr), ntohs(send_peer->rsock_addr.sin_port));
		free(send_peer);
	}
	else if(prev == NULL)
	{
		rsock->outgoing_peer = send_peer->next_peer;
		printf("rudp: Freeing memory to peer %s:%d\n",
				inet_ntoa(send_peer->rsock_addr.sin_addr), ntohs(send_peer->rsock_addr.sin_port));
		free(send_peer);
	}
}

/*==================================================================
 * Remove socket from open socket list
 *==================================================================*/
void remove_socket(struct rudp_socket_type *rsock)
{
	struct rudp_socket_type *current = NULL;
	struct rudp_socket_type *prev = NULL;

	current = rudp_list_head;
	while(current != NULL)
	{
		if(current == rsock)
		{
			break;
		}
		prev = current;
		current = current->next_node;
	}

	if(prev != NULL && current != NULL)
	{
		prev->next_node = current->next_node;

		// delete event register for the socket
		int ret = event_fd_delete(rudp_receive_data, (void *)current);
		if(ret<0){
		    perror("Error deregistering event_fd\n");
		}

		// free memory allocation
		printf("rudp: Freeing memory to socket %d\n", current->rsocket_fd);
		free(current);
	}
	else if(prev == NULL)
	{
		rudp_list_head = current->next_node;

		// delete event register for the socket
		int ret = event_fd_delete(rudp_receive_data, (void *)current);
		if(ret<0){
		    perror("Error deregistering event_fd\n");
		}

		// free memory allocation
		printf("rudp: Freeing memory to socket %d\n", current->rsocket_fd);
		free(current);
	}
}

/*==================================================================
 * Compare to sockaddr data
 *==================================================================*/
int compare_sockaddr(struct sockaddr_in *s1, struct sockaddr_in *s2){

	char fromip[16];
	char toip[16];
	strcpy(fromip, inet_ntoa(s1->sin_addr));
	strcpy(toip, inet_ntoa(s2->sin_addr));
    
	if((s1->sin_family == s2->sin_family) && (strcmp(fromip, toip)==0) && (s1->sin_port == s2->sin_port))
	{
		return 1;
	}
	
	return 0;
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
	if(bytes <= 0)
	{
		printf("[Error]: recvfrom failed(fd=%d).\n", fd);
		return -1;
	}
	bytes -= sizeof(struct rudp_hdr);		// Only Data size
	
	rsocket->rsock_addr = dest_addr;

	rudp_process_received_packet((void*)&rudp_data, rsocket, bytes);
        return 0;
}

/*===================================================================
 * Analyzing Packet type
 * - RUDP_DATA, RUDP_ACK, RUDP_SYN, RUDP_FIN
 *===================================================================*/
void rudp_process_received_packet(void *buf, rudp_socket_node *rsocket, int len)
{
	rudp_packet_t *rudp_data = NULL;
	struct sockaddr_in from;
	int seq_num, type;
	
	rudp_data = (rudp_packet_t*)buf;
	from = rsocket->rsock_addr;
	type = rudp_data->header.type;
	seq_num = rudp_data->header.seqno;

	switch(type)
	{
	case RUDP_DATA:

		printf("rudp: RUDP_DATA (seq: %d)\n", seq_num);
		// Send RUDP_ACK		
		rudp_send_ack_packet(rsocket, &from, seq_num+1);		
		
		/*  recvfrom_handler_callback: rudp_receiver() <vs_recv.c>
		 */
		rsocket->recvfrom_handler_callback(rsocket, &from, rudp_data->data, len);	
		break;
	case RUDP_ACK:
		printf("rudp: RUDP_ACK (seq: %d) from %s:%d\n", seq_num, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
		// Send RUDP_DATA
		rudp_send_data(rsocket, &from, seq_num);
		break;
	case RUDP_SYN:
		printf("rudp: RUDP_SYN (seq: %d) from %s:%d\n", seq_num, inet_ntoa(from.sin_addr), ntohs(from.sin_port));		
		// Send RUDP_ACK
		rudp_send_ack_packet(rsocket, &from, seq_num+1);
		break;
	case RUDP_FIN:
		printf("rudp: RUDP_FIN (seq: %d)\n", seq_num);
		rudp_fin_received = TRUE;
		// Send RUDP_ACK
		rudp_send_ack_packet(rsocket, &from, seq_num+1);
		break;
	default:
		break;
	}
	
}

/*===================================================================================

 * Received an ACK packet, continue sending packet to the sender
 *===================================================================================*/
int rudp_send_data(rudp_socket_node *rsocket, struct sockaddr_in *from, int seq)
{
	struct rudp_socket_type *rsock;		// pointer to open socket
	struct rudp_send_peer *send_peer;	// pointer to send peer list
	struct send_data_window *window;	// pointer to send window
	struct send_data_buffer *packet_buff;	// pointer to packet buffer
	unsigned int seqno;			// packet sequence number
	int sockfd; // dummy variables

	// check whether the socket is in the open socket list
	rsock = find_rudp_socket(rsocket);
	if(rsock != NULL)
	{
		// check whether the sender address is in the outgoing peer list
		send_peer = rsock->outgoing_peer;
		while(send_peer != NULL)
		{
			if(compare_sockaddr(from, &send_peer->rsock_addr))
			{
				// exit loop
				break;
			}
			send_peer = send_peer->next_peer;
		}
		
		if(send_peer != NULL)
		{
			// delete timeout register
		    if(event_timeout_delete(retransmit, send_peer->window->rudp_packet) == -1)
		    {
		    	perror("rudp: Error deleting timeout..\n");
				return -1;
		    }
			// check the sequence number, remove data with smaller sequence number from window
			window = send_peer->window;
			packet_buff = send_peer->queue_buff;
			while(window != NULL)
			{
				seqno = window->rudp_packet->header.seqno;
				if(seqno < seq)
				{
					send_peer->window = window->next_buff;
					send_peer->queue_buff = packet_buff->next_buff;
					free(window);
					free(packet_buff);
					window = send_peer->window;
					packet_buff = send_peer->queue_buff;
				}
				else
				{
					window = window->next_buff;
					packet_buff = packet_buff->next_buff;
				}
			}

			// check whether the peer status is FINISHED
			if(send_peer->status == FINISHED && send_peer->window == NULL)
			{
				// remove peer from outgoing peer list
				remove_send_peer(rsock, send_peer);

				// if the outgoing peer list has became empty
				if(rsock->outgoing_peer == NULL)
				{
					printf("rudp: Signaling RUDP_EVENT_CLOSED to application\n");
					rsock->event_handler_callback((rudp_socket_t)rsock, RUDP_EVENT_CLOSED, NULL);

					// if the incoming peer list is also empty, close the socket
					if(rsock->incoming_peer == NULL)
					{
						sockfd = rsock->rsocket_fd;

						// remove socket from open socket list
						remove_socket(rsock);
						printf("rudp: Closing socket %d\n", sockfd);
				 		close(sockfd);
					}
				}
				return 0;
			}

			// if not, continue transmission to the peer
			// printf("rudp: Debug: Continue transmission.\n");
			transmit(send_peer, rsock->rsocket_fd);
			return 0;
		}
		else
		{
			printf("rudp: Debug: Error, ACK received from unknown peer\n");
			return -1;
		}
	}
	else
	{
		return -1;
	}
}
/*===================================================================================
 * Send an ACK Packet to the Sender
 *===================================================================================*/
int rudp_send_ack_packet(rudp_socket_node *rsocket, struct sockaddr_in *to, int seq_num)
{
	rudp_packet_t rudp_ack;
	rudp_socket_node *socket;
	struct rudp_recv_peer *recv_peer, *new_recv_peer, *prev_recv_peer;
	int ret = 0;
	
	memset(&rudp_ack, 0x0, sizeof(rudp_packet_t));

	rudp_ack.header.version = RUDP_VERSION;
	rudp_ack.header.type = RUDP_ACK;
	rudp_ack.header.seqno = seq_num;

	ret = sendto((int)rsocket->rsocket_fd, (void *)&rudp_ack, sizeof(rudp_packet_t), 0,
				(struct sockaddr*)to, sizeof(struct sockaddr_in));
	if(ret <= 0)
	{
		fprintf(stderr, "rudp: sendto fail(%d)\n", ret);
		return -1;
	}
	// Add receiving peers to a linked list
	socket = find_rudp_socket(rsocket);
	
	new_recv_peer = (struct rudp_recv_peer*)malloc(sizeof(struct rudp_recv_peer));
	if(new_recv_peer == NULL) 
	{
		fprintf(stderr, "rudp_send_ack_packet: Memory allocation failed.\n");		
		return -1;
	}
    
	memset(new_recv_peer, 0x0, sizeof(struct rudp_recv_peer));

	new_recv_peer->status = SENDING;
	new_recv_peer->seq = seq_num;
	new_recv_peer->next_recv_peer = NULL;
	new_recv_peer->rsock_addr = *to;
    
	recv_peer = socket->incoming_peer;

	// Move to the end of the incoming peer list
	while(recv_peer)
	{
		printf("rudp_send_ack_packet: recv_peer:0x%x \n", recv_peer);
		prev_recv_peer = recv_peer;
		recv_peer = recv_peer->next_recv_peer;		
	}
    
	if(prev_recv_peer == NULL) 
	{
		printf("Add one recv_peer.\n");
		socket->incoming_peer = new_recv_peer;
	} 
	else 
	{
		printf("Add the peer to the end of the list\n");
		prev_recv_peer->next_recv_peer = new_recv_peer;		
	}

	return 0;
}

/*==================================================================
 * 
 * rudp_socket: Create a RUDP socket. 
 * May use a random port by setting port to zero. 
 *==================================================================*/

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
	
	if(port == 0) 
	{
		port = rand() % 120000 + 1024;
	}
	printf("rudp_socket: Socketfd: %d, Port number: %d\n", sockfd, port);

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
	rudp_socket = rdup_add_socket(sockfd, port, in);
	if(rudp_socket == NULL)
	{
		fprintf(stderr, "rudp: create_rudp_socket failed.\n");
		return NULL;
	}

	// Registeration a function for receiving socket data from Network
	ret = event_fd((int)sockfd, &rudp_receive_data, 
					(void*)rudp_socket, "rudp_receive_data");

	if(ret < 0)
	{
		printf("[Error] event_fd failed: rudp_receive_data()\n");
		return NULL;
	}
	return (rudp_socket_t*)rudp_socket;
}

/* 
 *rudp_close: Close socket 
 */ 

int rudp_close(rudp_socket_t rsocket) 
{
	struct rudp_socket_type *rsock;		// pointer to open socket list
	struct rudp_send_peer *send_peer;	// pointer to send peers list
	struct rudp_recv_peer *recv_peer, *prev_peer;
	struct send_data_buffer *packet_buff;	// pointer to packet buffer
	unsigned int seqno;			// packet sequence number

	rudp_socket_node *rudp_socket = NULL;
	rudp_socket = (rudp_socket_node*)rsocket;

	// check whether the socket is in the open socket list
	rsock = find_rudp_socket(rsocket);
	if(rsock == NULL)
	{
		fprintf(stderr, "rudp_close: rsock is NULL.\n");
		return -1;
	}

	// Receiver side socket closing
	recv_peer=rsock->incoming_peer;
	
	while (recv_peer != NULL)
	{	
		prev_peer = recv_peer;
		prev_peer->status = FINISHED;
		prev_peer->next_recv_peer = NULL;
		recv_peer = recv_peer->next_recv_peer; 
	}

	// sender side
	send_peer = rsock->outgoing_peer;
	while(send_peer != NULL)
	{
		send_peer->status = CLOSING;

		// append FIN to the send buffer
		seqno = send_peer->seq;
		packet_buff = set_rudp_packet(RUDP_FIN, seqno+1, NULL, 0);
		rudp_add_packet(&send_peer->queue_buff, packet_buff);
		send_peer->seq = seqno+1;

		send_peer = send_peer->next_peer;
	}

	return 0;
}

/*==================================================================
 * 
 *rudp_recvfrom_handler: Register receive callback function 
 * 
 *==================================================================*/
int rudp_recvfrom_handler(rudp_socket_t rsocket, 
			  int (*handler)(rudp_socket_t, struct sockaddr_in *, 
					 char *, int)) 
{
	rudp_socket_node *rudp_socket;

	printf("rudp: rudp_recvfrom_handler.\n");
	rudp_socket = (rudp_socket_node*)rsocket;
	rudp_socket->recvfrom_handler_callback = handler;		// rudp_receiver

	return 0;
}

/*==================================================================
 * 
 *rudp_event_handler: Register event handler callback function 
 * 
 *==================================================================*/
int rudp_event_handler(rudp_socket_t rsocket, 
		       int (*handler)(rudp_socket_t, rudp_event_t, 
				      struct sockaddr_in *)) 
{
	rudp_socket_node *rudp_socket;

	printf("rudp: rudp_event_handler.\n");
	rudp_socket = (rudp_socket_node*)rsocket;
	rudp_socket->event_handler_callback = handler;
        return 0;
}


/*==================================================================
 * 
 * rudp_sendto: Handle packet to be sent to the receiver.
 * 
 *==================================================================*/
int rudp_sendto(rudp_socket_t rsocket, void* data, int len, struct sockaddr_in* to) {

	struct rudp_send_peer *send_peer;	// pointer to send peers list
	struct rudp_socket_type *rsock;		// pointer to open socket list
	struct send_data_buffer *packet_buff;	// pointer to packet buffer
	unsigned int seqno;			// packet sequence number

	// check whether the socket is in the open socket list
	rsock = find_rudp_socket(rsocket);

	if(rsock != NULL) // if the socket is already open
	{
		// check whether the destination address is in the outgoing peer list
		send_peer = rsock->outgoing_peer;
		while(send_peer != NULL)
		{
			if(compare_sockaddr(to, &send_peer->rsock_addr))
			{
				// if so, append the data to the send buffer
				seqno = send_peer->seq;
				packet_buff = set_rudp_packet(RUDP_DATA, seqno+1, data, len);
				rudp_add_packet(&send_peer->queue_buff, packet_buff);
			
				// increase the sequence number
				send_peer->seq = seqno+1;

				// try to transmit data (if window is available)
				transmit(send_peer, rsock->rsocket_fd);

				// return out 
				return 0;
			}
			send_peer = send_peer->next_peer;
		}

		// if the destination address is not in the list add it to send peer list 
		rdup_add_send(rsock, to);
		send_peer = rsock->outgoing_peer;

		// set up random sequence number
		srand(time(NULL));
		seqno = rand() % 0xFFFFFFFF + 1;

		// add the data to the send peer buffer: SYN
		packet_buff = set_rudp_packet(RUDP_SYN, seqno, NULL, 0); 
		rudp_add_packet(&send_peer->queue_buff, packet_buff);

		// add the data to the send peer buffer: DATA
		packet_buff = set_rudp_packet(RUDP_DATA, seqno+1, data, len); 
		rudp_add_packet(&send_peer->queue_buff, packet_buff);
		send_peer->seq = seqno+1;

		// start transmission to the peer
		send_peer->status = SENDING;
		printf("rudp: Debug: Start transmission.\n");
		transmit(send_peer, rsock->rsocket_fd);
		return 0;

	}
	else // if the socket is not in the open socket list, return error
	{	
		printf("rudp: Debug: Error, socket not exist.\n");
		return -1;
	}
}

/*==================================================================
 * 
 * transmit: Send packet to the receiver. 
 * 
 *==================================================================*/
int transmit(struct rudp_send_peer *send_peer, int socketfd)
{
	int wcnt;				// counter of window occupancy
	struct send_data_window *window;	// pointer to the send window
	struct send_data_buffer *queue;		// pointer to the send queue
	int cnt, length, seq, dumcnt;		// dummy variables
	int socket;				// socket file descriptor
	struct sockaddr_in rsock_addr;		// socket address
	struct timeval timer, t0, t1;		// timer variables

	// initialize timer variables
	timer.tv_sec = timer.tv_usec = 0;
	t0.tv_sec = t0.tv_usec = 0;
	t1.tv_sec = t1.tv_usec = 0;

	// check amount of packets in the window
	queue = send_peer->queue_buff;
	window = send_peer->window;
	wcnt = 0;
	dumcnt = 0;
	while(window != NULL)
	{
		wcnt++;
		window = window->next_buff;
		queue = queue->next_buff;	// shift the queue buffer pointer because the data is in window already
	}

	// if less than RUDP_WINDOW and there still data in queue, copy data from queue buffer to fill in the window
	if((wcnt < RUDP_WINDOW) && (queue != NULL))
	{
		printf("rudp: Window size: %d.\n", (RUDP_WINDOW-wcnt));
		for(cnt=0; cnt<(RUDP_WINDOW-wcnt); cnt++)
		{
			rudp_add_window(&send_peer->window, queue);
			queue = queue->next_buff;
			dumcnt++;

			if(queue == NULL)	// no more data
			{
				break;
			}
		}
		printf("rudp: Added %d packets to the window.\n", dumcnt);
	}

	// transmit all untransmitted data in window
	window = send_peer->window;
	socket = socketfd;
	rsock_addr = send_peer->rsock_addr;
	dumcnt = 0;

	for(cnt=0; cnt<RUDP_WINDOW && window != NULL; cnt++)
	{
		// untransmitted window
		if(window->send_flag == 0)
		{
			seq = window->rudp_packet->header.seqno;
			length = sizeof(window->rudp_packet->header) + window->len;

			if(sendto(socket, window->rudp_packet, length, 0, (struct sockaddr *)&rsock_addr, sizeof(rsock_addr)) <= 0)
			{
				perror("Error in send_to: ");
				return -1;
			}
			printf("rudp: Packet sent to (%s:%d) via socket (%d) seq (%d).\n", 
							inet_ntoa(rsock_addr.sin_addr), ntohs(rsock_addr.sin_port), socket, seq);

			window->send_flag = 1;	// set to status to sent
			dumcnt++;  		// increment packet counter

			// check whether the data is FIN
			if(window->rudp_packet->header.type == RUDP_FIN)
			{
				// if so, set status to FINISHED
				send_peer->status = FINISHED;
				printf("rudp: FIN sent to %s:%d\n", inet_ntoa(rsock_addr.sin_addr), ntohs(rsock_addr.sin_port));
			}

			// Start the timeout callback with event_timeout
			timer.tv_sec = RUDP_TIMEOUT/1000;           	// convert to second
			timer.tv_usec = (RUDP_TIMEOUT%1000) * 1000; 	// convert to micro
			gettimeofday(&t0, NULL);     			// current time of the day
			timeradd(&t0, &timer, &t1);  //add the timeout time with thecurrent time of the day

			// register timeout
			if(event_timeout(t1, &retransmit, window->rudp_packet, "timer_callback") == -1)
			{
			    	perror("rudp: Error registering event_timeout\n");
			    	return -1;
			}
		}

		window = window->next_buff;
		if(window == NULL)
		{
			printf("rudp: Number of packet sent: %d.\n", dumcnt);
			break;
		}
	}

	return 0;
}


/*==================================================================
 *
 * retransmit: Retransmit packet to the receiver.
 *
 *==================================================================*/
int retransmit(int fd, void *arg)
{
	struct rudp_socket_type *rsock;		// pointer to open socket list
	struct rudp_send_peer *send_peer;	// pointer to send peers list
	struct send_data_buffer *packet_buff;	// pointer to packet buffer

	rudp_packet_t *rudp_packet;		// pointer to RUDP packet
	int sockfd;				// socket file descriptor
	struct sockaddr_in rsock_addr;		// the address of the destination
	int length;				// transmission data length
	struct timeval timer, t0, t1;		// timer variables

	// initialize timer variables
	timer.tv_sec = timer.tv_usec = 0;
	t0.tv_sec = t0.tv_usec = 0;
	t1.tv_sec = t1.tv_usec = 0;

	int found = 0;

	rudp_packet = (rudp_packet_t *)arg;

	// search for the packet owner (brute force method, lol..)
	rsock = rudp_list_head;
	while(rsock != NULL)
	{
		sockfd = rsock->rsocket_fd;
		send_peer = rsock->outgoing_peer;

		while(send_peer != NULL)
		{
			rsock_addr = send_peer->rsock_addr;
			packet_buff = send_peer->queue_buff;

			while(packet_buff != NULL)
			{
				if(rudp_packet == &(packet_buff->rudp_packet))
				{
					found = 1;
					break;
				}
				packet_buff = packet_buff->next_buff;
			}
			if(found)
				break;
			send_peer = send_peer->next_peer;
		}
		if(found)
			break;
		rsock = rsock->next_node;
	}

	if(found && packet_buff->transcnt < RUDP_MAXRETRANS)
	{
		length = packet_buff->len + sizeof(rudp_packet->header);
		packet_buff->transcnt++;

		printf("rudp: retransmit packet seq=%d to %s:%d\n", 
				rudp_packet->header.seqno, inet_ntoa(rsock_addr.sin_addr), ntohs(rsock_addr.sin_port));

		if(sendto(sockfd, rudp_packet, length, 0, (struct sockaddr *)&rsock_addr, sizeof(rsock_addr)) <= 0)
		{
			perror("Error in send_to: ");
			return -1;
		}

		// register timeout again
		timer.tv_sec = RUDP_TIMEOUT/1000;           	// convert to second
		timer.tv_usec = (RUDP_TIMEOUT%1000) * 1000; 	// convert to micro
		gettimeofday(&t0, NULL);     			// current time of the day
		timeradd(&t0, &timer, &t1);  //add the timeout time with thecurrent time of the day

		if(event_timeout(t1, &retransmit, &packet_buff->rudp_packet, "timer_callback") == -1)
		{
		    	perror("rudp: Error registering event_timeout\n");
		    	return -1;
		}

		return 0;
	}
	else if(!found)
	{
		printf("rudp: Could not find retransmission destination.\n");
		return -1;
	}
	else
	{
		printf("rudp: Retransmission count exceeded the limit %d times.\n", RUDP_MAXRETRANS);
		return -1;
	}
}
