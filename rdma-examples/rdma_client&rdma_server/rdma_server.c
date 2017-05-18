/*
 * Copyright (c) 2005-2009 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under the OpenIB.org BSD license
 * below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>			
#include <rdma/rdma_verbs.h>		

static char *port = "127.0.0.1";				//default port

struct rdma_cm_id *listen_id, *id;		//rdma_cm_id: RDMA identifier
struct ibv_mr *mr, *send_mr;			//ibv_mr:memory region
int send_flags;
uint8_t send_msg[16];
uint8_t recv_msg[16];

static int run(void)
{
	struct rdma_addrinfo hints, *res;
	struct ibv_qp_init_attr init_attr;
	struct ibv_qp_attr qp_attr;
	struct ibv_wc wc;
	int ret;

	//hints: contains hints about the type of service the caller supports
	memset(&hints, 0, sizeof hints);		
	//makes it a server, resolve address information for use on the passive side of connections
	hints.ai_flags = RAI_PASSIVE;			
	hints.ai_port_space = RDMA_PS_TCP;		
	
	/*
	 *@Description: provides transport independent address translation. It resolves the destination
	 *node and service address and returns information required to establish device communication
	 *@param: hints -> reference to an rdma_addrinfo structure containing hints about the type of 
	 *service 
	 *@param: NULL -> because it is server side
	 *@return: An rdma_addrinfo structure which returns information needed to establish communication
	 */
	ret = rdma_getaddrinfo(NULL, port, &hints, &res);	
	if (ret) {
		perror("rdma_getaddrinfo");
		return ret;
	}

	memset(&init_attr, 0, sizeof init_attr);
	init_attr.cap.max_send_wr = init_attr.cap.max_recv_wr = 1;	//max number of outstanding send requests in the send queue
	init_attr.cap.max_send_sge = init_attr.cap.max_recv_sge = 1;	//max number of scatter/gather elements in a WR on the send queue
	init_attr.cap.max_inline_data = 16;		//Maximum size in bytes of inline data on the send queue.
	init_attr.sq_sig_all = 1;		//value is a, all send requests(WR) will generate completion queue events.
	/*
	 *@Description: rdma_create_ep creates an identifier and option QP used to track commutication information
	 *@param: listen_id: commutication identifier, res: Address information associated with the rdma_cm_id
	 */
	ret = rdma_create_ep(&listen_id, res, NULL, &init_attr);	
	if (ret) {
		perror("rdma_create_ep");
		goto out_free_addrinfo;
	}

	ret = rdma_listen(listen_id, 0);		//server Listen, like listenfd
	if (ret) {
		perror("rdma_listen");
		goto out_destroy_listen_ep;
	}

	ret = rdma_get_request(listen_id, &id);		//id: rdma_cm_id associated with the new connection, like connfd
	if (ret) {
		perror("rdma_get_request");
		goto out_destroy_listen_ep;
	}

	memset(&qp_attr, 0, sizeof qp_attr);
	memset(&init_attr, 0, sizeof init_attr);
	ret = ibv_query_qp(id->qp, &qp_attr, IBV_QP_CAP,
			   &init_attr);		//@output:qp_attr, init_attr
	if (ret) {
		perror("ibv_query_qp");
		goto out_destroy_accept_ep;
	}
	if (init_attr.cap.max_inline_data >= 16)
		send_flags = IBV_SEND_INLINE;
	else
		printf("rdma_server: device doesn't support IBV_SEND_INLINE, "
		       "using sge sends\n");
	
	//register an array of memory buffers for sending or receiving messages or for RDMA options
	mr = rdma_reg_msgs(id, recv_msg, 16);		
	if (!mr) {
		ret = -1;
		perror("rdma_reg_msgs for recv_msg");
		goto out_destroy_accept_ep;
	}
	if ((send_flags & IBV_SEND_INLINE) == 0) {			//send
		send_mr = rdma_reg_msgs(id, send_msg, 16);		//send
		if (!send_mr) {
			ret = -1;
			perror("rdma_reg_msgs for send_msg");
			goto out_dereg_recv;
		}
	}
	//registed-->post, The posted buffers will be queued to receive an incoming message sent by the remote peer.
	ret = rdma_post_recv(id, NULL, recv_msg, 16, mr);	
	if (ret) {
		perror("rdma_post_recv");
		goto out_dereg_send;
	}
	
	//called from the listening side to accept a connection or datagram service lookup request.
	ret = rdma_accept(id, NULL);	
	if (ret) {
		perror("rdma_accept");
		goto out_dereg_send;
	}

	while ((ret = rdma_get_recv_comp(id, &wc)) == 0);
	if (ret < 0) {
		perror("rdma_get_recv_comp");
		goto out_disconnect;
	}

	ret = rdma_post_send(id, NULL, send_msg, 16, send_mr, send_flags);
	if (ret) {
		perror("rdma_post_send");
		goto out_disconnect;
	}

	while ((ret = rdma_get_send_comp(id, &wc)) == 0);
	if (ret < 0)
		perror("rdma_get_send_comp");
	else
		ret = 0;

out_disconnect:
	rdma_disconnect(id);
out_dereg_send:
	if ((send_flags & IBV_SEND_INLINE) == 0)
		rdma_dereg_mr(send_mr);
out_dereg_recv:
	rdma_dereg_mr(mr);
out_destroy_accept_ep:
	rdma_destroy_ep(id);
out_destroy_listen_ep:
	rdma_destroy_ep(listen_id);
out_free_addrinfo:
	rdma_freeaddrinfo(res);
	return ret;
}

int main(int argc, char **argv)
{
	int op, ret;

	while ((op = getopt(argc, argv, "p:")) != -1) {
		switch (op) {
		case 'p':
			port = optarg;
			break;
		default:
			printf("usage: %s\n", argv[0]);
			printf("\t[-p port_number]\n");
			exit(1);
		}
	}

	printf("rdma_server: start\n");
	ret = run();
	printf("rdma_server: end %d\n", ret);
	return ret;
}
