#include <libnet.h>
#include <math.h>
#include <sys/time.h>
#include <signal.h>
#include <string.h>
#include <netinet/in.h>

#include "tcpreplay.h"
#include "cidr.h"
#include "cache.h"
#include "err.h"
#include "do_packets.h"
#include "timer.h"
#include "list.h"
#include "xX.h"

extern struct options options;
extern char *cachedata;
extern CIDR *cidrdata;
extern struct timeval begin, end;
extern unsigned long bytes_sent, failed, pkts_sent, cache_packets;
extern volatile int didsig;

extern int include_exclude_mode;
extern CIDR *xX_cidr;
extern LIST *xX_list;


#ifdef DEBUG
extern int debug;
#endif


void packet_stats();



/*
 * we've got a race condition, this is our workaround
 */
void
catcher(int signo)
{
	/* stdio in signal handlers cause a race, instead we set a flag */
	if (signo == SIGINT)
		didsig = 1;
}


/*
 * the main loop function.  This is where we figure out
 * what to do with each packet
 */

void
do_packets(int fd, int (*get_next)(int, struct packet *))
{
	struct libnet_ethernet_hdr *eth_hdr = NULL;
#if USE_LIBNET_VERSION == 10
	struct libnet_link_int *l = NULL;
#elif USE_LIBNET_VERSION == 11
	libnet_t *l = NULL;
#endif
	u_char packetbuff[MAXPACKET];
	ip_hdr_t *ip_hdr;
	struct packet pkt;
	struct timeval last;
	int ret;
	unsigned long packetnum = 0;


	/* 
	 * point the ip_hdr to the temp packet buff
	 * This holds layer 3 and up!  We need to do this so we can 
	 * correctly calculate checksums @ layer 4
	 */
	ip_hdr = &packetbuff;
	
	/* register signals */
	didsig = 0;
	(void)signal(SIGINT, catcher);

	timerclear(&last);

	while ( (*get_next) (fd, &pkt) ) {
		if (didsig) {
			packet_stats();
			_exit(1);
		}

		packetnum ++;

		/* look for include or exclude LIST match */
		if (xX_list != NULL) {
			if (include_exclude_mode < xXExclude) {
				if (!check_list(xX_list, (packetnum))) {
					continue;
				}
			} else if (check_list(xX_list, (packetnum))) {
				continue;
			}
		}
			

		eth_hdr = (struct libnet_ethernet_hdr *)(pkt.data);
		memset(&packetbuff, '\0', sizeof(packetbuff));

		/* does packet have an IP header?  if so set our pointer to it */
		if (ntohs(eth_hdr->ether_type) == ETHERTYPE_IP) {
			/* 
			 * copy layer 3 and up to our temp packet buffer
			 * for now on, we have to edit the packetbuff because
			 * just before we send the packet, we copy the packetbuff 
			 * back onto the pkt.data + LIBNET_ETH_H buffer
			 * we do all this work to prevent byte alignment issues
			 */
			memcpy(ip_hdr, (pkt.data + LIBNET_ETH_H), pkt.len - LIBNET_ETH_H);
			
			/* look for include or exclude CIDR match */
			if (xX_cidr != NULL) {
				if (! process_xX_by_cidr(include_exclude_mode, xX_cidr, ip_hdr)) {
					continue;
				}
			}

		}

		/* check for martians? */
		if (options.no_martians && (ip_hdr->ip_hl != 0)) {
			switch ((ntohl(ip_hdr->ip_dst.s_addr) & 0xff000000) >> 24) {
			case 0: case 127: case 255:
#ifdef DEBUG
				if (debug) {
					warnx("Skipping martian.  Packet #%d", pkts_sent);
				}
#endif

				/* then skip the packet */
				continue;

			default:
				/* continue processing */
				break;
			}
		}

 
		/* Dual nic processing */
		if (options.intf2 != NULL) {

			if (cachedata != NULL) { 
				l = (LIBNET *)cache_mode(cachedata, packetnum, eth_hdr);
			} else if (options.cidr) { 
				l = (LIBNET *)cidr_mode(eth_hdr, ip_hdr);
			} else {
				errx(1, "Strange, we should of never of gotten here");
			}
		} else {
			/* normal single nic operation */
			l = options.intf1;
			/* check for destination MAC rewriting */
			if (memcmp(options.intf1_mac, NULL_MAC, 6) != 0) {
				memcpy(eth_hdr->ether_dhost, options.intf1_mac, ETHER_ADDR_LEN);
			}
		}

		/* sometimes we should not send the packet */
		if (l == CACHE_NOSEND)
			continue;

		/* Untruncate packet? Only for IP packets */
		if (options.trunc) {
#if USE_LIBNET_VERSION == 10
			untrunc_packet(&pkt, ip_hdr, NULL);
#elif USE_LIBNET_VERSION == 11
		    untrunc_packet(&pkt, ip_hdr, (void *)l);
#endif
		}


		/* do we need to spoof the src/dst IP address? */
		if (options.seed && ip_hdr->ip_hl != 0) {
#if USE_LIBNET_VERSION == 10
			randomize_ips(&pkt, ip_hdr, NULL);
#elif USE_LIBNET_VERSION == 11
			randomize_ips(&pkt, ip_hdr, (void *)l);
#endif
		}
	

		/* 
		 * put back the layer 3 and above back in the pkt.data buffer 
		 * we can't edit the packet at layer 3 or above beyond this point
		 */
		memcpy((pkt.data + LIBNET_ETH_H), ip_hdr, pkt.len - LIBNET_ETH_H);

		if (!options.topspeed)
			do_sleep(&pkt.ts, &last, pkt.len);

		/* Physically send the packet */
		do {
#if USE_LIBNET_VERSION == 10
			ret = libnet_write_link_layer(l, l->device, (u_char *)pkt.data, pkt.len);
#elif USE_LIBNET_VERSION == 11
			ret = libnet_adv_write_link(l, (u_char*)pkt.data, pkt.len);
#endif
			if (ret == -1) {
				/* Make note of failed writes due to full buffers */
				if (errno == ENOBUFS) {
					failed++;
				} else {
#if USE_LIBNET_VERSION == 10
					err(1, "libnet_write_link_layer(): %s", strerror(errno));
#elif USE_LIBNET_VERSION == 11
					err(1, "libnet_adv_write_link(): %s", strerror(errno));
#endif
				}
			}
		} while (ret == -1);

		bytes_sent += pkt.len;
		pkts_sent++;

		last = pkt.ts;
	}
}

/*
 * randomizes the source and destination IP addresses based on a 
 * pseudo-random number which is generated via the seed.
 */
void randomize_ips(struct packet *pkt, ip_hdr_t *ip_hdr, void *l)
{
	/* randomize IP addresses based on the value of random */
#ifdef DEBUG
	dbg(1, "Old Src IP: 0x%08lx\tOld Dst IP: 0x%08lx", 
		ip_hdr->ip_src.s_addr,
		ip_hdr->ip_dst.s_addr);
#endif

	ip_hdr->ip_dst.s_addr = 
		(ip_hdr->ip_dst.s_addr ^ options.seed) - 
		(ip_hdr->ip_dst.s_addr & options.seed);
	ip_hdr->ip_src.s_addr = 
		(ip_hdr->ip_src.s_addr ^ options.seed) -
		(ip_hdr->ip_src.s_addr & options.seed);
	
	
#ifdef DEBUG
	dbg(1, "New Src IP: 0x%08lx\tNew Dst IP: 0x%08lx\n",
		ip_hdr->ip_src.s_addr,
		ip_hdr->ip_dst.s_addr);
#endif

	/* recalc the UDP/TCP checksum(s) */
	if ((ip_hdr->ip_p == IPPROTO_UDP) || (ip_hdr->ip_p == IPPROTO_TCP)) {
#if USE_LIBNET_VERSION == 10
		if (libnet_do_checksum((u_char *)ip_hdr, ip_hdr->ip_p, 
							   pkt->len - LIBNET_ETH_H - LIBNET_IP_H) < 0)
			warnx("Layer 4 checksum failed");
#elif USE_LIBNET_VERSION == 11
		if (libnet_do_checksum((libnet_t *)l, (u_char *)ip_hdr, ip_hdr->ip_p,
							   pkt->len - LIBNET_ETH_H - LIBNET_IP_H) < 0)
			warnx("Layer 4 checksum failed");
#endif
	}

	/* recalc IP checksum */
#if USE_LIBNET_VERSION == 10
	if (libnet_do_checksum((u_char *)ip_hdr, IPPROTO_IP, LIBNET_IP_H) < 0)
		warnx("IP checksum failed");
#elif USE_LIBNET_VERSION == 11
	if (libnet_do_checksum((libnet_t *)l, (u_char *)ip_hdr, IPPROTO_IP,
						   pkt->len - LIBNET_ETH_H - LIBNET_IP_H) < 0)
		warnx("IP checksum failed");
#endif

}

/*
 * determines based upon the cachedata which interface the given packet 
 * should go out.  Also rewrites any layer 2 data we might need to adjust.
 * Returns a void cased pointer to the options.intfX of the corresponding 
 * interface.
 */

void * 
cache_mode(char *cachedata, int packet_num, struct libnet_ethernet_hdr *eth_hdr) 
{
	void * l = NULL;
	int result;

	if (packet_num > cache_packets)
		errx(1, "Exceeded number of packets in cache file");

	result = check_cache(cachedata, packet_num);
	if (result == CACHE_NOSEND) {
		return NULL;
	} else if (result == CACHE_PRIMARY) {
		l = options.intf1;
		
		/* check for destination MAC rewriting */
		if (memcmp(options.intf1_mac, NULL_MAC, 6) != 0) {
			memcpy(eth_hdr->ether_dhost, options.intf1_mac, ETHER_ADDR_LEN);
		}
	} else if (result == CACHE_SECONDARY) {
		l = options.intf2;

		/* check for destination MAC rewriting */
		if (memcmp(options.intf2_mac, NULL_MAC, 6) != 0) {
			memcpy(eth_hdr->ether_dhost, options.intf2_mac, ETHER_ADDR_LEN);
		}
	} else {
		errx(1, "check_cache() returned an error.  Aborting...");
	}

	return l;
}

/*
 * determines based upon the cidrdata which interface the given packet 
 * should go out.  Also rewrites any layer 2 data we might need to adjust.
 * Returns a void cased pointer to the options.intfX of the corresponding
 * interface.
 */

void * 
cidr_mode(struct libnet_ethernet_hdr *eth_hdr, ip_hdr_t *ip_hdr)
{
	void * l = NULL;

	if (ip_hdr == NULL) {
		/* non IP packets go out intf1 */
		l = options.intf1;
					
		/* check for destination MAC rewriting */
		if (memcmp(options.intf1_mac, NULL_MAC, 6) != 0) {
			memcpy(eth_hdr->ether_dhost, options.intf1_mac, ETHER_ADDR_LEN);
		}
	} else if (check_ip_CIDR(cidrdata, ip_hdr->ip_src.s_addr)) {
		/* set interface to send out packet */
		l = options.intf1;
		
		/* check for destination MAC rewriting */
		if (memcmp(options.intf1_mac, NULL_MAC, 6) != 0) {
			memcpy(eth_hdr->ether_dhost, options.intf1_mac, ETHER_ADDR_LEN);
		}
	} else {
		/* override interface to send out packet */
		l = options.intf2;
		
		/* check for destination MAC rewriting */
		if (memcmp(options.intf2_mac, NULL_MAC, 6) != 0) {
			memcpy(eth_hdr->ether_dhost, options.intf2_mac, ETHER_ADDR_LEN);
		}
	}

	return l;
}


/*
 * this code will untruncate a packet via padding it with null
 * or resetting the actual packet len to the snaplen.  In either case
 * it will recalcuate the IP and transport layer checksums
 *
 * Note that the *l parameter should be the libnet_t *l for libnet 1.1
 * or NULL for libnet 1.0
 */

void
untrunc_packet(struct packet *pkt, ip_hdr_t *ip_hdr, void *l)
{

	/* if actual len == cap len or there's no IP header, don't do anything */
	if ((pkt->len == pkt->actual_len) || (ip_hdr == NULL)) {
		return;
	}

	/* Pad packet or truncate it */
	if (options.trunc == PAD_PACKET) {
		memset(pkt->data + pkt->len, 0, sizeof(pkt->data) - pkt->len);
		pkt->len = pkt->actual_len;
	} else if (options.trunc == TRUNC_PACKET) {
		ip_hdr->ip_len = htons(pkt->len);
	} else {
		errx(1, "Hello!  I'm not supposed to be here!");
	}
	
	/* recalc the UDP/TCP checksum(s) */
	if ((ip_hdr->ip_p == IPPROTO_UDP) || (ip_hdr->ip_p == IPPROTO_TCP)) {
#if USE_LIBNET_VERSION == 10
		if (libnet_do_checksum((u_char *)ip_hdr, ip_hdr->ip_p, 
							   pkt->len - LIBNET_ETH_H - LIBNET_IP_H) < 0)	
		warnx("Layer 4 checksum failed");
#elif USE_LIBNET_VERSION == 11
		if (libnet_do_checksum((libnet_t *)l, (u_char *)ip_hdr, ip_hdr->ip_p,
							   pkt->len - LIBNET_ETH_H - LIBNET_IP_H) < 0)
			warnx("Layer 4 checksum failed");
#endif
	}

	
	/* recalc IP checksum */
#if USE_LIBNET_VERSION == 10
	if (libnet_do_checksum((u_char *)ip_hdr, IPPROTO_IP, LIBNET_IP_H) < 0)
		warnx("IP checksum failed");
#elif USE_LIBNET_VERSION == 11
	if (libnet_do_checksum((libnet_t *)l, (u_char *)ip_hdr, IPPROTO_IP,
						   pkt->len - LIBNET_ETH_H - LIBNET_IP_H) < 0)
		warnx("IP checksum failed");
#endif

}



/*
 * Given the timestamp on the current packet and the last packet sent,
 * calculate the appropriate amount of time to sleep and do so.
 */
void 
do_sleep(struct timeval *time, struct timeval *last, int len)
{
	static struct timeval didsleep;	
	static struct timeval start;	
	struct timeval nap, now, delta;
	float n;

	if (gettimeofday(&now, NULL) < 0)
		err(1, "gettimeofday");

	/* First time through for this file */
	if (!timerisset(last)) {
		start = now;
		timerclear(&delta);
		timerclear(&didsleep);
	} else {
		timersub(&now, &start, &delta);
	}

	if (options.mult) {
		/* 
		 * Replay packets a factor of the time they were originally sent.
		 */
		if (timerisset(last) && timercmp(time, last, >)) 
			timersub(time, last, &nap);
		else  
			/* 
			 * Don't sleep if this is our first packet, or if the
			 * this packet appears to have been sent before the 
			 * last packet.
			 */
			timerclear(&nap);

		timerdiv(&nap, options.mult);

	} else if (options.rate) {
		/* 
		 * Ignore the time supplied by the capture file and send data at
		 * a constant 'rate' (bytes per second).
		 */
		if (timerisset(last)) {
			n = (float)len / (float)options.rate;
			nap.tv_sec = n;
			nap.tv_usec = (n - nap.tv_sec) * 1000000;
		} else
			timerclear(&nap);
	}

	timeradd(&didsleep, &nap, &didsleep);

	if (timercmp(&didsleep, &delta, >)) {
		timersub(&didsleep, &delta, &nap);

		/* sleep & usleep only return EINTR & EINVAL, neither which we'd
	 	 * like to restart */
		if (nap.tv_sec)	 
			(void)sleep(nap.tv_sec);
		if (nap.tv_usec)	 
			(void)usleep(nap.tv_usec);
	}
}