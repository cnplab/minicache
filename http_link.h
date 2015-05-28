/*
 * thpHTTP - A tiny high performance HTTP server for Mini-OS
 *  This HTTP server is based on http_parser.
 *
 * Copyright(C) 2014-2015 NEC Laboratories Europe. All rights reserved.
 *                        Simon Kuenzer <simon.kuenzer@neclab.eu>
 */
#ifndef _HTTP_LINK_H_
#define _HTTP_LINK_H_

#include "http_defs.h"
#include "shfs_fio.h"

#define httpreq_link_nb_buffers(chunksize)  ((max((DIV_ROUND_UP(HTTPREQ_TCP_MAXSNDBUF, (size_t) chunksize)), 2)) << 1)

/* server states */
enum http_req_link_origin_sstate {
	HRLOS_ERROR = 0,
	HRLOS_RESOLVE,
	HRLOS_WAIT_RESOLVE,
	HRLOS_CONNECT,
	HRLOS_WAIT,
	HRLOS_WAIT_RESPONSE,
	HRLOS_CONNECTED,
	HRLOS_EOF /* end-of-file */
};

/* client states */
enum http_req_link_origin_cstate {
	HRLOC_ERROR,
	HRLOC_REQUEST,
	HRLOC_GETRESPONSE,
	HRLOC_CONNECTED,
};

struct http_req_link_origin {
	struct tcp_pcb *tpcb;
	ip_addr_t rip;
	uint16_t rport;
	uint16_t timeout;

	dlist_el(links);
	dlist_head(clients);
	uint32_t nb_clients;

	SHFS_FD fd;

	size_t sent;
	size_t sent_infly;
	enum http_req_link_origin_sstate sstate;
	enum http_req_link_origin_cstate cstate;
	struct shfs_cache_entry *cce[HTTPREQ_LINK_MAXNB_BUFFERS];

	struct http_parser parser;

	struct {
		char req[HTTP_HDR_DLINE_MAXLEN];
		struct http_send_hdr hdr;
		size_t hdr_total_len;
		size_t hdr_acked_len;
	} request;

	struct {
		struct http_recv_hdr hdr;
		const char *mime;
	} response;

	struct mempool_obj *pobj;
};

int   httplink_init   (struct http_srv *hs);
void  httplink_exit   (struct http_srv *hs);
err_t httplink_close  (struct http_req_link_origin *o, enum http_sess_close type);
err_t httplink_connected(void *argp, struct tcp_pcb * tpcb, err_t err);
err_t httplink_sent   (void *argp, struct tcp_pcb *tpcb, uint16_t len);
err_t httplink_recv   (void *argp, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
void  httplink_error  (void *argp, err_t err);
err_t httplink_poll   (void *argp, struct tcp_pcb *tpcb);

static inline void httplink_notify_clients(struct http_req_link_origin *o)
{
  struct http_req *hreq;
  struct http_req *hreq_next;

  hreq = dlist_first_el(o->clients, typeof(*hreq));
  while(hreq) {
    hreq_next = dlist_next_el(hreq, l.clients);
    printd("Notifying client %p (hsess %p)\n", hreq, hreq->hsess);
    httpsess_respond(hreq->hsess);
    hreq = hreq_next;
  }
}

static inline int httpreq_link_prepare_hdr(struct http_req *hreq)
{
	//struct http_srv *hs = hreq->hsess->hs;
	struct mempool_obj *pobj;
	struct http_req_link_origin *o;

	o = (struct http_req_link_origin *) shfs_fio_get_cookie(hreq->fd);
	if (o) {
		/* append this request to client list (join) */
		dlist_append(hreq, o->clients, l.clients);
		hreq->l.origin = o;
		++o->nb_clients;

		printd("origin found %p, request %p joined\n", o, hreq);
		return 0;
	}

	/* create a new upstream link */
	pobj = mempool_pick(hs->link_pool);
	if (!pobj)
		goto err_out;
	o = (struct http_req_link_origin *) pobj->data;
	o->pobj = pobj;

	o->fd = shfs_fio_clonef(hreq->fd);
	if (!o->fd)
		goto err_free_o;
	o->tpcb = tcp_new();
	if (!o->tpcb)
		goto err_close_fd;
	o->sstate = HRLOS_RESOLVE;
	o->cstate = HRLOC_ERROR;

	tcp_arg(o->tpcb, o);
	tcp_recv(o->tpcb, httplink_recv); /* recv callback */
	tcp_sent(o->tpcb, httplink_sent); /* sent ack callback */
	tcp_err (o->tpcb, httplink_error); /* err callback */
	tcp_poll(o->tpcb, httplink_poll, HTTP_POLL_INTERVAL); /* poll callback */
	tcp_setprio(o->tpcb, HTTP_LINK_TCP_PRIO);

	/* append origin to list of origins */
	dlist_init_el(o, links);
	dlist_append(o, hs->links, links);
	++hs->nb_links;

	/* append this request to client list */
	dlist_init_head(o->clients);
	dlist_append(hreq, o->clients, l.clients);
	hreq->l.origin = o;
	o->nb_clients = 1;

	/* init parser */
	o->parser.data = (void *) &o->response.hdr;
	http_parser_init(&o->parser, HTTP_RESPONSE);
	http_recvhdr_reset(&o->response.hdr);
	o->response.mime = NULL;

	http_sendhdr_reset(&o->request.hdr);
	o->sent = 0;
	o->sent_infly = 0;
	o->request.hdr_total_len = 0;
	o->request.hdr_acked_len = 0;

	/* add cookie to file descriptor (never fails) */
	shfs_fio_set_cookie(o->fd, o);

	printd("new origin %p with request %p created\n", o, hreq);
	return 0;

 err_close_fd:
	shfs_fio_close(o->fd);
 err_free_o:
	mempool_put(pobj);
 err_out:
	return -ENOMEM;
}

#if LWIP_DNS
void httpreq_link_dnscb(const char *name, ip_addr_t *ipaddr, void *argp);
#endif

static inline int httpreq_link_build_hdr(struct http_req *hreq)
{
	//struct http_srv *hs = hreq->hsess->hs;
	size_t nb_slines;
	size_t nb_dlines;
	struct http_req_link_origin *o = hreq->l.origin;
	err_t err;
	int ret;

	/* connection procedure */
	switch(o->sstate) {
	case HRLOS_RESOLVE:
		/* resolv remote host name */
		printd("Resolving origin host address...\n");
		o->rport = shfs_fio_link_rport(o->fd);
#if LWIP_DNS
		ret = shfshost2ipaddr(shfs_fio_link_rhost(o->fd), &o->rip, httpreq_link_dnscb, hreq);
		if (ret >= 1) {
			o->sstate = HRLOS_WAIT_RESOLVE;
			return -EAGAIN;
		}
#else
		ret = shfshost2ipaddr(shfs_fio_link_rhost(o->fd), &o->rip);
#endif
		if (ret < 0) {
			printd("Resolution of origin host address failed: %d\n", ret);
			goto err_out;
		}
		printd("Resolution could be done directly\n");
		o->sstate = HRLOS_CONNECT;
		goto case_HRLOS_CONNECT;

	case_HRLOS_CONNECT:
	case HRLOS_CONNECT:
		/* connect to remote */
		printd("Connecting to origin host...\n");
		o->timeout = HTTP_LINK_CONNECT_TIMEOUT;
		err = tcp_connect(o->tpcb, &o->rip, o->rport, httplink_connected);
		if (err != ERR_OK)
			goto err_out;
		o->sstate = HRLOS_WAIT;
		return -EAGAIN;

	case HRLOS_CONNECTED:
		/* create header for client */
		nb_slines = http_sendhdr_get_nbslines(&hreq->response.hdr);
		nb_dlines = http_sendhdr_get_nbdlines(&hreq->response.hdr);

		hreq->response.code = 200;	/* 200 OK */
		http_sendhdr_add_shdr(&hreq->response.hdr, &nb_slines,
				      HTTP_SHDR_200(hreq->request.http_major, hreq->request.http_minor));
		if (o->response.mime)
			http_sendhdr_add_dline(&hreq->response.hdr, &nb_dlines,
					       "%s: %s\r\n", _http_dhdr[HTTP_DHDR_MIME], o->response.mime);
		hreq->is_stream = 1;

		http_sendhdr_set_nbslines(&hreq->response.hdr, nb_slines);
		http_sendhdr_set_nbdlines(&hreq->response.hdr, nb_dlines);
		return 0;

	case HRLOS_ERROR:
	  goto err_out;
	default: /* wait states */
	  return -EAGAIN; /* stay in phase */
	}

	/* build response */
	return 0; /* next phase */

 err_out: /* will end up in err500_hdr */
	printd("Error happened on origin %p, exiting request %p\n", o, hreq);
	o->sstate = HRLOS_ERROR;
	return -1;
}

static inline void httpreq_link_close(struct http_req *hreq)
{
	//struct http_srv *hs = hreq->hsess->hs;
	struct http_req_link_origin *o = hreq->l.origin;

	--o->nb_clients;
	dlist_unlink(&hreq->l, o->clients, clients);
	printd("request %p removed from origin %p\n", hreq, o);
	if (o->nb_clients == 0) {
		shfs_fio_clear_cookie(o->fd);
		--hs->nb_links;
		dlist_unlink(o, hs->links, links);
		if (o->tpcb) /* close connection to origin if not done yet */
			httplink_close(o, HSC_CLOSE);
		shfs_fio_close(o->fd);
		mempool_put(o->pobj);
		printd("origin %p destroyed\n", o);
	}
}

#endif /* _HTTP_LINK_H_ */
