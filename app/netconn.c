/*
 * netconn.c: bytes sent and received per TCP connection, from the kernel's sock_diag
 * netlink interface (the same one "ss -ti" uses). See netconn.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#include <linux/tcp.h>
#include "netconn.h"

#define TCP_STATE_LISTEN 10

typedef struct { guint64 sent, recvd; gboolean seen; } Prev;

static GHashTable* prev = NULL;        /* socket inode -> counters at the previous scan */
static gint64 prevUs = 0;

static const struct { int port; const char* name; } SERVICES[] = {
	{ 554, "Video (RTSP)" }, { 80, "Web (HTTP)" }, { 443, "Web (HTTPS)" }, { 8080, "Web (HTTP 8080)" },
	{ 8443, "Web (HTTPS 8443)" }, { 22, "SSH" }, { 21, "FTP" }, { 1883, "MQTT" }, { 8883, "MQTT (TLS)" },
	{ 1935, "RTMP" }, { 5060, "SIP" }, { 5061, "SIP (TLS)" }, { 25, "Email" }, { 465, "Email" },
	{ 587, "Email" }, { 445, "File share (SMB)" }, { 2049, "File share (NFS)" }, { 853, "DNS (TLS)" },
	{ 0, NULL }
};
static const char* service_of(int port) {
	for (int i = 0; SERVICES[i].name; i++) if (SERVICES[i].port == port) return SERVICES[i].name;
	return NULL;
}

static gboolean not_seen(gpointer k, gpointer v, gpointer u) { (void)k; (void)u; return !((Prev*)v)->seen; }
static void unsee(gpointer k, gpointer v, gpointer u) { (void)k; (void)u; ((Prev*)v)->seen = FALSE; }

static int by_use(const void* a, const void* b) {
	const NetUse* x = a; const NetUse* y = b;
	double p = x->tx + x->rx, q = y->tx + y->rx;
	return p < q ? 1 : p > q ? -1 : 0;
}

/* One address family. Returns FALSE if the kernel refused. */
static gboolean dump(int fd, int family, GHashTable* groups, GArray* out, double dtSec) {
	struct { struct nlmsghdr nlh; struct inet_diag_req_v2 r; } req;
	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len = sizeof(req);
	req.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
	req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.r.sdiag_family = family;
	req.r.sdiag_protocol = IPPROTO_TCP;
	req.r.idiag_states = 0xFFF & ~(1u << TCP_STATE_LISTEN);
	req.r.idiag_ext = 1 << (INET_DIAG_INFO - 1);
	struct sockaddr_nl sa;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	if (sendto(fd, &req, sizeof(req), 0, (struct sockaddr*)&sa, sizeof(sa)) < 0) return FALSE;

	static char buf[32768];
	for (;;) {
		ssize_t len = recv(fd, buf, sizeof(buf), 0);
		if (len <= 0) return FALSE;
		for (struct nlmsghdr* h = (struct nlmsghdr*)buf; NLMSG_OK(h, (size_t)len); h = NLMSG_NEXT(h, len)) {
			if (h->nlmsg_type == NLMSG_DONE) return TRUE;
			if (h->nlmsg_type == NLMSG_ERROR) return FALSE;
			struct inet_diag_msg* m = NLMSG_DATA(h);
			if (!m->idiag_inode) continue;                       /* closing sockets have no counters */
			char peer[INET6_ADDRSTRLEN] = "";
			if (m->idiag_family == AF_INET) {
				inet_ntop(AF_INET, m->id.idiag_dst, peer, sizeof(peer));
			} else {
				const unsigned char* a = (const unsigned char*)m->id.idiag_dst;
				static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
				if (memcmp(a, mapped, 12) == 0) inet_ntop(AF_INET, a + 12, peer, sizeof(peer));
				else inet_ntop(AF_INET6, a, peer, sizeof(peer));
			}
			/* Traffic inside the camera never reaches the Ethernet port */
			if (g_str_has_prefix(peer, "127.") || strcmp(peer, "::1") == 0) continue;
			guint64 sent = 0, recvd = 0;
			gboolean got = FALSE;
			struct rtattr* a = (struct rtattr*)(m + 1);
			int alen = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*m)));
			for (; RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
				if (a->rta_type != INET_DIAG_INFO) continue;
				const struct tcp_info* ti = RTA_DATA(a);
				if (RTA_PAYLOAD(a) >= offsetof(struct tcp_info, tcpi_bytes_received) + sizeof(ti->tcpi_bytes_received)) {
					sent = ti->tcpi_bytes_acked; recvd = ti->tcpi_bytes_received; got = TRUE;
				}
			}
			if (!got) continue;
			Prev* p = g_hash_table_lookup(prev, GUINT_TO_POINTER(m->idiag_inode));
			double tx = 0, rx = 0;
			if (p && dtSec > 0) {
				if (sent >= p->sent) tx = 8.0 * (double)(sent - p->sent) / dtSec / 1e6;
				if (recvd >= p->recvd) rx = 8.0 * (double)(recvd - p->recvd) / dtSec / 1e6;
			}
			if (!p) { p = g_new0(Prev, 1); g_hash_table_insert(prev, GUINT_TO_POINTER(m->idiag_inode), p); }
			p->sent = sent; p->recvd = recvd; p->seen = TRUE;

			/* Name it by the camera's service (someone connected to it) or by where the camera connects to */
			int lport = ntohs(m->id.idiag_sport), rport = ntohs(m->id.idiag_dport);
			char service[32];
			const char* s = service_of(lport);
			if (s) g_strlcpy(service, s, sizeof(service));
			else if ((s = service_of(rport))) snprintf(service, sizeof(service), "To %s", s);
			else snprintf(service, sizeof(service), "Port %d", lport < rport ? lport : rport);
			gchar* key = g_strdup_printf("%s|%s", service, peer);
			gpointer idx;
			if (g_hash_table_lookup_extended(groups, key, NULL, &idx)) {
				NetUse* u = &g_array_index(out, NetUse, GPOINTER_TO_UINT(idx));
				u->tx += tx; u->rx += rx; u->conns++;
				g_free(key);
			} else {
				NetUse u;
				memset(&u, 0, sizeof(u));
				g_strlcpy(u.service, service, sizeof(u.service));
				g_strlcpy(u.peer, peer, sizeof(u.peer));
				u.tx = tx; u.rx = rx; u.conns = 1;
				g_array_append_val(out, u);
				g_hash_table_insert(groups, key, GUINT_TO_POINTER(out->len - 1));
			}
		}
	}
}

GArray* netconn_scan(void) {
	if (!prev) prev = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
	if (fd < 0) return NULL;
	gint64 now = g_get_monotonic_time();
	double dt = prevUs ? (double)(now - prevUs) / 1e6 : 0;
	prevUs = now;
	g_hash_table_foreach(prev, unsee, NULL);
	GHashTable* groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GArray* out = g_array_new(FALSE, TRUE, sizeof(NetUse));
	gboolean ok4 = dump(fd, AF_INET, groups, out, dt);
	gboolean ok6 = dump(fd, AF_INET6, groups, out, dt);
	close(fd);
	g_hash_table_destroy(groups);
	g_hash_table_foreach_remove(prev, not_seen, NULL);
	if (!ok4 && !ok6) { g_array_free(out, TRUE); return NULL; }
	g_array_sort(out, by_use);
	return out;
}
