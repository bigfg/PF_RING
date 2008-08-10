/*
 *
 * (C) 2005-08 - Luca Deri <deri@ntop.org>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef _PFRING_H_
#define _PFRING_H_

/* Test Only */
//#define USE_PCAP 


#include <sys/types.h>

#ifndef __USE_XOPEN2K
typedef volatile int pthread_spinlock_t;
extern int pthread_spin_init (pthread_spinlock_t *__lock, int __pshared) __THROW;

/* Destroy the spinlock LOCK.  */
extern int pthread_spin_destroy (pthread_spinlock_t *__lock) __THROW;

/* Wait until spinlock LOCK is retrieved.  */
extern int pthread_spin_lock (pthread_spinlock_t *__lock) __THROW;

/* Release spinlock LOCK.  */
extern int pthread_spin_unlock (pthread_spinlock_t *__lock) __THROW;
#endif


#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>

#ifndef HAVE_PCAP
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/sockios.h>
#endif

#ifdef USE_PCAP 
#include <pcap.h>
#endif

#include <arpa/inet.h>
#include <linux/ring.h>
#include <sys/ioctl.h>


#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <pthread.h>


#define PAGE_SIZE         4096

#define POLL_SLEEP_STEP         10 /* ns = 0.1 ms */
#define POLL_SLEEP_MIN        POLL_SLEEP_STEP
#define POLL_SLEEP_MAX        1000 /* ns */
#define POLL_QUEUE_MIN_LEN     500 /* # packets */

#ifndef max
#define max(a, b) (a > b ? a : b)
#endif

#ifdef  __cplusplus
extern "C" {
#endif

#ifdef SAFE_RING_MODE
  static char staticBucket[2048];
#endif

  /* ********************************* */

  typedef struct {
    char *buffer, *slots, *device_name;
    int  fd;
    FlowSlotInfo *slots_info;
    u_int page_id, slot_id, pkts_per_page;
    u_int poll_sleep;
    u_int8_t clear_promisc, reentrant;
    u_long num_poll_calls;
    pthread_spinlock_t spinlock;
  } pfring;

  typedef struct {
    u_int64_t recv, drop;
  } pfring_stat;

  /* NOTE: keep 'struct pfring_pkthdr' in sync with 'struct pcap_pkthdr' (ring.h) */

  /* ********************************* */

  int pfring_set_cluster(pfring *ring, u_int clusterId);
  int pfring_set_channel_id(pfring *ring, int32_t channel_id);
  int pfring_remove_from_cluster(pfring *ring);
  int pfring_set_reflector(pfring *ring, char *reflectorDevice);
  pfring* pfring_open(char *device_name, u_int8_t promisc, u_int32_t caplen, u_int8_t reentrant);
  void pfring_close(pfring *ring);
  int pfring_stats(pfring *ring, pfring_stat *stats);
  int pfring_recv(pfring *ring, char* buffer, u_int buffer_len, 
		  struct pfring_pkthdr *hdr, u_char wait_for_incoming_packet);
  int pfring_get_filtering_rule_stats(pfring *ring, u_int16_t rule_id,
				      char* stats, u_int *stats_len);
  int pfring_get_hash_filtering_rule_stats(pfring *ring,
					   hash_filtering_rule* rule,
					   char* stats, u_int *stats_len);
  int pfring_add_filtering_rule(pfring *ring, filtering_rule* rule_to_add);
  int pfring_handle_hash_filtering_rule(pfring *ring,
					hash_filtering_rule* rule_to_add,
					u_char add_rule);
  int pfring_enable_ring(pfring *ring);
  int pfring_remove_filtering_rule(pfring *ring, u_int16_t rule_id);
  int pfring_toggle_filtering_policy(pfring *ring, u_int8_t rules_default_accept_policy);
  int pfring_version(pfring *ring, u_int32_t *version);
  int pfring_set_sampling_rate(pfring *ring, u_int32_t rate /* 1 = no sampling */);
  int pfring_get_selectable_fd(pfring *ring);
#ifdef  __cplusplus
}
#endif

#endif /* _PFRING_H_ */