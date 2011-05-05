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

/* Structure for outgoing packet buffer */
struct send_data_buffer
{
	int send_flag;				// 1: packet sent, 0: not yet sent
	rudp_packet_t rudp_packet;		// data to be sent
	int len;
	struct send_data_buffer * next_buff;	// pointer to the next buffer area
};

/* Structure for send window */
struct send_data_window
{
	int send_flag;				// 1: packet sent, 0: not yet sent
	rudp_packet_t *rudp_packet;		// data to be sent
	int len;
	struct send_data_window *next_buff;	// pointer to the next buffer area
};

/* Structure for list of peers to send data to */
struct rudp_send_peer
{
	struct rudp_socket_type *rudp_node;	// list of peer nodes
	int status;				// 1:initial, 2:SYN sent, 3:sending DATA, 4:FIN sent
	struct send_data_window *window;	// window buffer
	struct send_data_buffer *queue_buf;	// queue buffer (wait for window to be empty)
	int seq;				// sequence number
	struct rudp_send_peer *next_send_peer;
};

/* Structure for list of peers to receive data from */
struct rudp_recv_peer
{
	int status;		// 
	int seq;		// sequence number
	struct rudp_recv_peer *next_recv_peer;
};

/* Structure for list of open sockets */
struct rudp_socket_type
{
	int rsocket_fd;
	int port;
	struct sockaddr_in	rsock_addr;	// the socket address of the destination(to/from)
	int (*recvfrom_handler_callback)(rudp_socket_t, struct sockaddr_in *, char *, int);
	int (*event_handler_callback)(rudp_socket_t, rudp_event_t, struct sockaddr_in *);
	struct rudp_socket_type *next_node;
};

typedef struct rudp_socket_type rudp_socket_node;

/* The head of list of open sockets */
rudp_socket_node *rudp_list_head = NULL;

/* The head of send peer list */
struct rudp_send_peer *send_peer_head = NULL;

/*===============================================================================
 * Function prototypes
 *==============================================================================*/
struct rudp_socket_type *rdup_add_socket(int fd, int port, struct sockaddr_in addr);
int rudp_receive_data(int fd, void *arg);
int rudp_process_received_packet(void *buf, rudp_socket_node *rsocket, int len);
int transmit(struct rudp_send_peer *send_peer, int seqno);

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
	node->port = port;
	node->rsock_addr = (struct sockaddr_in)addr;
	node->recvfrom_handler_callback = NULL;
	node->event_handler_callback = NULL;
	node->next_node = rudp_list_head;
	rudp_list_head = node;
	
	return node;
}

/*==================================================================
 * Add RUDP socket to the send peer list
 *==================================================================*/
void rdup_add_send(struct rudp_socket_type *rsocket)
{
	struct rudp_send_peer *node = NULL;

	node = (struct rudp_send_peer*)malloc(sizeof(struct rudp_send_peer));
	
	if(node == NULL)
	{
		fprintf(stderr, "rudp: rudp_add_send allocation failed.\n");
		return;
	}
	
	node->rudp_node = rsocket;
	node->status = 1;
	node->window = NULL;
	node->queue_buf = NULL;
	node->seq = 0;
	node->next_send_peer= send_peer_head;
	send_peer_head = node;
	
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
/*
struct send_data_buffer
{
	int send_flag;				// 1: packet sent, 0: not yet sent
	rudp_packet_t rudp_packet;		// data to be sent
	struct send_data_buffer * next_buff;	// pointer to the next buffer area

};
*/
}
/*==================================================================
 * Finding RUDP socket in the send peer list
 *==================================================================*/
struct rudp_send_peer *find_send_peer(rudp_socket_t rudp_socket) 
{
	struct rudp_send_peer *current = send_peer_head;
	while(current != NULL)
	{
		if(current->rudp_node->rsocket_fd == (int)rudp_socket)
		{
			return current;
		}
	}
	return NULL;
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

	strncpy(buffer->rudp_packet.data, data, len);
	buffer->len = len;

	buffer->next_buff = NULL;

	return buffer; 

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
	bytes -= sizeof(struct rudp_hdr);		// Only Data size
	
	rsocket->rsock_addr = dest_addr;

	rudp_process_received_packet((void*)&rudp_data, rsocket, bytes);
	return 0;
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
	return 0;
}
/*===================================================================
 * 
 *===================================================================*/

// Add linked list

// Delete linked list

// recvfrom_handler_callback function

// timeout event callbacck function
 

/*==================================================================
 * 
 * rudp_socket: Create a RUDP socket. 
 * May use a random port by setting port to zero. 
 *
 *==================================================================*/
rudp_socket_t rudp_socket(int port) 
{
	rudp_socket_node *rudp_socket = NULL;
	int sockfd = -1;
	struct sockaddr_in in;	
	
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
	rudp_socket = rdup_add_socket(sockfd, port, in);
	if(rudp_socket == NULL)
	{
		fprintf(stderr, "rudp: create_rudp_socket failed.\n");
		return NULL;
	}

					
	return (rudp_socket_t*)rudp_socket;
}


/*==================================================================
 * 
 *rudp_close: Close socket 
 * 
 *==================================================================*/
int rudp_close(rudp_socket_t rsocket) {
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

	printf("rudp_recvfrom_handler\n");
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

	printf("rudp_event_handler\n");
	rudp_socket = (rudp_socket_node*)rsocket;
	rudp_socket->event_handler_callback = handler;
	
	return 0;
}


/*==================================================================
 * 
 * rudp_sendto: Handle packet to be send to the receiver. 
 * 
 *==================================================================*/
int rudp_sendto(rudp_socket_t rsocket, void* data, int len, struct sockaddr_in* to) {

	struct rudp_send_peer *send_peer;	// pointer to send peers list
	struct rudp_socket_type *rsock;		// pointer to open socket list
	struct send_data_buffer *packet_buff;	// pointer to packet buffer
	unsigned int seqno;			// packet sequence number

	// check whether the socket is in the send peer list
	send_peer = find_send_peer(rsocket);
	if(send_peer != NULL)
	{
		// if so, check whether the sockaddr is the same
		printf("Debug: The peer is in the send list.\n");
		if(compare_sockaddr(to, &send_peer->rudp_node->rsock_addr))
		{
			// if so, append the data to the send buffer
			seqno = send_peer->seq;
			packet_buff = set_rudp_packet(RUDP_DATA, seqno+1, data, len);
			rudp_add_packet(&send_peer->queue_buf, packet_buff);
			
			// increase the sequence number
			send_peer->seq = seqno+1; 

			return 0;
		}
		// if the sockaddr is different, return -1
		else
		{
			return -1;
		}
	}
	else
	{	
		printf("Debug: The peer is NOT in the send list.\n");
		// if not in send peer list, check whether the socket is open already
		rsock = find_rudp_socket(rsocket);

		// if so, add the to address to send peer list
		if(rsock != NULL)
		{
			printf("Debug: Adding the peer to the send list.\n");
			rdup_add_send(rsock, to);

			// add the data to the send peer buffer: SYN
			packet_buff = set_rudp_packet(RUDP_SYN, (rand() % 0xFFFFFFFF + 1), NULL, 0); 
			rudp_add_packet(&send_peer_head->queue_buf, packet_buff);

			// add the data to the send peer buffer: DATA
			packet_buff = set_rudp_packet(RUDP_DATA, 0, data, len); 
			rudp_add_packet(&send_peer_head->queue_buf, packet_buff);

			// start transmission to the peer
			printf("Debug: Start transmission.\n");
			transmit(send_peer_head, 0);
			return 0;

		}
		else
		{
			// return -1 if not open
			printf("Debug: Error, socket not exist.\n");
			return -1;
		}
	}
}

/*==================================================================
 * 
 * transmit: Send packet to the receiver. 
 * 
 *==================================================================*/
int transmit(struct rudp_send_peer *send_peer, int seqno)
{
	int wcnt;				// counter of window occupancy
	struct send_data_window *window;	// pointer to the send window
	struct send_data_buffer *queue;		// pointer to the send queue
	int cnt, length;				// dummy variables
	int socket;				// socket file descriptor
	struct sockaddr_in rsock_addr;		// socket address
	struct timeval timer, t0, t1;		// timer variables

	// check amount of packets in the window
	queue = send_peer->queue_buf;
	window = send_peer->window;
	wcnt = 0;
	while(window != NULL)
	{
		wcnt++;
		window = window->next_buff;
	}

	// if less than RUDP_WINDOW, copy data from queue buffer to fill in the window
	if(wcnt < RUDP_WINDOW)
	{
		printf("Data in window: %d\n", wcnt);
		for(cnt=0; cnt<(RUDP_WINDOW-wcnt); cnt++)
		{
			rudp_add_window(&send_peer->window, queue);
			queue = queue->next_buff;
			send_peer->queue_buf = queue;
			if(queue == NULL)	// no more data
				break;
		}
	}

	// transmit all untransmitted data in window
	window = send_peer->window;
	socket = send_peer->rudp_node->rsocket_fd;
	rsock_addr = send_peer->rudp_node->rsock_addr;
	length = sizeof(window->rudp_packet->header) + window->len;
	for(cnt=0; cnt<RUDP_WINDOW; cnt++)
	{
		// untransmitted window
		if(window->send_flag == 0)
		{
			if(sendto(socket, window->rudp_packet, length, 0, (struct sockaddr *)&rsock_addr, sizeof(rsock_addr)) <= 0)
			{
				perror("Error in send_to: ");
				return -1;
			}
			window->send_flag = 1;	// set to sent data

			// Start the timeout callback with event_timeout

			timer.tv_sec = RUDP_TIMEOUT/1000;           // timeout value is given in milisecond, so convert to second
			timer.tv_usec = (RUDP_TIMEOUT%1000) * 1000; // remaining miliseconds are converted to micro
			gettimeofday(&t0, NULL);     //current time of the day
			timeradd(&t0, &timer, &t1);  //add the timeout time with teh current time of the day
	    
			//register timeout
			//if(event_timeout(t1, trans_timeout, (void *) &(peer->window[i]->packet), "timer_callback") == -1)
			{
				perror("Error while registering the event_timeout");
				return -1;
			}
		}
		window = window->next_buff;
	}

	return 0;
}

/*
struct rudp_send_peer
{
	struct rudp_socket_type *rudp_node;	// list of peer nodes
	int status;				// 1:initial, 2:SYN sent, 3:sending DATA, 4:FIN sent
	struct send_data_buffer *window;	// window buffer
	struct send_data_buffer *queue_buf;	// queue buffer (wait for window to be empty)
	int seq;				// sequence number
	struct rudp_send_peer *next_send_peer;
};
struct rudp_socket_type
{
	int rsocket_fd;
	int port;
	struct sockaddr_in	rsock_addr;	// the socket address of the destination(to/from)
	int (*recvfrom_handler_callback)(rudp_socket_t, struct sockaddr_in *, char *, int);
	int (*event_handler_callback)(rudp_socket_t, rudp_event_t, struct sockaddr_in *);
	struct rudp_socket_type *next_node;
};
struct send_data_buffer

{
	int send_flag;				// 1: packet sent, 0: not yet sent
	rudp_packet_t rudp_packet;		// data to be sent
	struct send_data_buffer * next_buff;	// pointer to the next buffer area
};
*/
