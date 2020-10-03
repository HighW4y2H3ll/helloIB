#include <stdio.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>


#define XCHG_PORT 0x6666

#define QUEUESZ 100000
#define BUFADDR 0x80080000
#define BUFSZ	4096

struct xchg_info {
	uint32_t	mrkey;
	uint32_t	qp_num;
	union ibv_gid	gid;
};

#ifdef BUILD_SERVER
#define REMOTE_NODE "node-1"
#define HOSTNAME "node-0"
#elif defined BUILD_CLIENT
#define REMOTE_NODE "node-0"
#define HOSTNAME "node-1"
#endif

int exchange_info(struct xchg_info *remote, struct xchg_info *local) {
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)	return -1;
#ifdef BUILD_SERVER
	struct hostent *rnode = gethostbyname(HOSTNAME);
#elif defined BUILD_CLIENT
	struct hostent *rnode = gethostbyname(REMOTE_NODE);
#endif
	if (!rnode)	return -1;
	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_addr = *(struct in_addr*)(rnode->h_addr),
		.sin_port = htons(XCHG_PORT),
	};
#ifdef BUILD_SERVER
	if (bind(sock, (const struct sockaddr *)&sa, sizeof(sa)) < 0)
		return -1;
	if (listen(sock, 0) < 0)
		return -1;
	int comm = accept(sock, NULL, NULL);
	if (comm < 0)	return -1;
	if (read(comm, remote, sizeof(struct xchg_info)) < 0)
		return -1;
	if (write(comm, local, sizeof(struct xchg_info)) < 0)
		return -1;
	close(comm);
#elif defined BUILD_CLIENT
	if (connect(sock, (const struct sockaddr *)&sa, sizeof(sa)) < 0)
		return -1;
	if (write(sock, local, sizeof(struct xchg_info)) < 0)
		return -1;
	if (read(sock, remote, sizeof(struct xchg_info)) < 0)
		return -1;
#endif
	close(sock);
	return 0;
}

int main() {
	int err = 0;
	int ndev;
	struct ibv_device **dev_list;
       	struct ibv_context *ctx = NULL;
	struct ibv_device_attr dev_attr;

	// Find the device
	dev_list = ibv_get_device_list(&ndev);
	if (!dev_list) {
		return -1;
	}

	for (int i = 0; i < ndev; i++) {
		//fprintf(stderr, "dev %s, %s\n", dev_list[i]->dev_path, dev_list[i]->ibdev_path);
		// There're several ways to find the right interface:
		// - rdma_resolve_addr()
		// - look through /sys/class/infiniband dir
		// - Here I choose the most stupid way
		if (!strncmp(dev_list[i]->name, "mlx5_3", IBV_SYSFS_NAME_MAX)) {
			ctx = ibv_open_device(dev_list[i]);
			break;
		}
	}
	if (!ctx) {
		err = -1;
		goto failed_ctx;
	}
	if (err = ibv_query_device(ctx, &dev_attr)) {
		err = -1;
		goto failed_pd;
	}

	// Prepare Pinned Memory for RDMA
	char *buf = (char*)mmap((void*)BUFADDR, BUFSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	if (buf == (char*)-1) {
		err = -1;
		fprintf(stderr, "Mmap failed\n");
		goto failed_pd;
	}
	struct ibv_pd *pd = ibv_alloc_pd(ctx);
	if (!pd) {
		err = -1;
		goto failed_pd;
	}

	struct ibv_mr *mr = ibv_reg_mr(pd, buf, BUFSZ,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
			);
	if (!mr) {
		err = -1;
		goto failed_mr;
	}
	fprintf(stderr, "MR Created: lkey %x, rkey %x\n", mr->lkey, mr->rkey);

	// Prepare Completion Queue, (Send/Recv) Queue Pair
	// - Create CQ: Doing Poll Mode only, so set `channel` to NULL (only used in Event Mode)
	struct ibv_cq *cq = ibv_create_cq(ctx, QUEUESZ, NULL, NULL, 0);
	if (!cq) {
		err = -1;
		fprintf(stderr, "create CQ failed\n");
		goto failed_cq;
	}
	// - Create QP: No Shared Recv Queue; No inline message
	struct ibv_qp_init_attr qp_attr = {0};
	qp_attr.qp_type = IBV_QPT_RC;	// For RDMA read: https://www.rdmamojo.com/2013/01/26/ibv_post_send/
	qp_attr.send_cq = cq;
	qp_attr.recv_cq = cq;
	qp_attr.cap.max_send_wr = dev_attr.max_qp_wr/ndev;	// num of outstanding send/recv Work Req (before entering SQ/RQ)
	qp_attr.cap.max_recv_wr = dev_attr.max_qp_wr/ndev;
	qp_attr.cap.max_send_sge = 1;	// 1 scatter/getter should be enough for each Work Req, or `dev_attr.max_sge`
	qp_attr.cap.max_recv_sge = 1;
	qp_attr.sq_sig_all = 0;	// Suppress Work Completion on successful posted on Send Queue
	struct ibv_qp *qp = ibv_create_qp(pd, &qp_attr);
	if (!qp) {
		err = -1;
		fprintf(stderr, "create QP failed\n");
		goto failed_qp;
	}
	fprintf(stderr, "QP Created: qp_num: %d, state: %d, qp_type: %d\n", qp->qp_num, qp->state, qp->qp_type);

	// Exchange Necessary Info
	union ibv_gid gid;
	if (err = ibv_query_gid(ctx, 1, 0, &gid)) {
		fprintf(stderr, "query gid error\n");
		goto failed_trans;
	}
	fprintf(stderr, "gid (a.k.a IPv6 addr)  %llx - %llx\n", gid.global.subnet_prefix, gid.global.interface_id);
	struct xchg_info remote_info;
	struct xchg_info local_info = {
		.mrkey = mr->lkey,
		.qp_num = qp->qp_num,
		.gid = gid,
	};
	if (err = exchange_info(&remote_info, &local_info)) {
		fprintf(stderr, "Data Exchange failed\n");
		goto failed_trans;
	}
	fprintf(stderr, "Remote - mrkey %x, qp_num %d, gid %llx-%llx\n", remote_info.mrkey, remote_info.qp_num, remote_info.gid.global.subnet_prefix, remote_info.gid.global.interface_id);

	// Move through the QP state machine: It's a delicate science - https://www.rdmamojo.com/2014/01/18/connecting-queue-pairs/
	// Thanks rdmamojo: https://www.rdmamojo.com/2013/01/12/ibv_modify_qp/
	struct ibv_port_attr port_attr;
	if (err = ibv_query_port(ctx, 1, &port_attr)) {
		fprintf(stderr, "query port error\n");
		goto failed_trans;
	}
	fprintf(stderr, "state %x - lid (differentiate cards in the same box) %x - sm_lid %x - mtu %x\n", port_attr.state, port_attr.lid, port_attr.sm_lid, port_attr.active_mtu);
	// - RESET -> INIT
	struct ibv_qp_attr attr = {
		.qp_state = IBV_QPS_INIT,
		.port_num = 1,	// always 1
		.pkey_index = 0,
		.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE,
       	};
	int qp_flag =
	       	IBV_QP_STATE |
		IBV_QP_PORT |
	       	IBV_QP_PKEY_INDEX |
		IBV_QP_ACCESS_FLAGS;
	if (err = ibv_modify_qp(qp, &attr, qp_flag)) {
		fprintf(stderr, "QP: failed to transition to INIT (%d)\n", err);
		goto failed_trans;
	}
	// - INIT -> RTR
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_2048;
	attr.dest_qp_num = remote_info.qp_num;
	attr.rq_psn = 0;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 0x12;
	attr.ah_attr.is_global = 1;
	attr.ah_attr.grh.dgid = remote_info.gid;
	attr.ah_attr.grh.sgid_index = 0;
	attr.ah_attr.grh.hop_limit = 1;
	attr.ah_attr.dlid = 0;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = 1;
	qp_flag = IBV_QP_STATE |
	       	IBV_QP_PATH_MTU |
	       	IBV_QP_DEST_QPN |
	       	IBV_QP_RQ_PSN |
	       	IBV_QP_MAX_DEST_RD_ATOMIC |
	       	IBV_QP_MIN_RNR_TIMER |
	       	IBV_QP_AV;
	if (err = ibv_modify_qp(qp, &attr, qp_flag)) {
		fprintf(stderr, "QP: failed to transition to RTR (%d)\n", err);
		goto failed_trans;
	}
#ifdef BUILD_CLIENT
	attr.qp_state = IBV_QPS_RTS;
	attr.sq_psn = 0;
	attr.timeout = 0x12;
	attr.retry_cnt = 6;
	attr.rnr_retry = 7;	// Infinite
	attr.max_rd_atomic = 1;
	qp_flag = IBV_QP_STATE |
		IBV_QP_SQ_PSN |
		IBV_QP_TIMEOUT |
	       	IBV_QP_RETRY_CNT |
		IBV_QP_RNR_RETRY |
		IBV_QP_MAX_QP_RD_ATOMIC;
	if (err = ibv_modify_qp(qp, &attr, qp_flag)) {
		fprintf(stderr, "QP: failed to transition to RTR (%d)\n", err);
		goto failed_trans;
	}
#endif

	// Start Communication
#ifdef BUILD_SERVER
	// - Step 1: Client RDMA Read
	strcpy((char*)BUFADDR, "SERVER");
	// Note that it sometimes also work without posting a recv req, but having one definately makes it more reliable
	struct ibv_sge sg = {
		.addr = BUFADDR,
		.length = 16,
		.lkey = mr->lkey,
	};
	struct ibv_recv_wr wr = {
		.sg_list = &sg,
		.num_sge = 1,
	};
	struct ibv_recv_wr *bad_wr;
	if (ibv_post_recv(qp, &wr, &bad_wr)) {
		err = -1;
		goto out;
	}

	// - Check Step 1 finished: https://www.rdmamojo.com/2013/02/02/ibv_post_recv/
	// Recv Completion is not guarnteed in RoCE mode for Mellanox Card:
	// - https://docs.mellanox.com/pages/viewpage.action?pageId=12004903#RDMAoverConvergedEthernet(RoCE)-SettingtheRoCEModeforaQueuePair(QP)
	// Will need IB switch or OpenSM to configure Mellanox Card to IB mode (instead ov RoCE)
	struct ibv_wc wc;
	int ncomp;
	//do {
	//	ncomp = ibv_poll_cq(cq, 1, &wc);
	//	sleep(1);
	//	fprintf(stderr, "QP Polling state: %x\n", qp->state);
	//} while (!ncomp);
	//fprintf(stderr, "WC: status %d\n", wc.status);

	// - Step 2: Client RDMA Write
	if (ibv_post_recv(qp, &wr, &bad_wr)) {
		err = -1;
		goto out;
	}
	// Still no recv WC, but yet need a loop here to avoid server exit
	do {
		ncomp = ibv_poll_cq(cq, 1, &wc);
		sleep(1);
		fprintf(stderr, "Remote Write: %s\n", (char*)BUFADDR);
	} while (!ncomp);
	fprintf(stderr, "WC: status %d\n", wc.status);
	fprintf(stderr, "Remote Write: %s\n", (char*)BUFADDR);

#elif defined BUILD_CLIENT
	// - Step 1: RDMA Read
	struct ibv_sge sg = {
		.addr = BUFADDR,
		.length = 16,
		.lkey = mr->lkey,
	};
	struct ibv_send_wr sr = {
		.sg_list = &sg,
		.num_sge = 1,
		.opcode = IBV_WR_RDMA_READ,
		.send_flags = IBV_SEND_SIGNALED,	// 0 to suppress signal in CQ
		.wr.rdma.remote_addr = BUFADDR,
		.wr.rdma.rkey = remote_info.mrkey,
	};
	struct ibv_send_wr *bad_wr;
	if (ibv_post_send(qp, &sr, &bad_wr)) {
		err = -1;
		fprintf(stderr, "Post Send Req failed\n");
		goto out;
	}
	// Polling
	struct ibv_wc wc;
	int ncomp;
	do {
		ncomp = ibv_poll_cq(cq, 1, &wc);
	} while (!ncomp);
	fprintf(stderr, "WC: status %d\n", wc.status);
	fprintf(stderr, "Remote Read: %s\n", (char*)BUFADDR);

	// - Step 2: RDMA WRITE
	strcpy((char*)BUFADDR, "client");
	sr.opcode = IBV_WR_RDMA_WRITE;
	if (ibv_post_send(qp, &sr, &bad_wr)) {
		err = -1;
		fprintf(stderr, "Post Send Req failed\n");
		goto out;
	}
	// Polling
	do {
		ncomp = ibv_poll_cq(cq, 1, &wc);
	} while (!ncomp);
	fprintf(stderr, "WC: status %d\n", wc.status);
#endif

out:
failed_trans:
	ibv_destroy_qp(qp);
failed_qp:
	ibv_destroy_cq(cq);
failed_cq:
	ibv_dereg_mr(mr);
failed_mr:
	ibv_dealloc_pd(pd);
failed_pd:
	ibv_close_device(ctx);
failed_ctx:
	ibv_free_device_list(dev_list);
	return err;
}
