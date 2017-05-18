#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <infiniband/verbs.h>

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s receive packets from remote\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -d, --dev-name=<dev>   use  device <dev>)\n");
	printf("  -i, --dev_port=<port>  use port <port> of device (default 1)\n");
}


int main(int argc, char *argv[]) {
	char *devname = NULL;
	int   dev_port = 1;
	int   num_devices;


	static struct option long_options[] = {
		{ .name = "dev-name",  .has_arg = 1, .val = 'd' },
		{ .name = "dev-port",  .has_arg = 1, .val = 'i' },
	};


	while (1) {
		int c;

		c = getopt_long(argc, argv, "p:d:i:g:q:l:",
				long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'd':
			devname = strdup(optarg);
			break;

		case 'i':
			dev_port = strtol(optarg, NULL, 0);
			if (dev_port < 0) {
				usage(argv[0]);
				return 1;
			}
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	/*
	 *@description: get the list of available RDMA devices
	 *@return: array of RDMA devices currently available
	 *@param: num_devices -> the number of devices returned in the array
	 */
	struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
	if (!dev_list) {
		perror("Failed to get RDMA devices list");
		return 1;
	}

	int i;
	for (i = 0; i < num_devices; ++i)
		if (!strcmp(ibv_get_device_name(dev_list[i]), devname))
			break;

	if (i == num_devices) {
		fprintf(stderr, "RDMA device %s not found\n", devname);
		goto  free_dev_list;
	}
	
	//get the structure ibv_device of current RDMA device
	struct ibv_device *device  = dev_list[i];
	
	/*
	 *@description: open an RDMA device context
	 *@return: a pointer to the allocated device context
	 */
	struct ibv_context *context = ibv_open_device(device);
	if (!context) {
		fprintf(stderr, "Couldn't get context for %s\n",
				ibv_get_device_name(device));
		goto free_dev_list;
	}
	/*
	 *@description: allocate a protection domain, Both memory region and \
	 *queue pair have associated with pd
	 *@return: a pointer to the allocated pd
	 */
	struct ibv_pd *pd = ibv_alloc_pd(context);
	if (!pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto close_device;
	}

#define REGION_SIZE 0x1800
	char mr_buffer[REGION_SIZE];
	
	/*
	 *@description: register a memory region associated with the pd
	 *@return: a pointer to the register MR\n
	 */
	struct ibv_mr *mr = ibv_reg_mr(pd, mr_buffer, REGION_SIZE,
			IBV_ACCESS_LOCAL_WRITE);		//IBV_ACCESS_LOCAL_WRITE
	if (!mr) {
		fprintf(stderr, "Couldn't register MR\n");
		goto close_pd;
	}

#define CQ_SIZE 0x100
	
	/*
	 *@description: create a Completion Queue
	 *@return: a pointer to the CQ
	 */
	struct ibv_cq *cq = ibv_create_cq(context, CQ_SIZE, NULL,
			NULL, 0);
	if (!cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto free_mr;
	}

#define MAX_NUM_RECVS 0x10
#define MAX_GATHER_ENTRIES 2
#define MAX_SCATTER_ENTRIES 2
	
	//struct ibv_qp_init_attr: used for init QP attributes 
	struct ibv_qp_init_attr attr = {
		.send_cq = cq,			//set send cq
		.recv_cq = cq,			//set receive cq
		.cap     = {
			.max_send_wr  = 0,
			.max_recv_wr  = MAX_NUM_RECVS,
			.max_send_sge = MAX_GATHER_ENTRIES,	//Maximum number of scatter/gather elements in a WR on the send queue
			.max_recv_sge = MAX_SCATTER_ENTRIES,//Maximum number of SGEs in a WR on the receive queue
		},
		.qp_type = IBV_QPT_UD,	//Datagram
	};

	/*
	 *@description: create a Queue Pair associated with the pd
	 *@return: a pointer to the QP
	 *@param: attr -> some attributes set with the created QP
	 */
	struct ibv_qp *qp = ibv_create_qp(pd, &attr);
	if (!qp) {
		fprintf(stderr, "Couldn't create QP\n");
		goto free_cq;
	}
	
	//struct ibv_qp_attr: used for modify QP attributes
	struct ibv_qp_attr qp_modify_attr;

#define WELL_KNOWN_QKEY 0x11111111
	
	/*
	 * When a QP is newly created, it is in the RESET state. The first state
	 * that needs to happen is to bring the QP in the INIT state.
	 * Once the QP is transistioned into the INIT state, the user may begin to
	 * post receive buffers to the receive queue via the ibv_post_recv. At least
	 * one receive buffer should be posted before the QP ca be transistioned to the RTR state.
	 */
	qp_modify_attr.qp_state        = IBV_QPS_INIT;
	qp_modify_attr.pkey_index      = 0;
	qp_modify_attr.port_num        = dev_port;
	qp_modify_attr.qkey            = WELL_KNOWN_QKEY;
	if (ibv_modify_qp(qp, &qp_modify_attr,
				IBV_QP_STATE              |
				IBV_QP_PKEY_INDEX         |
				IBV_QP_PORT               |
				IBV_QP_QKEY)) {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		goto free_qp;
	}

	/*
	 * Once the QP is transistioned into the RTR state, the QP begins receive processing.
	 */
	qp_modify_attr.qp_state		= IBV_QPS_RTR;		//ready to receive

	if (ibv_modify_qp(qp, &qp_modify_attr, IBV_QP_STATE)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		goto free_qp;
	}

	/*
	 struct ibv_recv_wr{
		uint64_t            wr_if;		//work request ID
		struct ibv_recv_wr  *next;		//pointer to next WR
		struct ibv_sge      *sg_list;	//scatter array for this WR
		int                 num_sge;	//number of entries in sg_list
	 };
	 */
	struct ibv_recv_wr wr;		
	struct ibv_recv_wr *bad_wr;
	struct ibv_sge list;
	struct ibv_wc wc;
	int ne;

	fprintf(stderr, "Listinig on QP Number 0x%06x\n", qp->qp_num);
	sleep(1);

#define MAX_MSG_SIZE 0x100
	
	/*
	 *struct ibv_sge{
		uint64_t   addr;		//address of buffer
		uint32_t   length;		//length of buffer
		uint32_t   lkey;		//local key of buffer from ibv_reg_mr
	 };
	 */
	while( 1 ) {
	for (i = 0; i < 4; i++) {
		list.addr   = (uint64_t)(mr_buffer + MAX_MSG_SIZE*i);		//address of buffer
		list.length = MAX_MSG_SIZE;
		list.lkey   = mr->lkey;


		wr.wr_id      = i;
		wr.sg_list    = &list;
		wr.num_sge    = 1;
		wr.next       = NULL;
		
		/*
		 *@description: ibv_post_recv posts a linked list of WRs to a QP's receive queue.
		 *At least one receive buffer should be posted to the receive queue to transistion
		 *to the QP to RTR. Receive buffers are consumed as the remote peer excutes Send,
		 *Send with Immediate and RDMA Write with Immediate operations. Receive buffers are 
		 *NOT used for other RDMA operations. Processing of the WR list is stopped on the first
		 *error and a pointer to the offending WR is returned in bad_wr
		 */
		if (ibv_post_recv(qp,&wr,&bad_wr)) {
			fprintf(stderr, "Function ibv_post_recv failed\n");
			return 1;
		}
	}

	i = 0;
	while (i < 4) { 
		/*
		 *@description: ibv_poll_cq retrieves CQEs from a CQ. The user should allocate an array of
		 *struct ibv_wc and pass it to the call in wc.The number of entries available in wc should be
		 *passed in num_entries. It is the user’s responsibility to free this memory.
		 */
		do { ne = ibv_poll_cq(cq, 1,&wc);}  while (ne == 0);
		if (ne < 0) {
			fprintf(stderr, "CQ is in error state");
			return 1;
		}

		if (wc.status) {
			fprintf(stderr, "Bad completion (status %d)\n",(int)wc.status);
			return 1;
		} else {
			printf("received: %s\n", mr_buffer + MAX_MSG_SIZE*i +
					40);
		}

		i++;
	}
	printf("Press enter to respost\n");
	getchar();
	}

free_qp:
	ibv_destroy_qp(qp);

free_cq:
	ibv_destroy_cq(cq);

free_mr:
	ibv_dereg_mr(mr);

close_pd:
	ibv_dealloc_pd(pd);

close_device:
	ibv_close_device(context);

free_dev_list:
	ibv_free_device_list(dev_list);

	return 0;
}





