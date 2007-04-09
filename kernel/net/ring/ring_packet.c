/* ***************************************************************
 *
 * (C) 2004-07 - Luca Deri <deri@ntop.org>
 *
 * This code includes contributions courtesy of
 * - Jeff Randall <jrandall@nexvu.com>
 * - Helmut Manck <helmut.manck@secunet.com>
 * - Brad Doctor <brad@stillsecure.com>
 * - Amit D. Chaudhary <amit_ml@rajgad.com>
 * - Francesco Fusco <fusco@ntop.org>
 * - Michael Stiller <ms@2scale.net>
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
 *
 */

#include <linux/version.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19))
#include <linux/autoconf.h>
#else
#include <linux/config.h>
#endif
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/in6.h>
#include <linux/init.h>
#include <linux/filter.h>
#include <linux/ring.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/list.h>
#include <linux/proc_fs.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
#include <net/xfrm.h>
#else
#include <linux/poll.h>
#endif
#include <net/sock.h>
#include <asm/io.h>   /* needed for virt_to_phys() */
#ifdef CONFIG_INET
#include <net/inet_common.h>
#endif

/* #define RING_DEBUG */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,11))
static inline int remap_page_range(struct vm_area_struct *vma,
				   unsigned long uvaddr,
				   unsigned long paddr,
				   unsigned long size,
				   pgprot_t prot) {
  return(remap_pfn_range(vma, uvaddr, paddr >> PAGE_SHIFT,
			 size, prot));
}
#endif

/* ************************************************* */

#define CLUSTER_LEN       8

struct ring_cluster {
  u_short             cluster_id; /* 0 = no cluster */
  u_short             num_cluster_elements;
  enum cluster_type   hashing_mode;
  u_short             hashing_id;
  struct sock         *sk[CLUSTER_LEN];
  struct ring_cluster *next;      /* NULL = last element of the cluster */
};

/* ************************************************* */

struct ring_element {
  struct list_head  list;
  struct sock      *sk;
};

/* ************************************************* */

struct ring_opt {
  struct net_device *ring_netdev;

  u_short ring_pid;

  /* Cluster */
  u_short cluster_id; /* 0 = no cluster */

  /* Reflector */
  struct net_device *reflector_dev;

  /* Packet buffers */
  unsigned long order;

  /* Ring Slots */
  unsigned long ring_memory;
  FlowSlotInfo *slots_info; /* Basically it points to ring_memory */
  char *ring_slots;  /* Basically it points to ring_memory
			+sizeof(FlowSlotInfo) */

  /* Packet Sampling */
  u_int pktToSample, sample_rate;

  /* BPF Filter */
  struct sk_filter *bpfFilter;

  /* Aho-Corasick */
  ACSM_STRUCT2 * acsm;

  /* Locks */
  atomic_t num_ring_slots_waiters;
  wait_queue_head_t ring_slots_waitqueue;
  rwlock_t ring_index_lock;

  /* Bloom Filters */
  u_char bitmask_enabled;
  bitmask_selector mac_bitmask, vlan_bitmask, ip_bitmask, twin_ip_bitmask,
    port_bitmask, twin_port_bitmask, proto_bitmask;
  u_int32_t num_mac_bitmask_add, num_mac_bitmask_remove;
  u_int32_t num_vlan_bitmask_add, num_vlan_bitmask_remove;
  u_int32_t num_ip_bitmask_add, num_ip_bitmask_remove;
  u_int32_t num_port_bitmask_add, num_port_bitmask_remove;
  u_int32_t num_proto_bitmask_add, num_proto_bitmask_remove;

  /* Indexes (Internal) */
  u_int insert_page_id, insert_slot_id;
};

/* ************************************************* */

/* List of all ring sockets. */
static struct list_head ring_table;
static u_int ring_table_size;

/* List of all clusters */
static struct ring_cluster *ring_cluster_list;

static rwlock_t ring_mgmt_lock = RW_LOCK_UNLOCKED;

/* ********************************** */

/* /proc entry for ring module */
struct proc_dir_entry *ring_proc_dir = NULL;
struct proc_dir_entry *ring_proc = NULL;

static int ring_proc_get_info(char *, char **, off_t, int, int *, void *);
static void ring_proc_add(struct ring_opt *pfr);
static void ring_proc_remove(struct ring_opt *pfr);
static void ring_proc_init(void);
static void ring_proc_term(void);

/* ********************************** */

/* Forward */
static struct proto_ops ring_ops;

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,11))
static struct proto ring_proto;
#endif

static int skb_ring_handler(struct sk_buff *skb, u_char recv_packet,
			    u_char real_skb);
static int buffer_ring_handler(struct net_device *dev, char *data, int len);
static int remove_from_cluster(struct sock *sock, struct ring_opt *pfr);

/* Extern */

/* ********************************** */

/* Defaults */
static unsigned int bucket_len = 128, num_slots = 4096, sample_rate = 1,
  transparent_mode = 1, enable_tx_capture = 1;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16))
module_param(bucket_len, uint, 0644);
module_param(num_slots,  uint, 0644);
module_param(sample_rate, uint, 0644);
module_param(transparent_mode, uint, 0644);
module_param(enable_tx_capture, uint, 0644);
#else
MODULE_PARM(bucket_len, "i");
MODULE_PARM(num_slots, "i");
MODULE_PARM(sample_rate, "i");
MODULE_PARM(transparent_mode, "i");
MODULE_PARM(enable_tx_capture, "i");
#endif

MODULE_PARM_DESC(bucket_len, "Number of ring buckets");
MODULE_PARM_DESC(num_slots,  "Number of ring slots");
MODULE_PARM_DESC(sample_rate, "Ring packet sample rate");
MODULE_PARM_DESC(transparent_mode,
		 "Set to 1 to set transparent mode "
		 "(slower but backwards compatible)");

MODULE_PARM_DESC(enable_tx_capture, "Set to 1 to capture outgoing packets");

/* ********************************** */

#define MIN_QUEUED_PKTS      64
#define MAX_QUEUE_LOOPS      64


#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
#define ring_sk_datatype(__sk) ((struct ring_opt *)__sk)
#define ring_sk(__sk) ((__sk)->sk_protinfo)
#else
#define ring_sk_datatype(a) (a)
#define ring_sk(__sk) ((__sk)->protinfo.pf_ring)
#endif

#define _rdtsc() ({ uint64_t x; asm volatile("rdtsc" : "=A" (x)); x; })

/*
  int dev_queue_xmit(struct sk_buff *skb)
  skb->dev;
  struct net_device *dev_get_by_name(const char *name)
*/

/* ********************************** */

/*
**   $Id$
**
**   acsmx2.c
**
**   Multi-Pattern Search Engine
**
**   Aho-Corasick State Machine - version 2.0
**
**   Supports both Non-Deterministic and Deterministic Finite Automata
**
**
**   Reference - Efficient String matching: An Aid to Bibliographic Search
**               Alfred V Aho and Margaret J Corasick
**               Bell Labratories
**               Copyright(C) 1975 Association for Computing Machinery,Inc
**
**   +++
**   +++ Version 1.0 notes - Marc Norton:
**   +++
**
**   Original implementation based on the 4 algorithms in the paper by Aho & Corasick,
**   some implementation ideas from 'Practical Algorithms in C', and some
**   of my own.
**
**   1) Finds all occurrences of all patterns within a text.
**
**   +++
**   +++ Version 2.0 Notes - Marc Norton/Dan Roelker:
**   +++
**
**   New implementation modifies the state table storage and access model to use
**   compacted sparse vector storage. Dan Roelker and I hammered this strategy out
**   amongst many others in order to reduce memory usage and improve caching performance.
**   The memory usage is greatly reduced, we only use 1/4 of what we use to. The caching
**   performance is better in pure benchmarking tests, but does not show overall improvement
**   in Snort.  Unfortunately, once a pattern match test has been performed Snort moves on to doing
**   many other things before we get back to a patteren match test, so the cache is voided.
**
**   This versions has better caching performance characteristics, reduced memory,
**   more state table storage options, and requires no a priori case conversions.
**   It does maintain the same public interface. (Snort only used banded storage).
**
**     1) Supports NFA and DFA state machines, and basic keyword state machines
**     2) Initial transition table uses Linked Lists
**     3) Improved state table memory options. NFA and DFA state
**        transition tables are converted to one of 4 formats during compilation.
**        a) Full matrix
**        b) Sparse matrix
**        c) Banded matrix (Default-this is the only one used in snort)
**        d) Sparse-Banded matrix
**     4) Added support for acstate_t in .h file so we can compile states as
**        16, or 32 bit state values for another reduction in memory consumption,
**        smaller states allows more of the state table to be cached, and improves
**        performance on x86-P4.  Your mileage may vary, especially on risc systems.
**     5) Added a bool to each state transition list to indicate if there is a matching
**        pattern in the state. This prevents us from accessing another data array
**        and can improve caching/performance.
**     6) The search functions are very sensitive, don't change them without extensive testing,
**        or you'll just spoil the caching and prefetching opportunities.
**
**   Extras for fellow pattern matchers:
**    The table below explains the storage format used at each step.
**    You can use an NFA or DFA to match with, the NFA is slower but tiny - set the structure directly.
**    You can use any of the 4 storage modes above -full,sparse,banded,sparse-bands, set the structure directly.
**    For applications where you have lots of data and a pattern set to search, this version was up to 3x faster
**    than the previous verion, due to caching performance. This cannot be fully realized in Snort yet,
**    but other applications may have better caching opportunities.
**    Snort only needs to use the banded or full storage.
**
**  Transition table format at each processing stage.
**  -------------------------------------------------
**  Patterns -> Keyword State Table (List)
**  Keyword State Table -> NFA (List)
**  NFA -> DFA (List)
**  DFA (List)-> Sparse Rows  O(m-avg # transitions per state)
**	      -> Banded Rows  O(1)
**            -> Sparse-Banded Rows O(nb-# bands)
**	      -> Full Matrix  O(1)
**
** Copyright(C) 2002,2003,2004 Marc Norton
** Copyright(C) 2003,2004 Daniel Roelker
** Copyright(C) 2002,2003,2004 Sourcefire,Inc.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*
*/

/*
 *
 */
#define MEMASSERT(p,s) if(!p){printk("ACSM-No Memory: %s!\n",s);}

/*
 *
 */
static int max_memory = 0;

/*
 *
 */
typedef struct acsm_summary_s
{
  unsigned    num_states;
  unsigned    num_transitions;
  ACSM_STRUCT2 acsm;

}acsm_summary_t;

/*
 *
 */
static acsm_summary_t summary={0,0};

/*
** Case Translation Table
*/
static unsigned char xlatcase[256];
/*
 *
 */

inline int toupper(int ch) {
  if ( (unsigned int)(ch - 'a') < 26u )
    ch += 'A' - 'a';
  return ch;
}

static void init_xlatcase(void)
{
  int i;
  for (i = 0; i < 256; i++)
    {
      xlatcase[i] = toupper(i);
    }
}

/*
 *    Case Conversion
 */
static
inline
void
ConvertCaseEx (unsigned char *d, unsigned char *s, int m)
{
  int i;
#ifdef XXXX
  int n;
  n   = m & 3;
  m >>= 2;

  for (i = 0; i < m; i++ )
    {
      d[0] = xlatcase[ s[0] ];
      d[2] = xlatcase[ s[2] ];
      d[1] = xlatcase[ s[1] ];
      d[3] = xlatcase[ s[3] ];
      d+=4;
      s+=4;
    }

  for (i=0; i < n; i++)
    {
      d[i] = xlatcase[ s[i] ];
    }
#else
  for (i=0; i < m; i++)
    {
      d[i] = xlatcase[ s[i] ];
    }

#endif
}


/*
 *
 */
static void *
AC_MALLOC (int n)
{
  void *p;
  p = kmalloc (n, GFP_KERNEL);
  if (p)
    max_memory += n;
  return p;
}


/*
 *
 */
static void
AC_FREE (void *p)
{
  if (p)
    kfree (p);
}


/*
 *    Simple QUEUE NODE
 */
typedef struct _qnode
{
  int state;
  struct _qnode *next;
}
  QNODE;

/*
 *    Simple QUEUE Structure
 */
typedef struct _queue
{
  QNODE * head, *tail;
  int count;
}
  QUEUE;

/*
 *   Initialize the queue
 */
static void
queue_init (QUEUE * s)
{
  s->head = s->tail = 0;
  s->count= 0;
}

/*
 *  Find a State in the queue
 */
static int
queue_find (QUEUE * s, int state)
{
  QNODE * q;
  q = s->head;
  while( q )
    {
      if( q->state == state ) return 1;
      q = q->next;
    }
  return 0;
}

/*
 *  Add Tail Item to queue (FiFo/LiLo)
 */
static void
queue_add (QUEUE * s, int state)
{
  QNODE * q;

  if( queue_find( s, state ) ) return;

  if (!s->head)
    {
      q = s->tail = s->head = (QNODE *) AC_MALLOC (sizeof (QNODE));
      MEMASSERT (q, "queue_add");
      q->state = state;
      q->next = 0;
    }
  else
    {
      q = (QNODE *) AC_MALLOC (sizeof (QNODE));
      q->state = state;
      q->next = 0;
      s->tail->next = q;
      s->tail = q;
    }
  s->count++;
}


/*
 *  Remove Head Item from queue
 */
static int
queue_remove (QUEUE * s)
{
  int state = 0;
  QNODE * q;
  if (s->head)
    {
      q       = s->head;
      state   = q->state;
      s->head = s->head->next;
      s->count--;

      if( !s->head )
	{
	  s->tail = 0;
	  s->count = 0;
	}
      AC_FREE (q);
    }
  return state;
}


/*
 *   Return items in the queue
 */
static int
queue_count (QUEUE * s)
{
  return s->count;
}


/*
 *  Free the queue
 */
static void
queue_free (QUEUE * s)
{
  while (queue_count (s))
    {
      queue_remove (s);
    }
}

/*
 *  Get Next State-NFA
 */
static
int List_GetNextState( ACSM_STRUCT2 * acsm, int state, int input )
{
  trans_node_t * t = acsm->acsmTransTable[state];

  while( t )
    {
      if( t->key == input )
	{
	  return t->next_state;
	}
      t=t->next;
    }

  if( state == 0 ) return 0;

  return ACSM_FAIL_STATE2; /* Fail state ??? */
}

/*
 *  Get Next State-DFA
 */
static
int List_GetNextState2( ACSM_STRUCT2 * acsm, int state, int input )
{
  trans_node_t * t = acsm->acsmTransTable[state];

  while( t )
    {
      if( t->key == input )
	{
	  return t->next_state;
	}
      t = t->next;
    }

  return 0; /* default state */
}
/*
 *  Put Next State - Head insertion, and transition updates
 */
static
int List_PutNextState( ACSM_STRUCT2 * acsm, int state, int input, int next_state )
{
  trans_node_t * p;
  trans_node_t * tnew;

  // printk("   List_PutNextState: state=%d, input='%c', next_state=%d\n",state,input,next_state);


  /* Check if the transition already exists, if so just update the next_state */
  p = acsm->acsmTransTable[state];
  while( p )
    {
      if( p->key == input )  /* transition already exists- reset the next state */
	{
	  p->next_state = next_state;
	  return 0;
	}
      p=p->next;
    }

  /* Definitely not an existing transition - add it */
  tnew = (trans_node_t*)AC_MALLOC(sizeof(trans_node_t));
  if( !tnew ) return -1;

  tnew->key        = input;
  tnew->next_state = next_state;
  tnew->next       = 0;

  tnew->next = acsm->acsmTransTable[state];
  acsm->acsmTransTable[state] = tnew;

  acsm->acsmNumTrans++;

  return 0;
}
/*
 *   Free the entire transition table
 */
static
int List_FreeTransTable( ACSM_STRUCT2 * acsm )
{
  int i;
  trans_node_t * t, *p;

  if( !acsm->acsmTransTable ) return 0;

  for(i=0;i< acsm->acsmMaxStates;i++)
    {
      t = acsm->acsmTransTable[i];

      while( t )
	{
	  p = t->next;
	  kfree(t);
	  t = p;
	  max_memory -= sizeof(trans_node_t);
	}
    }

  kfree(acsm->acsmTransTable);

  max_memory -= sizeof(void*) * acsm->acsmMaxStates;

  acsm->acsmTransTable = 0;

  return 0;
}

/*
 *
 */
/*
  static
  int List_FreeList( trans_node_t * t )
  {
  int tcnt=0;

  trans_node_t *p;

  while( t )
  {
  p = t->next;
  kfree(t);
  t = p;
  max_memory -= sizeof(trans_node_t);
  tcnt++;
  }

  return tcnt;
  }
*/

/*
 *   Converts row of states from list to a full vector format
 */
static
int List_ConvToFull(ACSM_STRUCT2 * acsm, acstate_t state, acstate_t * full )
{
  int tcnt = 0;
  trans_node_t * t = acsm->acsmTransTable[ state ];

  memset(full,0,sizeof(acstate_t)*acsm->acsmAlphabetSize);

  if( !t ) return 0;

  while(t)
    {
      full[ t->key ] = t->next_state;
      tcnt++;
      t = t->next;
    }
  return tcnt;
}

/*
 *   Copy a Match List Entry - don't dup the pattern data
 */
static ACSM_PATTERN2*
CopyMatchListEntry (ACSM_PATTERN2 * px)
{
  ACSM_PATTERN2 * p;

  p = (ACSM_PATTERN2 *) AC_MALLOC (sizeof (ACSM_PATTERN2));
  MEMASSERT (p, "CopyMatchListEntry");

  memcpy (p, px, sizeof (ACSM_PATTERN2));

  p->next = 0;

  return p;
}

/*
 *  Check if a pattern is in the list already,
 *  validate it using the 'id' field. This must be unique
 *  for every pattern.
 */
/*
  static
  int FindMatchListEntry (ACSM_STRUCT2 * acsm, int state, ACSM_PATTERN2 * px)
  {
  ACSM_PATTERN2 * p;

  p = acsm->acsmMatchList[state];
  while( p )
  {
  if( p->id == px->id ) return 1;
  p = p->next;
  }

  return 0;
  }
*/


/*
 *  Add a pattern to the list of patterns terminated at this state.
 *  Insert at front of list.
 */
static void
AddMatchListEntry (ACSM_STRUCT2 * acsm, int state, ACSM_PATTERN2 * px)
{
  ACSM_PATTERN2 * p;

  p = (ACSM_PATTERN2 *) AC_MALLOC (sizeof (ACSM_PATTERN2));

  MEMASSERT (p, "AddMatchListEntry");

  memcpy (p, px, sizeof (ACSM_PATTERN2));

  p->next = acsm->acsmMatchList[state];

  acsm->acsmMatchList[state] = p;
}


static void
AddPatternStates (ACSM_STRUCT2 * acsm, ACSM_PATTERN2 * p)
{
  int            state, next, n;
  unsigned char *pattern;

  n       = p->n;
  pattern = p->patrn;
  state   = 0;

  /*
   *  Match up pattern with existing states
   */
  for (; n > 0; pattern++, n--)
    {
      next = List_GetNextState(acsm,state,*pattern);
      if (next == ACSM_FAIL_STATE2 || next == 0)
	{
	  break;
	}
      state = next;
    }

  /*
   *   Add new states for the rest of the pattern bytes, 1 state per byte
   */
  for (; n > 0; pattern++, n--)
    {
      acsm->acsmNumStates++;
      List_PutNextState(acsm,state,*pattern,acsm->acsmNumStates);
      state = acsm->acsmNumStates;
    }

  AddMatchListEntry (acsm, state, p );
}

/*
 *   Build A Non-Deterministic Finite Automata
 *   The keyword state table must already be built, via AddPatternStates().
 */
static void
Build_NFA (ACSM_STRUCT2 * acsm)
{
  int r, s, i;
  QUEUE q, *queue = &q;
  acstate_t     * FailState = acsm->acsmFailState;
  ACSM_PATTERN2 ** MatchList = acsm->acsmMatchList;
  ACSM_PATTERN2  * mlist,* px;

  /* Init a Queue */
  queue_init (queue);


  /* Add the state 0 transitions 1st, the states at depth 1, fail to state 0 */
  for (i = 0; i < acsm->acsmAlphabetSize; i++)
    {
      s = List_GetNextState2(acsm,0,i);
      if( s )
	{
	  queue_add (queue, s);
	  FailState[s] = 0;
	}
    }

  /* Build the fail state successive layer of transitions */
  while (queue_count (queue) > 0)
    {
      r = queue_remove (queue);

      /* Find Final States for any Failure */
      for (i = 0; i < acsm->acsmAlphabetSize; i++)
	{
	  int fs, next;

	  s = List_GetNextState(acsm,r,i);

	  if( s != ACSM_FAIL_STATE2 )
	    {
	      queue_add (queue, s);

	      fs = FailState[r];

	      /*
	       *  Locate the next valid state for 'i' starting at fs
	       */
	      while( (next=List_GetNextState(acsm,fs,i)) == ACSM_FAIL_STATE2 )
		{
		  fs = FailState[fs];
		}

	      /*
	       *  Update 's' state failure state to point to the next valid state
	       */
	      FailState[s] = next;

	      /*
	       *  Copy 'next'states MatchList to 's' states MatchList,
	       *  we copy them so each list can be AC_FREE'd later,
	       *  else we could just manipulate pointers to fake the copy.
	       */
	      for( mlist = MatchList[next];
		   mlist;
		   mlist = mlist->next)
		{
		  px = CopyMatchListEntry (mlist);

		  /* Insert at front of MatchList */
		  px->next = MatchList[s];
		  MatchList[s] = px;
		}
	    }
	}
    }

  /* Clean up the queue */
  queue_free (queue);
}

/*
 *   Build Deterministic Finite Automata from the NFA
 */
static void
Convert_NFA_To_DFA (ACSM_STRUCT2 * acsm)
{
  int i, r, s, cFailState;
  QUEUE  q, *queue = &q;
  acstate_t * FailState = acsm->acsmFailState;

  /* Init a Queue */
  queue_init (queue);

  /* Add the state 0 transitions 1st */
  for(i=0; i<acsm->acsmAlphabetSize; i++)
    {
      s = List_GetNextState(acsm,0,i);
      if ( s != 0 )
	{
	  queue_add (queue, s);
	}
    }

  /* Start building the next layer of transitions */
  while( queue_count(queue) > 0 )
    {
      r = queue_remove(queue);

      /* Process this states layer */
      for (i = 0; i < acsm->acsmAlphabetSize; i++)
	{
	  s = List_GetNextState(acsm,r,i);

	  if( s != ACSM_FAIL_STATE2 && s!= 0)
	    {
	      queue_add (queue, s);
	    }
	  else
	    {
	      cFailState = List_GetNextState(acsm,FailState[r],i);

	      if( cFailState != 0 && cFailState != ACSM_FAIL_STATE2 )
		{
		  List_PutNextState(acsm,r,i,cFailState);
		}
	    }
	}
    }

  /* Clean up the queue */
  queue_free (queue);
}

/*
 *
 *  Convert a row lists for the state table to a full vector format
 *
 */
static int
Conv_List_To_Full(ACSM_STRUCT2 * acsm)
{
  int         tcnt, k;
  acstate_t * p;
  acstate_t ** NextState = acsm->acsmNextState;

  for(k=0;k<acsm->acsmMaxStates;k++)
    {
      p = AC_MALLOC( sizeof(acstate_t) * (acsm->acsmAlphabetSize+2) );
      if(!p) return -1;

      tcnt = List_ConvToFull( acsm, (acstate_t)k, p+2 );

      p[0] = ACF_FULL;
      p[1] = 0; /* no matches yet */

      NextState[k] = p; /* now we have a full format row vector  */
    }

  return 0;
}

/*
 *   Convert DFA memory usage from list based storage to a sparse-row storage.
 *
 *   The Sparse format allows each row to be either full or sparse formatted.  If the sparse row has
 *   too many transitions, performance or space may dictate that we use the standard full formatting
 *   for the row.  More than 5 or 10 transitions per state ought to really whack performance. So the
 *   user can specify the max state transitions per state allowed in the sparse format.
 *
 *   Standard Full Matrix Format
 *   ---------------------------
 *   acstate_t ** NextState ( 1st index is row/state, 2nd index is column=event/input)
 *
 *   example:
 *
 *        events -> a b c d e f g h i j k l m n o p
 *   states
 *     N            1 7 0 0 0 3 0 0 0 0 0 0 0 0 0 0
 *
 *   Sparse Format, each row : Words     Value
 *                            1-1       fmt(0-full,1-sparse,2-banded,3-sparsebands)
 *			     2-2       bool match flag (indicates this state has pattern matches)
 *                            3-3       sparse state count ( # of input/next-state pairs )
 *                            4-3+2*cnt 'input,next-state' pairs... each sizof(acstate_t)
 *
 *   above example case yields:
 *     Full Format:    0, 1 7 0 0 0 3 0 0 0 0 0 0 0 0 0 0 ...
 *     Sparse format:  1, 3, 'a',1,'b',7,'f',3  - uses 2+2*ntransitions (non-default transitions)
 */
static int
Conv_Full_DFA_To_Sparse(ACSM_STRUCT2 * acsm)
{
  int          cnt, m, k, i;
  acstate_t  * p, state, maxstates=0;
  acstate_t ** NextState = acsm->acsmNextState;
  acstate_t    full[MAX_ALPHABET_SIZE];

  for(k=0;k<acsm->acsmMaxStates;k++)
    {
      cnt=0;

      List_ConvToFull(acsm, (acstate_t)k, full );

      for (i = 0; i < acsm->acsmAlphabetSize; i++)
	{
	  state = full[i];
	  if( state != 0 && state != ACSM_FAIL_STATE2 ) cnt++;
	}

      if( cnt > 0 ) maxstates++;

      if( k== 0 || cnt > acsm->acsmSparseMaxRowNodes )
	{
	  p = AC_MALLOC(sizeof(acstate_t)*(acsm->acsmAlphabetSize+2) );
	  if(!p) return -1;

	  p[0] = ACF_FULL;
	  p[1] = 0;
	  memcpy(&p[2],full,acsm->acsmAlphabetSize*sizeof(acstate_t));
	}
      else
	{
	  p = AC_MALLOC(sizeof(acstate_t)*(3+2*cnt));
	  if(!p) return -1;

	  m      = 0;
	  p[m++] = ACF_SPARSE;
	  p[m++] = 0;   /* no matches */
	  p[m++] = cnt;

	  for(i = 0; i < acsm->acsmAlphabetSize ; i++)
	    {
	      state = full[i];
	      if( state != 0 && state != ACSM_FAIL_STATE2 )
		{
		  p[m++] = i;
		  p[m++] = state;
		}
	    }
	}

      NextState[k] = p; /* now we are a sparse formatted state transition array  */
    }

  return 0;
}
/*
  Convert Full matrix to Banded row format.

  Word     values
  1        2  -> banded
  2        n  number of values
  3        i  index of 1st value (0-256)
  4 - 3+n  next-state values at each index

*/
static int
Conv_Full_DFA_To_Banded(ACSM_STRUCT2 * acsm)
{
  int first = -1, last;
  acstate_t * p, state, full[MAX_ALPHABET_SIZE];
  acstate_t ** NextState = acsm->acsmNextState;
  int       cnt,m,k,i;

  for(k=0;k<acsm->acsmMaxStates;k++)
    {
      cnt=0;

      List_ConvToFull(acsm, (acstate_t)k, full );

      first=-1;
      last =-2;

      for (i = 0; i < acsm->acsmAlphabetSize; i++)
	{
	  state = full[i];

	  if( state !=0 && state != ACSM_FAIL_STATE2 )
	    {
	      if( first < 0 ) first = i;
	      last = i;
	    }
	}

      /* calc band width */
      cnt= last - first + 1;

      p = AC_MALLOC(sizeof(acstate_t)*(4+cnt));

      if(!p) return -1;

      m      = 0;
      p[m++] = ACF_BANDED;
      p[m++] = 0;   /* no matches */
      p[m++] = cnt;
      p[m++] = first;

      for(i = first; i <= last; i++)
	{
	  p[m++] = full[i];
	}

      NextState[k] = p; /* now we are a banded formatted state transition array  */
    }

  return 0;
}

/*
 *   Convert full matrix to Sparse Band row format.
 *
 *   next  - Full formatted row of next states
 *   asize - size of alphabet
 *   zcnt - max number of zeros in a run of zeros in any given band.
 *
 *  Word Values
 *  1    ACF_SPARSEBANDS
 *  2    number of bands
 *  repeat 3 - 5+ ....once for each band in this row.
 *  3    number of items in this band*  4    start index of this band
 *  5-   next-state values in this band...
 */
static
int calcSparseBands( acstate_t * next, int * begin, int * end, int asize, int zmax )
{
  int i, nbands,zcnt,last=0;
  acstate_t state;

  nbands=0;
  for( i=0; i<asize; i++ )
    {
      state = next[i];

      if( state !=0 && state != ACSM_FAIL_STATE2 )
	{
	  begin[nbands] = i;
	  zcnt=0;

	  for( ; i< asize; i++ )
	    {
	      state = next[i];
	      if( state ==0 || state == ACSM_FAIL_STATE2 )
		{
		  zcnt++;
		  if( zcnt > zmax ) break;
		}
	      else
		{
		  zcnt=0;
		  last = i;
		}
	    }

	  end[nbands++] = last;

	}
    }

  return nbands;
}


/*
 *   Sparse Bands
 *
 *   Row Format:
 *   Word
 *   1    SPARSEBANDS format indicator
 *   2    bool indicates a pattern match in this state
 *   3    number of sparse bands
 *   4    number of elements in this band
 *   5    start index of this band
 *   6-   list of next states
 *
 *   m    number of elements in this band
 *   m+1  start index of this band
 *   m+2- list of next states
 */
static int
Conv_Full_DFA_To_SparseBands(ACSM_STRUCT2 * acsm)
{
  acstate_t  * p;
  acstate_t ** NextState = acsm->acsmNextState;
  int          cnt,m,k,i,zcnt=acsm->acsmSparseMaxZcnt;

  int       band_begin[MAX_ALPHABET_SIZE];
  int       band_end[MAX_ALPHABET_SIZE];
  int       nbands,j;
  acstate_t full[MAX_ALPHABET_SIZE];

  for(k=0;k<acsm->acsmMaxStates;k++)
    {
      cnt=0;

      List_ConvToFull(acsm, (acstate_t)k, full );

      nbands = calcSparseBands( full, band_begin, band_end, acsm->acsmAlphabetSize, zcnt );

      /* calc band width space*/
      cnt = 3;
      for(i=0;i<nbands;i++)
	{
	  cnt += 2;
	  cnt += band_end[i] - band_begin[i] + 1;

	  /*printk("state %d: sparseband %d,  first=%d, last=%d, cnt=%d\n",k,i,band_begin[i],band_end[i],band_end[i]-band_begin[i]+1); */
	}

      p = AC_MALLOC(sizeof(acstate_t)*(cnt));

      if(!p) return -1;

      m      = 0;
      p[m++] = ACF_SPARSEBANDS;
      p[m++] = 0; /* no matches */
      p[m++] = nbands;

      for( i=0;i<nbands;i++ )
	{
	  p[m++] = band_end[i] - band_begin[i] + 1;  /* # states in this band */
	  p[m++] = band_begin[i];   /* start index */

	  for( j=band_begin[i]; j<=band_end[i]; j++ )
	    {
	      p[m++] = full[j];  /* some states may be state zero */
	    }
	}

      NextState[k] = p; /* now we are a sparse-banded formatted state transition array  */
    }

  return 0;
}

/*
 *
 *   Convert an NFA or DFA row from sparse to full format
 *   and store into the 'full'  buffer.
 *
 *   returns:
 *     0 - failed, no state transitions
 *    *p - pointer to 'full' buffer
 *
 */
/*
  static
  acstate_t * acsmConvToFull(ACSM_STRUCT2 * acsm, acstate_t k, acstate_t * full )
  {
  int i;
  acstate_t * p, n, fmt, index, nb, bmatch;
  acstate_t ** NextState = acsm->acsmNextState;

  p   = NextState[k];

  if( !p ) return 0;

  fmt = *p++;

  bmatch = *p++;

  if( fmt ==ACF_SPARSE )
  {
  n = *p++;
  for( ; n>0; n--, p+=2 )
  {
  full[ p[0] ] = p[1];
  }
  }
  else if( fmt ==ACF_BANDED )
  {

  n = *p++;
  index = *p++;

  for( ; n>0; n--, p++ )
  {
  full[ index++ ] = p[0];
  }
  }
  else if( fmt ==ACF_SPARSEBANDS )
  {
  nb    = *p++;
  for(i=0;i<nb;i++)
  {
  n     = *p++;
  index = *p++;
  for( ; n>0; n--, p++ )
  {
  full[ index++ ] = p[0];
  }
  }
  }
  else if( fmt == ACF_FULL )
  {
  memcpy(full,p,acsm->acsmAlphabetSize*sizeof(acstate_t));
  }

  return full;
  }
*/

/*
 *   Select the desired storage mode
 */
int acsmSelectFormat2( ACSM_STRUCT2 * acsm, int m )
{
  switch( m )
    {
    case ACF_FULL:
    case ACF_SPARSE:
    case ACF_BANDED:
    case ACF_SPARSEBANDS:
      acsm->acsmFormat = m;
      break;
    default:
      return -1;
    }

  return 0;
}
/*
 *
 */
void acsmSetMaxSparseBandZeros2( ACSM_STRUCT2 * acsm, int n )
{
  acsm->acsmSparseMaxZcnt = n;
}
/*
 *
 */
void acsmSetMaxSparseElements2( ACSM_STRUCT2 * acsm, int n )
{
  acsm->acsmSparseMaxRowNodes = n;
}
/*
 *
 */
int acsmSelectFSA2( ACSM_STRUCT2 * acsm, int m )
{
  switch( m )
    {
    case FSA_TRIE:
    case FSA_NFA:
    case FSA_DFA:
      acsm->acsmFSA = m;
    default:
      return -1;
    }
}
/*
 *
 */
int acsmSetAlphabetSize2( ACSM_STRUCT2 * acsm, int n )
{
  if( n <= MAX_ALPHABET_SIZE )
    {
      acsm->acsmAlphabetSize = n;
    }
  else
    {
      return -1;
    }
  return 0;
}
/*
 *  Create a new AC state machine
 */
static ACSM_STRUCT2 * acsmNew2 (void)
{
  ACSM_STRUCT2 * p;

  init_xlatcase ();

  p = (ACSM_STRUCT2 *) AC_MALLOC(sizeof (ACSM_STRUCT2));
  MEMASSERT (p, "acsmNew");

  if (p)
    {
      memset (p, 0, sizeof (ACSM_STRUCT2));

      /* Some defaults */
      p->acsmFSA               = FSA_DFA;
      p->acsmFormat            = ACF_BANDED;
      p->acsmAlphabetSize      = 256;
      p->acsmSparseMaxRowNodes = 256;
      p->acsmSparseMaxZcnt     = 10;
    }

  return p;
}
/*
 *   Add a pattern to the list of patterns for this state machine
 *
 */
int
acsmAddPattern2 (ACSM_STRUCT2 * p, unsigned char *pat, int n, int nocase,
		 int offset, int depth, void * id, int iid)
{
  ACSM_PATTERN2 * plist;

  plist = (ACSM_PATTERN2 *) AC_MALLOC (sizeof (ACSM_PATTERN2));
  MEMASSERT (plist, "acsmAddPattern");

  plist->patrn = (unsigned char *) AC_MALLOC ( n );
  MEMASSERT (plist->patrn, "acsmAddPattern");

  ConvertCaseEx(plist->patrn, pat, n);

  plist->casepatrn = (unsigned char *) AC_MALLOC ( n );
  MEMASSERT (plist->casepatrn, "acsmAddPattern");

  memcpy (plist->casepatrn, pat, n);

  plist->n      = n;
  plist->nocase = nocase;
  plist->offset = offset;
  plist->depth  = depth;
  plist->id     = id;
  plist->iid    = iid;

  plist->next     = p->acsmPatterns;
  p->acsmPatterns = plist;

  return 0;
}
/*
 *   Add a Key to the list of key+data pairs
 */
int acsmAddKey2(ACSM_STRUCT2 * p, unsigned char *key, int klen, int nocase, void * data)
{
  ACSM_PATTERN2 * plist;

  plist = (ACSM_PATTERN2 *) AC_MALLOC (sizeof (ACSM_PATTERN2));
  MEMASSERT (plist, "acsmAddPattern");

  plist->patrn = (unsigned char *) AC_MALLOC (klen);
  memcpy (plist->patrn, key, klen);

  plist->casepatrn = (unsigned char *) AC_MALLOC (klen);
  memcpy (plist->casepatrn, key, klen);

  plist->n      = klen;
  plist->nocase = nocase;
  plist->offset = 0;
  plist->depth  = 0;
  plist->id     = 0;
  plist->iid = 0;

  plist->next = p->acsmPatterns;
  p->acsmPatterns = plist;

  return 0;
}

/*
 *  Copy a boolean match flag int NextState table, for caching purposes.
 */
static
void acsmUpdateMatchStates( ACSM_STRUCT2 * acsm )
{
  acstate_t        state;
  acstate_t     ** NextState = acsm->acsmNextState;
  ACSM_PATTERN2 ** MatchList = acsm->acsmMatchList;

  for( state=0; state<acsm->acsmNumStates; state++ )
    {
      if( MatchList[state] )
	{
	  NextState[state][1] = 1;
	}
      else
	{
	  NextState[state][1] = 0;
	}
    }
}

/*
 *   Compile State Machine - NFA or DFA and Full or Banded or Sparse or SparseBands
 */
int
acsmCompile2 (ACSM_STRUCT2 * acsm)
{
  int               k;
  ACSM_PATTERN2    * plist;

  /* Count number of states */
  for (plist = acsm->acsmPatterns; plist != NULL; plist = plist->next)
    {
      acsm->acsmMaxStates += plist->n;
      /* acsm->acsmMaxStates += plist->n*2; if we handle case in the table */
    }
  acsm->acsmMaxStates++; /* one extra */

  /* Alloc a List based State Transition table */
  acsm->acsmTransTable =(trans_node_t**) AC_MALLOC(sizeof(trans_node_t*) * acsm->acsmMaxStates );
  MEMASSERT (acsm->acsmTransTable, "acsmCompile");

  memset (acsm->acsmTransTable, 0, sizeof(trans_node_t*) * acsm->acsmMaxStates);

  /* Alloc a failure table - this has a failure state, and a match list for each state */
  acsm->acsmFailState =(acstate_t*) AC_MALLOC(sizeof(acstate_t) * acsm->acsmMaxStates );
  MEMASSERT (acsm->acsmFailState, "acsmCompile");

  memset (acsm->acsmFailState, 0, sizeof(acstate_t) * acsm->acsmMaxStates );

  /* Alloc a MatchList table - this has a lis tof pattern matches for each state, if any */
  acsm->acsmMatchList=(ACSM_PATTERN2**) AC_MALLOC(sizeof(ACSM_PATTERN2*) * acsm->acsmMaxStates );
  MEMASSERT (acsm->acsmMatchList, "acsmCompile");

  memset (acsm->acsmMatchList, 0, sizeof(ACSM_PATTERN2*) * acsm->acsmMaxStates );

  /* Alloc a separate state transition table == in state 's' due to event 'k', transition to 'next' state */
  acsm->acsmNextState=(acstate_t**)AC_MALLOC( acsm->acsmMaxStates * sizeof(acstate_t*) );
  MEMASSERT(acsm->acsmNextState, "acsmCompile-NextState");

  for (k = 0; k < acsm->acsmMaxStates; k++)
    {
      acsm->acsmNextState[k]=(acstate_t*)0;
    }

  /* Initialize state zero as a branch */
  acsm->acsmNumStates = 0;

  /* Add the 0'th state,  */
  //acsm->acsmNumStates++;

  /* Add each Pattern to the State Table - This forms a keywords state table  */
  for (plist = acsm->acsmPatterns; plist != NULL; plist = plist->next)
    {
      AddPatternStates (acsm, plist);
    }

  acsm->acsmNumStates++;

  if( acsm->acsmFSA == FSA_DFA || acsm->acsmFSA == FSA_NFA )
    {
      /* Build the NFA */
      Build_NFA (acsm);
    }

  if( acsm->acsmFSA == FSA_DFA )
    {
      /* Convert the NFA to a DFA */
      Convert_NFA_To_DFA (acsm);
    }

  /*
   *
   *  Select Final Transition Table Storage Mode
   *
   */
  if( acsm->acsmFormat == ACF_SPARSE )
    {
      /* Convert DFA Full matrix to a Sparse matrix */
      if( Conv_Full_DFA_To_Sparse(acsm) )
	return -1;
    }

  else if( acsm->acsmFormat == ACF_BANDED )
    {
      /* Convert DFA Full matrix to a Sparse matrix */
      if( Conv_Full_DFA_To_Banded(acsm) )
	return -1;
    }

  else if( acsm->acsmFormat == ACF_SPARSEBANDS )
    {
      /* Convert DFA Full matrix to a Sparse matrix */
      if( Conv_Full_DFA_To_SparseBands(acsm) )
	return -1;
    }
  else if( acsm->acsmFormat == ACF_FULL )
    {
      if( Conv_List_To_Full( acsm ) )
	return -1;
    }

  acsmUpdateMatchStates( acsm ); /* load boolean match flags into state table */

  /* Free up the Table Of Transition Lists */
  List_FreeTransTable( acsm );

  /* For now -- show this info */
  /*
   *  acsmPrintInfo( acsm );
   */


  /* Accrue Summary State Stats */
  summary.num_states      += acsm->acsmNumStates;
  summary.num_transitions += acsm->acsmNumTrans;

  memcpy( &summary.acsm, acsm, sizeof(ACSM_STRUCT2));

  return 0;
}

/*
 *   Get the NextState from the NFA, all NFA storage formats use this
 */
inline
acstate_t SparseGetNextStateNFA(acstate_t * ps, acstate_t state, unsigned  input)
{
  acstate_t fmt;
  acstate_t n;
  int       index;
  int       nb;

  fmt = *ps++;

  ps++;  /* skip bMatchState */

  switch( fmt )
    {
    case  ACF_BANDED:
      {
	n     = ps[0];
	index = ps[1];

	if( input <  index     )
	  {
	    if(state==0)
	      {
		return 0;
	      }
	    else
	      {
		return (acstate_t)ACSM_FAIL_STATE2;
	      }
	  }
	if( input >= index + n )
	  {
	    if(state==0)
	      {
		return 0;
	      }
	    else
	      {
		return (acstate_t)ACSM_FAIL_STATE2;
	      }
	  }
	if( ps[input-index] == 0  )
	  {
	    if( state != 0 )
	      {
		return ACSM_FAIL_STATE2;
	      }
	  }

	return (acstate_t) ps[input-index];
      }

    case ACF_SPARSE:
      {
	n = *ps++; /* number of sparse index-value entries */

	for( ; n>0 ; n-- )
	  {
	    if( ps[0] > input ) /* cannot match the input, already a higher value than the input  */
	      {
		return (acstate_t)ACSM_FAIL_STATE2; /* default state */
	      }
	    else if( ps[0] == input )
	      {
		return ps[1]; /* next state */
	      }
	    ps+=2;
	  }
	if( state == 0 )
	  {
	    return 0;
	  }
	return ACSM_FAIL_STATE2;
      }

    case ACF_SPARSEBANDS:
      {
	nb  = *ps++;   /* number of bands */

	while( nb > 0 )  /* for each band */
	  {
	    n     = *ps++;  /* number of elements */
	    index = *ps++;  /* 1st element value */

	    if( input <  index )
	      {
		if( state != 0 )
		  {
		    return (acstate_t)ACSM_FAIL_STATE2;
		  }
		return (acstate_t)0;
	      }
	    if( (input >=  index) && (input < (index + n)) )
	      {
		if( ps[input-index] == 0 )
		  {
		    if( state != 0 )
		      {
			return ACSM_FAIL_STATE2;
		      }
		  }
		return (acstate_t) ps[input-index];
	      }
	    nb--;
	    ps += n;
	  }
	if( state != 0 )
	  {
	    return (acstate_t)ACSM_FAIL_STATE2;
	  }
	return (acstate_t)0;
      }

    case ACF_FULL:
      {
	if( ps[input] == 0 )
	  {
	    if( state != 0 )
	      {
		return ACSM_FAIL_STATE2;
	      }
	  }
	return ps[input];
      }
    }

  return 0;
}



/*
 *   Get the NextState from the DFA Next State Transition table
 *   Full and banded are supported separately, this is for
 *   sparse and sparse-bands
 */
inline
acstate_t SparseGetNextStateDFA(acstate_t * ps, acstate_t state, unsigned  input)
{
  acstate_t  n, nb;
  int        index;

  switch( ps[0] )
    {
      /*   BANDED   */
    case  ACF_BANDED:
      {
	/* n=ps[2] : number of entries in the band */
	/* index=ps[3] : index of the 1st entry, sequential thereafter */

	if( input  <  ps[3]        )  return 0;
	if( input >= (ps[3]+ps[2]) )  return 0;

	return  ps[4+input-ps[3]];
      }

      /*   FULL   */
    case ACF_FULL:
      {
	return ps[2+input];
      }

      /*   SPARSE   */
    case ACF_SPARSE:
      {
	n = ps[2]; /* number of entries/ key+next pairs */

	ps += 3;

	for( ; n>0 ; n-- )
	  {
	    if( input < ps[0]  ) /* cannot match the input, already a higher value than the input  */
	      {
		return (acstate_t)0; /* default state */
	      }
	    else if( ps[0] == input )
	      {
		return ps[1]; /* next state */
	      }
	    ps += 2;
	  }
	return (acstate_t)0;
      }


      /*   SPARSEBANDS   */
    case ACF_SPARSEBANDS:
      {
	nb  =  ps[2]; /* number of bands */

	ps += 3;

	while( nb > 0 )  /* for each band */
	  {
	    n     = ps[0];  /* number of elements in this band */
	    index = ps[1];  /* start index/char of this band */
	    if( input <  index )
	      {
		return (acstate_t)0;
	      }
	    if( (input < (index + n)) )
	      {
		return (acstate_t) ps[2+input-index];
	      }
	    nb--;
	    ps += n;
	  }
	return (acstate_t)0;
      }
    }

  return 0;
}
/*
 *   Search Text or Binary Data for Pattern matches
 *
 *   Sparse & Sparse-Banded Matrix search
 */
static
inline
int
acsmSearchSparseDFA(ACSM_STRUCT2 * acsm, unsigned char *Tx, int n,
		    int (*Match) (void * id, int index, void *data),
		    void *data)
{
  acstate_t state;
  ACSM_PATTERN2   * mlist;
  unsigned char   * Tend;
  int               nfound = 0;
  unsigned char   * T, * Tc;
  int               index;
  acstate_t      ** NextState = acsm->acsmNextState;
  ACSM_PATTERN2  ** MatchList = acsm->acsmMatchList;

  Tc   = Tx;
  T    = Tx;
  Tend = T + n;

  for( state = 0; T < Tend; T++ )
    {
      state = SparseGetNextStateDFA ( NextState[state], state, xlatcase[*T] );

      /* test if this state has any matching patterns */
      if( NextState[state][1] )
	{
	  for( mlist = MatchList[state];
	       mlist!= NULL;
	       mlist = mlist->next )
	    {
	      index = T - mlist->n - Tc;
	      if( mlist->nocase )
		{
		  nfound++;
		  if (Match (mlist->id, index, data))
		    return nfound;
		}
	      else
		{
		  if( memcmp (mlist->casepatrn, Tx + index, mlist->n) == 0 )
		    {
		      nfound++;
		      if (Match (mlist->id, index, data))
			return nfound;
		    }
		}
	    }
	}
    }
  return nfound;
}
/*
 *   Full format DFA search
 *   Do not change anything here without testing, caching and prefetching
 *   performance is very sensitive to any changes.
 *
 *   Perf-Notes:
 *    1) replaced ConvertCaseEx with inline xlatcase - this improves performance 5-10%
 *    2) using 'nocase' improves performance again by 10-15%, since memcmp is not needed
 *    3)
 */
static
inline
int
acsmSearchSparseDFA_Full(ACSM_STRUCT2 * acsm, unsigned char *Tx, int n,
			 int (*Match) (void * id, int index, void *data),
			 void *data)
{
  ACSM_PATTERN2   * mlist;
  unsigned char   * Tend;
  unsigned char   * T;
  int               index;
  acstate_t         state;
  acstate_t       * ps;
  acstate_t         sindex;
  acstate_t      ** NextState = acsm->acsmNextState;
  ACSM_PATTERN2  ** MatchList = acsm->acsmMatchList;
  int               nfound    = 0;

  T    = Tx;
  Tend = Tx + n;

  for( state = 0; T < Tend; T++ )
    {
      ps     = NextState[ state ];

      sindex = xlatcase[ T[0] ];

      /* check the current state for a pattern match */
      if( ps[1] )
	{
	  for( mlist = MatchList[state];
	       mlist!= NULL;
	       mlist = mlist->next )
	    {
	      index = T - mlist->n - Tx;


	      if( mlist->nocase )
		{
		  nfound++;
		  if (Match (mlist->id, index, data))
		    return nfound;
		}
	      else
		{
		  if( memcmp (mlist->casepatrn, Tx + index, mlist->n ) == 0 )
		    {
		      nfound++;
		      if (Match (mlist->id, index, data))
			return nfound;
		    }
		}

	    }
	}

      state = ps[ 2u + sindex ];
    }

  /* Check the last state for a pattern match */
  for( mlist = MatchList[state];
       mlist!= NULL;
       mlist = mlist->next )
    {
      index = T - mlist->n - Tx;

      if( mlist->nocase )
	{
	  nfound++;
	  if (Match (mlist->id, index, data))
	    return nfound;
	}
      else
	{
	  if( memcmp (mlist->casepatrn, Tx + index, mlist->n) == 0 )
	    {
	      nfound++;
	      if (Match (mlist->id, index, data))
		return nfound;
	    }
	}
    }

  return nfound;
}
/*
 *   Banded-Row format DFA search
 *   Do not change anything here, caching and prefetching
 *   performance is very sensitive to any changes.
 *
 *   ps[0] = storage fmt
 *   ps[1] = bool match flag
 *   ps[2] = # elements in band
 *   ps[3] = index of 1st element
 */
static
inline
int
acsmSearchSparseDFA_Banded(ACSM_STRUCT2 * acsm, unsigned char *Tx, int n,
			   int (*Match) (void * id, int index, void *data),
			   void *data)
{
  acstate_t         state;
  unsigned char   * Tend;
  unsigned char   * T;
  int               sindex;
  int               index;
  acstate_t      ** NextState = acsm->acsmNextState;
  ACSM_PATTERN2  ** MatchList = acsm->acsmMatchList;
  ACSM_PATTERN2   * mlist;
  acstate_t       * ps;
  int               nfound = 0;

  T    = Tx;
  Tend = T + n;

  for( state = 0; T < Tend; T++ )
    {
      ps     = NextState[state];

      sindex = xlatcase[ T[0] ];

      /* test if this state has any matching patterns */
      if( ps[1] )
	{
	  for( mlist = MatchList[state];
	       mlist!= NULL;
	       mlist = mlist->next )
	    {
	      index = T - mlist->n - Tx;

	      if( mlist->nocase )
		{
		  nfound++;
		  if (Match (mlist->id, index, data))
		    return nfound;
		}
	      else
		{
		  if( memcmp (mlist->casepatrn, Tx + index, mlist->n) == 0 )
		    {
		      nfound++;
		      if (Match (mlist->id, index, data))
			return nfound;
		    }
		}
	    }
	}

      if(      sindex <   ps[3]          )  state = 0;
      else if( sindex >= (ps[3] + ps[2]) )  state = 0;
      else                                  state = ps[ 4u + sindex - ps[3] ];
    }

  /* Check the last state for a pattern match */
  for( mlist = MatchList[state];
       mlist!= NULL;
       mlist = mlist->next )
    {
      index = T - mlist->n - Tx;

      if( mlist->nocase )
	{
	  nfound++;
	  if (Match (mlist->id, index, data))
	    return nfound;
	}
      else
	{
	  if( memcmp (mlist->casepatrn, Tx + index, mlist->n) == 0 )
	    {
	      nfound++;
	      if (Match (mlist->id, index, data))
		return nfound;
	    }
	}
    }

  return nfound;
}



/*
 *   Search Text or Binary Data for Pattern matches
 *
 *   Sparse Storage Version
 */
static
inline
int
acsmSearchSparseNFA(ACSM_STRUCT2 * acsm, unsigned char *Tx, int n,
		    int (*Match) (void * id, int index, void *data),
		    void *data)
{
  acstate_t         state;
  ACSM_PATTERN2   * mlist;
  unsigned char   * Tend;
  int               nfound = 0;
  unsigned char   * T, *Tc;
  int               index;
  acstate_t      ** NextState= acsm->acsmNextState;
  acstate_t       * FailState= acsm->acsmFailState;
  ACSM_PATTERN2  ** MatchList = acsm->acsmMatchList;
  unsigned char     Tchar;

  Tc   = Tx;
  T    = Tx;
  Tend = T + n;

  for( state = 0; T < Tend; T++ )
    {
      acstate_t nstate;

      Tchar = xlatcase[ *T ];

      while( (nstate=SparseGetNextStateNFA(NextState[state],state,Tchar))==ACSM_FAIL_STATE2 )
	state = FailState[state];

      state = nstate;

      for( mlist = MatchList[state];
	   mlist!= NULL;
	   mlist = mlist->next )
	{
	  index = T - mlist->n - Tx;
	  if( mlist->nocase )
	    {
	      nfound++;
	      if (Match (mlist->id, index, data))
		return nfound;
	    }
	  else
	    {
	      if( memcmp (mlist->casepatrn, Tx + index, mlist->n) == 0 )
		{
		  nfound++;
		  if (Match (mlist->id, index, data))
		    return nfound;
		}
	    }
	}
    }

  return nfound;
}

/*
 *   Search Function
 */
int
acsmSearch2(ACSM_STRUCT2 * acsm, unsigned char *Tx, int n,
	    int (*Match) (void * id, int index, void *data),
	    void *data)
{

  switch( acsm->acsmFSA )
    {
    case FSA_DFA:

      if( acsm->acsmFormat == ACF_FULL )
	{
	  return acsmSearchSparseDFA_Full( acsm, Tx, n, Match,data );
	}
      else if( acsm->acsmFormat == ACF_BANDED )
	{
	  return acsmSearchSparseDFA_Banded( acsm, Tx, n, Match,data );
	}
      else
	{
	  return acsmSearchSparseDFA( acsm, Tx, n, Match,data );
	}

    case FSA_NFA:

      return acsmSearchSparseNFA( acsm, Tx, n, Match,data );

    case FSA_TRIE:

      return 0;
    }
  return 0;
}


/*
 *   Free all memory
 */
void
acsmFree2 (ACSM_STRUCT2 * acsm)
{
  int i;
  ACSM_PATTERN2 * mlist, *ilist;
  for (i = 0; i < acsm->acsmMaxStates; i++)
    {
      mlist = acsm->acsmMatchList[i];

      while (mlist)
	{
	  ilist = mlist;
	  mlist = mlist->next;
	  AC_FREE (ilist);
	}
      AC_FREE(acsm->acsmNextState[i]);
    }
  AC_FREE(acsm->acsmFailState);
  AC_FREE(acsm->acsmMatchList);
}

/* ********************************** */

static void ring_sock_destruct(struct sock *sk) {

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
  skb_queue_purge(&sk->sk_receive_queue);

  if (!sock_flag(sk, SOCK_DEAD)) {
#if defined(RING_DEBUG)
    printk("Attempt to release alive ring socket: %p\n", sk);
#endif
    return;
  }

  BUG_TRAP(!atomic_read(&sk->sk_rmem_alloc));
  BUG_TRAP(!atomic_read(&sk->sk_wmem_alloc));
#else

  BUG_TRAP(atomic_read(&sk->rmem_alloc)==0);
  BUG_TRAP(atomic_read(&sk->wmem_alloc)==0);

  if (!sk->dead) {
#if defined(RING_DEBUG)
    printk("Attempt to release alive ring socket: %p\n", sk);
#endif
    return;
  }
#endif

  kfree(ring_sk(sk));

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
  MOD_DEC_USE_COUNT;
#endif
}

/* ********************************** */

static void ring_proc_add(struct ring_opt *pfr) {
  if(ring_proc_dir != NULL) {
    char name[16];

    pfr->ring_pid = current->pid; 

    snprintf(name, sizeof(name), "%d", pfr->ring_pid);
    create_proc_read_entry(name, 0, ring_proc_dir,
			   ring_proc_get_info, pfr);
    /* printk("PF_RING: added /proc/net/pf_ring/%s\n", name); */
  }
}

/* ********************************** */

static void ring_proc_remove(struct ring_opt *pfr) {
  if(ring_proc_dir != NULL) {
    char name[16];

    snprintf(name, sizeof(name), "%d", pfr->ring_pid);
    remove_proc_entry(name, ring_proc_dir);
    /* printk("PF_RING: removed /proc/net/pf_ring/%s\n", name); */
  }
}

/* ********************************** */

static int ring_proc_get_info(char *buf, char **start, off_t offset,
			      int len, int *unused, void *data)
{
  int rlen = 0;
  struct ring_opt *pfr;
  FlowSlotInfo *fsi;

  if(data == NULL) {
    /* /proc/net/pf_ring/info */
    rlen = sprintf(buf,"Version             : %s\n", RING_VERSION);
    rlen += sprintf(buf + rlen,"Bucket length       : %d bytes\n", bucket_len);
    rlen += sprintf(buf + rlen,"Ring slots          : %d\n", num_slots);
    rlen += sprintf(buf + rlen,"Sample rate         : %d [1=no sampling]\n", sample_rate);

    rlen += sprintf(buf + rlen,"Capture TX          : %s\n",
		    enable_tx_capture ? "Yes [RX+TX]" : "No [RX only]");
    rlen += sprintf(buf + rlen,"Transparent mode    : %s\n", 
		    transparent_mode ? "Yes" : "No");
    rlen += sprintf(buf + rlen,"Total rings         : %d\n", ring_table_size);
  } else {
    /* detailed statistics about a PF_RING */
    pfr = (struct ring_opt*)data;

    if(data) {
      fsi = pfr->slots_info;

      if(fsi) {
	rlen = sprintf(buf,        "Bound Device  : %s\n",
		       pfr->ring_netdev->name == NULL ? "<NULL>" : pfr->ring_netdev->name);
	rlen += sprintf(buf + rlen,"Version       : %d\n",  fsi->version);
	rlen += sprintf(buf + rlen,"Sampling Rate : %d\n",  pfr->sample_rate);
	rlen += sprintf(buf + rlen,"Cluster Id    : %d\n",  pfr->cluster_id);
	rlen += sprintf(buf + rlen,"Tot Slots     : %d\n",  fsi->tot_slots);
	rlen += sprintf(buf + rlen,"Slot Len      : %d\n",  fsi->slot_len);
	rlen += sprintf(buf + rlen,"Data Len      : %d\n",  fsi->data_len);
	rlen += sprintf(buf + rlen,"Tot Memory    : %d\n",  fsi->tot_mem);
	rlen += sprintf(buf + rlen,"Tot Packets   : %lu\n", (unsigned long)fsi->tot_pkts);
	rlen += sprintf(buf + rlen,"Tot Pkt Lost  : %lu\n", (unsigned long)fsi->tot_lost);
	rlen += sprintf(buf + rlen,"Tot Insert    : %lu\n", (unsigned long)fsi->tot_insert);
	rlen += sprintf(buf + rlen,"Tot Read      : %lu\n", (unsigned long)fsi->tot_read);

      } else
	rlen = sprintf(buf, "WARNING fsi == NULL\n");
    } else
      rlen = sprintf(buf, "WARNING data == NULL\n");
  }

  return rlen;
}

/* ********************************** */

static void ring_proc_init(void) {
  ring_proc_dir = proc_mkdir("pf_ring", proc_net);

  if(ring_proc_dir) {
    ring_proc_dir->owner = THIS_MODULE;
    ring_proc = create_proc_read_entry("info", 0, ring_proc_dir,
				       ring_proc_get_info, NULL);
    if(!ring_proc)
      printk("PF_RING: unable to register proc file\n");
    else {
      ring_proc->owner = THIS_MODULE;
      printk("PF_RING: registered /proc/net/pf_ring/\n");
    }
  } else
    printk("PF_RING: unable to create /proc/net/pf_ring\n");
}

/* ********************************** */

static void ring_proc_term(void) {
  if(ring_proc != NULL) {
    remove_proc_entry("info", ring_proc_dir);
    if(ring_proc_dir != NULL) remove_proc_entry("pf_ring", proc_net);

    printk("PF_RING: deregistered /proc/net/pf_ring\n");
  }
}

/* ********************************** */

/*
 * ring_insert()
 *
 * store the sk in a new element and add it
 * to the head of the list.
 */
static inline void ring_insert(struct sock *sk) {
  struct ring_element *next;

#if defined(RING_DEBUG)
  printk("RING: ring_insert()\n");
#endif

  next = kmalloc(sizeof(struct ring_element), GFP_ATOMIC);
  if(next != NULL) {
    next->sk = sk;
    write_lock_irq(&ring_mgmt_lock);
    list_add(&next->list, &ring_table);
    write_unlock_irq(&ring_mgmt_lock);
  } else {
    if(net_ratelimit())
      printk("RING: could not kmalloc slot!!\n");
  }

  ring_table_size++;
  ring_proc_add(ring_sk(sk));
}

/* ********************************** */

/*
 * ring_remove()
 *
 * For each of the elements in the list:
 *  - check if this is the element we want to delete
 *  - if it is, remove it from the list, and free it.
 *
 * stop when we find the one we're looking for (break),
 * or when we reach the end of the list.
 */
static inline void ring_remove(struct sock *sk) {
  struct list_head *ptr;
  struct ring_element *entry;

  for(ptr = ring_table.next; ptr != &ring_table; ptr = ptr->next) {
    entry = list_entry(ptr, struct ring_element, list);

    if(entry->sk == sk) {
      list_del(ptr);
      kfree(ptr);
      ring_table_size--;
      break;
    }
  }
}

/* ********************************** */

static u_int32_t num_queued_pkts(struct ring_opt *pfr) {

  if(pfr->ring_slots != NULL) {

    u_int32_t tot_insert = pfr->slots_info->insert_idx,
#if defined(RING_DEBUG)
      tot_read = pfr->slots_info->tot_read, tot_pkts;
#else
    tot_read = pfr->slots_info->tot_read;
#endif

    if(tot_insert >= tot_read) {
#if defined(RING_DEBUG)
      tot_pkts = tot_insert-tot_read;
#endif
      return(tot_insert-tot_read);
    } else {
#if defined(RING_DEBUG)
      tot_pkts = ((u_int32_t)-1)+tot_insert-tot_read;
#endif
      return(((u_int32_t)-1)+tot_insert-tot_read);
    }

#if defined(RING_DEBUG)
    printk("-> num_queued_pkts=%d [tot_insert=%d][tot_read=%d]\n",
	   tot_pkts, tot_insert, tot_read);
#endif

  } else
    return(0);
}

/* ********************************** */

static inline FlowSlot* get_insert_slot(struct ring_opt *pfr) {
#if defined(RING_DEBUG)
  printk("get_insert_slot(%d)\n", pfr->slots_info->insert_idx);
#endif

  if(pfr->ring_slots != NULL) {
    FlowSlot *slot = (FlowSlot*)&(pfr->ring_slots[pfr->slots_info->insert_idx
						  *pfr->slots_info->slot_len]);
    return(slot);
  } else
    return(NULL);
}

/* ********************************** */

static inline FlowSlot* get_remove_slot(struct ring_opt *pfr) {
#if defined(RING_DEBUG)
  printk("get_remove_slot(%d)\n", pfr->slots_info->remove_idx);
#endif

  if(pfr->ring_slots != NULL)
    return((FlowSlot*)&(pfr->ring_slots[pfr->slots_info->remove_idx*
					pfr->slots_info->slot_len]));
  else
    return(NULL);
}

/* ******************************************************* */

static int parse_pkt(struct sk_buff *skb, u_int16_t skb_displ,
		     u_int8_t *l3_proto, u_int16_t *eth_type,
		     u_int16_t *l3_offset, u_int16_t *l4_offset,
		     u_int16_t *vlan_id, u_int32_t *ipv4_src,
		     u_int32_t *ipv4_dst,
		     u_int16_t *l4_src_port, u_int16_t *l4_dst_port,
		     u_int16_t *payload_offset) {
  struct iphdr *ip;
  struct ethhdr *eh = (struct ethhdr*)(skb->data-skb_displ);
  u_int16_t displ;

  *l3_offset = *l4_offset = *l3_proto = *payload_offset = 0;
  *eth_type = ntohs(eh->h_proto);

  if(*eth_type == 0x8100 /* 802.1q (VLAN) */) {
    (*vlan_id) = (skb->data[14] & 15)*256 + skb->data[15];
    *eth_type = (skb->data[16])*256 + skb->data[17];
    displ = 4;
  } else {
    displ = 0;
    (*vlan_id) = (u_int16_t)-1;
  }

  if(*eth_type == 0x0800 /* IP */) {
    *l3_offset = displ+sizeof(struct ethhdr);
    ip = (struct iphdr*)(skb->data-skb_displ+(*l3_offset));

    *ipv4_src = ntohl(ip->saddr), *ipv4_dst = ntohl(ip->daddr), *l3_proto = ip->protocol;

    if((ip->protocol == IPPROTO_TCP) || (ip->protocol == IPPROTO_UDP)) {
      *l4_offset = (*l3_offset)+(ip->ihl*4);

      if(ip->protocol == IPPROTO_TCP) {
	struct tcphdr *tcp = (struct tcphdr*)(skb->data-skb_displ+(*l4_offset));
	*l4_src_port = ntohs(tcp->source), *l4_dst_port = ntohs(tcp->dest);
	*payload_offset = (*l4_offset)+(tcp->doff * 4);
      } else if(ip->protocol == IPPROTO_UDP) {
	struct udphdr *udp = (struct udphdr*)(skb->data-skb_displ+(*l4_offset));
	*l4_src_port = ntohs(udp->source), *l4_dst_port = ntohs(udp->dest);
	*payload_offset = (*l4_offset)+sizeof(struct udphdr);
      } else
	*payload_offset = (*l4_offset);
    } else
      *l4_src_port = *l4_dst_port = 0;

    return(1); /* IP */
  } /* TODO: handle IPv6 */

  return(0); /* No IP */
}

/* **************************************************************** */

static void reset_bitmask(bitmask_selector *selector)
{
  memset((char*)selector->bits_memory, 0, selector->num_bits/8);

  while(selector->clashes != NULL) {
    bitmask_counter_list *next = selector->clashes->next;
    kfree(selector->clashes);
    selector->clashes = next;
  }
}

/* **************************************************************** */

static void alloc_bitmask(u_int32_t tot_bits, bitmask_selector *selector)
{
  u_int tot_mem = tot_bits/8;

  if(tot_mem <= PAGE_SIZE)
    selector->order = 1;
  else {
    for(selector->order = 0; (PAGE_SIZE << selector->order) < tot_mem; selector->order++)
      ;
  }

  printk("BITMASK: [order=%d][tot_mem=%d]\n", selector->order, tot_mem);

  while((selector->bits_memory = __get_free_pages(GFP_ATOMIC, selector->order)) == 0)
    if(selector->order-- == 0)
      break;

  if(selector->order == 0) {
    printk("BITMASK: ERROR not enough memory for bitmask\n");
    selector->num_bits = 0;
    return;
  }

  tot_mem = PAGE_SIZE << selector->order;
  printk("BITMASK: succesfully allocated [tot_mem=%d][order=%d]\n",
	 tot_mem, selector->order);

  selector->num_bits = tot_mem*8;
  selector->clashes = NULL;
  reset_bitmask(selector);
}

/* ********************************** */

static void free_bitmask(bitmask_selector *selector)
{
  if(selector->bits_memory > 0)
    free_pages(selector->bits_memory, selector->order);
}

/* ********************************** */

static void set_bit_bitmask(bitmask_selector *selector, u_int32_t the_bit) {
  u_int32_t idx = the_bit % selector->num_bits;

  if(BITMASK_ISSET(idx, selector)) {
    bitmask_counter_list *head = selector->clashes;

    printk("BITMASK: bit %u was already set\n", the_bit);

    while(head != NULL) {
      if(head->bit_id == the_bit) {
	head->bit_counter++;
	printk("BITMASK: bit %u is now set to %d\n", the_bit, head->bit_counter);
	return;
      }

      head = head->next;
    }

    head = kmalloc(sizeof(bitmask_counter_list), GFP_KERNEL);
    if(head) {
      head->bit_id = the_bit;
      head->bit_counter = 1 /* previous value */ + 1 /* the requested set */;
      head->next = selector->clashes;
      selector->clashes = head;
    } else {
      printk("BITMASK: not enough memory\n");
      return;
    }
  } else {
    BITMASK_SET(idx, selector);
    printk("BITMASK: bit %u is now set\n", the_bit);
  }
}

/* ********************************** */

static u_char is_set_bit_bitmask(bitmask_selector *selector, u_int32_t the_bit) {
  u_int32_t idx = the_bit % selector->num_bits;
  return(BITMASK_ISSET(idx, selector));
}

/* ********************************** */

static void clear_bit_bitmask(bitmask_selector *selector, u_int32_t the_bit) {
  u_int32_t idx = the_bit % selector->num_bits;

  if(!BITMASK_ISSET(idx, selector))
    printk("BITMASK: bit %u was not set\n", the_bit);
  else {
    bitmask_counter_list *head = selector->clashes, *prev = NULL;

    while(head != NULL) {
      if(head->bit_id == the_bit) {
	head->bit_counter--;

	printk("BITMASK: bit %u is now set to %d\n",
	       the_bit, head->bit_counter);

	if(head->bit_counter == 1) {
	  /* We can now delete this entry as '1' can be
	     accommodated into the bitmask */

	  if(prev == NULL)
	    selector->clashes = head->next;
	  else
	    prev->next = head->next;

	  kfree(head);
	}
	return;
      }

      prev = head; head = head->next;
    }

    BITMASK_CLR(idx, selector);
    printk("BITMASK: bit %u is now reset\n", the_bit);
  }
}

/* ********************************** */

/* Hash function */
static u_int32_t sdb_hash(u_int32_t value) {
  u_int32_t hash = 0, i;
  u_int8_t str[sizeof(value)];

  memcpy(str, &value, sizeof(value));

  for(i = 0; i < sizeof(value); i++) {
    hash = str[i] + (hash << 6) + (hash << 16) - hash;
  }

  return(hash);
}

/* ********************************** */

static void handle_bloom_filter_rule(struct ring_opt *pfr, char *buf) {
  u_int count;

  if(buf == NULL)
    return;
  else
    count = strlen(buf);

  printk("PF_RING: -> handle_bloom_filter_rule(%s)\n", buf);

  if((buf[count-1] == '\n') || (buf[count-1] == '\r')) buf[count-1] = '\0';

  if(count > 1) {
    u_int32_t the_bit;

    if(!strncmp(&buf[1], "vlan=", 5)) {
      sscanf(&buf[6], "%d", &the_bit);

      if(buf[0] == '+')
	set_bit_bitmask(&pfr->vlan_bitmask, the_bit), pfr->num_vlan_bitmask_add++;
      else
	clear_bit_bitmask(&pfr->vlan_bitmask, the_bit), pfr->num_vlan_bitmask_remove++;
    } else if(!strncmp(&buf[1], "mac=", 4)) {
      int a, b, c, d, e, f;

      if(sscanf(&buf[5], "%02x:%02x:%02x:%02x:%02x:%02x:",
		&a, &b, &c, &d, &e, &f) == 6) {
	u_int32_t mac_addr =  (a & 0xff) + (b & 0xff) + ((c & 0xff) << 24) + ((d & 0xff) << 16) + ((e & 0xff) << 8) + (f & 0xff);

	/* printk("PF_RING: -> [%u][%u][%u][%u][%u][%u] -> [%u]\n", a, b, c, d, e, f, mac_addr); */

	if(buf[0] == '+')
	  set_bit_bitmask(&pfr->mac_bitmask, mac_addr), pfr->num_mac_bitmask_add++;
	else
	  clear_bit_bitmask(&pfr->mac_bitmask, mac_addr), pfr->num_mac_bitmask_remove++;
      } else
	printk("PF_RING: -> Invalid MAC address '%s'\n", &buf[5]);
    } else if(!strncmp(&buf[1], "ip=", 3)) {
      int a, b, c, d;

      if(sscanf(&buf[4], "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
	u_int32_t ip_addr = ((a & 0xff) << 24) + ((b & 0xff) << 16) + ((c & 0xff) << 8) + (d & 0xff);

	if(buf[0] == '+')
	  set_bit_bitmask(&pfr->ip_bitmask, ip_addr), set_bit_bitmask(&pfr->ip_bitmask, sdb_hash(ip_addr)), pfr->num_ip_bitmask_add++;
	else
	  clear_bit_bitmask(&pfr->ip_bitmask, ip_addr), clear_bit_bitmask(&pfr->twin_ip_bitmask, sdb_hash(ip_addr)), pfr->num_ip_bitmask_remove++;
      } else
	printk("PF_RING: -> Invalid IP address '%s'\n", &buf[4]);
    } else if(!strncmp(&buf[1], "port=", 5)) {
      sscanf(&buf[6], "%d", &the_bit);

      if(buf[0] == '+')
	set_bit_bitmask(&pfr->port_bitmask, the_bit), set_bit_bitmask(&pfr->port_bitmask, sdb_hash(the_bit)), pfr->num_port_bitmask_add++;
      else
	clear_bit_bitmask(&pfr->port_bitmask, the_bit), clear_bit_bitmask(&pfr->twin_port_bitmask, sdb_hash(the_bit)), pfr->num_port_bitmask_remove++;
    } else if(!strncmp(&buf[1], "proto=", 6)) {
      if(!strncmp(&buf[7], "tcp", 3))       the_bit = 6;
      else if(!strncmp(&buf[7], "udp", 3))  the_bit = 17;
      else if(!strncmp(&buf[7], "icmp", 4)) the_bit = 1;
      else sscanf(&buf[7], "%d", &the_bit);

      if(buf[0] == '+')
	set_bit_bitmask(&pfr->proto_bitmask, the_bit);
      else
	clear_bit_bitmask(&pfr->proto_bitmask, the_bit);
    } else
      printk("PF_RING: -> Unknown rule type '%s'\n", buf);
  }
}

/* ********************************** */

static void reset_bloom_filters(struct ring_opt *pfr) {
  reset_bitmask(&pfr->mac_bitmask);
  reset_bitmask(&pfr->vlan_bitmask);
  reset_bitmask(&pfr->ip_bitmask); reset_bitmask(&pfr->twin_ip_bitmask);
  reset_bitmask(&pfr->port_bitmask); reset_bitmask(&pfr->twin_port_bitmask);
  reset_bitmask(&pfr->proto_bitmask);

  pfr->num_mac_bitmask_add   = pfr->num_mac_bitmask_remove   = 0;
  pfr->num_vlan_bitmask_add  = pfr->num_vlan_bitmask_remove  = 0;
  pfr->num_ip_bitmask_add    = pfr->num_ip_bitmask_remove    = 0;
  pfr->num_port_bitmask_add  = pfr->num_port_bitmask_remove  = 0;
  pfr->num_proto_bitmask_add = pfr->num_proto_bitmask_remove = 0;

  printk("PF_RING: rules have been reset\n");
}

/* ********************************** */

static void init_blooms(struct ring_opt *pfr) {
  alloc_bitmask(4096,  &pfr->mac_bitmask);
  alloc_bitmask(4096,  &pfr->vlan_bitmask);
  alloc_bitmask(32768, &pfr->ip_bitmask); alloc_bitmask(32768, &pfr->twin_ip_bitmask);
  alloc_bitmask(4096,  &pfr->port_bitmask); alloc_bitmask(4096,  &pfr->twin_port_bitmask);
  alloc_bitmask(4096,  &pfr->proto_bitmask);

  pfr->num_mac_bitmask_add   = pfr->num_mac_bitmask_remove = 0;
  pfr->num_vlan_bitmask_add  = pfr->num_vlan_bitmask_remove = 0;
  pfr->num_ip_bitmask_add    = pfr->num_ip_bitmask_remove   = 0;
  pfr->num_port_bitmask_add  = pfr->num_port_bitmask_remove = 0;
  pfr->num_proto_bitmask_add = pfr->num_proto_bitmask_remove = 0;

  reset_bloom_filters(pfr);
}

/* ********************************** */

inline int MatchFound (void* id, int index, void *data) { return(0); }

/* ********************************** */

static void add_skb_to_ring(struct sk_buff *skb,
			    struct ring_opt *pfr,
			    u_char recv_packet,
			    u_char real_skb /* 1=skb 0=faked skb */) {
  FlowSlot *theSlot;
  int idx, displ, fwd_pkt = 0;

  if(recv_packet) {
    /* Hack for identifying a packet received by the e1000 */
    if(real_skb) {
      displ = SKB_DISPLACEMENT;
    } else
      displ = 0; /* Received by the e1000 wrapper */
  } else
    displ = 0;

  write_lock(&pfr->ring_index_lock);
  pfr->slots_info->tot_pkts++;
  write_unlock(&pfr->ring_index_lock);

  /* BPF Filtering (from af_packet.c) */
  if(pfr->bpfFilter != NULL) {
    unsigned res = 1, len;

    len = skb->len-skb->data_len;

    write_lock(&pfr->ring_index_lock);
    skb->data -= displ;
    res = sk_run_filter(skb, pfr->bpfFilter->insns, pfr->bpfFilter->len);
    skb->data += displ;
    write_unlock(&pfr->ring_index_lock);

    if(res == 0) {
      /* Filter failed */

#if defined(RING_DEBUG)
      printk("add_skb_to_ring(skb): Filter failed [len=%d][tot=%llu]"
	     "[insertIdx=%d][pkt_type=%d][cloned=%d]\n",
	     (int)skb->len, pfr->slots_info->tot_pkts,
	     pfr->slots_info->insert_idx,
	     skb->pkt_type, skb->cloned);
#endif

      return;
    }
  }

  /* ************************** */

  if(pfr->sample_rate > 1) {
    if(pfr->pktToSample == 0) {
      write_lock(&pfr->ring_index_lock);
      pfr->pktToSample = pfr->sample_rate;
      write_unlock(&pfr->ring_index_lock);
    } else {
      write_lock(&pfr->ring_index_lock);
      pfr->pktToSample--;
      write_unlock(&pfr->ring_index_lock);

#if defined(RING_DEBUG)
      printk("add_skb_to_ring(skb): sampled packet [len=%d]"
	     "[tot=%llu][insertIdx=%d][pkt_type=%d][cloned=%d]\n",
	     (int)skb->len, pfr->slots_info->tot_pkts,
	     pfr->slots_info->insert_idx,
	     skb->pkt_type, skb->cloned);
#endif
      return;
    }
  }

  /* ************************************* */

  if((pfr->reflector_dev != NULL)
     && (!netif_queue_stopped(pfr->reflector_dev))) {
    int cpu = smp_processor_id();

    /* increase reference counter so that this skb is not freed */
    atomic_inc(&skb->users);

    skb->data -= displ;

    /* send it */
    if (pfr->reflector_dev->xmit_lock_owner != cpu) {
      /* Patch below courtesy of Matthew J. Roth <mroth@imminc.com> */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18))
      spin_lock_bh(&pfr->reflector_dev->xmit_lock);
      pfr->reflector_dev->xmit_lock_owner = cpu;
      spin_unlock_bh(&pfr->reflector_dev->xmit_lock);
#else
      netif_tx_lock_bh(pfr->reflector_dev);
#endif
      if (pfr->reflector_dev->hard_start_xmit(skb, pfr->reflector_dev) == 0) {
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18))
        spin_lock_bh(&pfr->reflector_dev->xmit_lock);
	pfr->reflector_dev->xmit_lock_owner = -1;
	spin_unlock_bh(&pfr->reflector_dev->xmit_lock);
#else
	netif_tx_unlock_bh(pfr->reflector_dev);
#endif
	skb->data += displ;
#if defined(RING_DEBUG)
	printk("++ hard_start_xmit succeeded\n");
#endif
	return; /* OK */
      }

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18))
      spin_lock_bh(&pfr->reflector_dev->xmit_lock);
      pfr->reflector_dev->xmit_lock_owner = -1;
      spin_unlock_bh(&pfr->reflector_dev->xmit_lock);
#else
      netif_tx_unlock_bh(pfr->reflector_dev);
#endif
    }

#if defined(RING_DEBUG)
    printk("++ hard_start_xmit failed\n");
#endif
    skb->data += displ;
    return; /* -ENETDOWN */
  }

  /* ************************************* */

#if defined(RING_DEBUG)
  printk("add_skb_to_ring(skb) [len=%d][tot=%llu][insertIdx=%d]"
	 "[pkt_type=%d][cloned=%d]\n",
	 (int)skb->len, pfr->slots_info->tot_pkts,
	 pfr->slots_info->insert_idx,
	 skb->pkt_type, skb->cloned);
#endif

  idx = pfr->slots_info->insert_idx;
  theSlot = get_insert_slot(pfr);

  if((theSlot != NULL) && (theSlot->slot_state == 0)) {
    struct pcap_pkthdr *hdr;
    char *bucket;
    int is_ip_pkt, debug = 0;

    /* Update Index */
    idx++;

    bucket = &theSlot->bucket;
    hdr = (struct pcap_pkthdr*)bucket;

    /* BD - API changed for time keeping */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,14))
    if(skb->stamp.tv_sec == 0) do_gettimeofday(&skb->stamp);

    hdr->ts.tv_sec = skb->stamp.tv_sec, hdr->ts.tv_usec = skb->stamp.tv_usec;
#else
    if(skb->tstamp.off_sec == 0) __net_timestamp(skb);

    hdr->ts.tv_sec = skb->tstamp.off_sec, hdr->ts.tv_usec = skb->tstamp.off_usec;
#endif
    hdr->caplen    = skb->len+displ;

    if(hdr->caplen > pfr->slots_info->data_len)
      hdr->caplen = pfr->slots_info->data_len;

    hdr->len = skb->len+displ;

    /* Extensions */
    is_ip_pkt = parse_pkt(skb, displ,
			  &hdr->l3_proto,
			  &hdr->eth_type,
			  &hdr->l3_offset,
			  &hdr->l4_offset,
			  &hdr->vlan_id,
			  &hdr->ipv4_src,
			  &hdr->ipv4_dst,
			  &hdr->l4_src_port,
			  &hdr->l4_dst_port,
			  &hdr->payload_offset);

    if(is_ip_pkt && pfr->bitmask_enabled) {
      int vlan_match = 0;

      fwd_pkt = 0;

      if(debug) {
	if(is_ip_pkt)
	  printk(KERN_INFO "PF_RING: [proto=%d][vlan=%d][sport=%d][dport=%d][src=%u][dst=%u]\n",
		 hdr->l3_proto, hdr->vlan_id, hdr->l4_src_port, hdr->l4_dst_port, hdr->ipv4_src, hdr->ipv4_dst);
	else
	  printk(KERN_INFO "PF_RING: [proto=%d][vlan=%d]\n", hdr->l3_proto, hdr->vlan_id);
      }

      if(hdr->vlan_id != (u_int16_t)-1) {
	vlan_match = is_set_bit_bitmask(&pfr->vlan_bitmask, hdr->vlan_id);
      } else
	vlan_match = 1;

      if(vlan_match) {
	struct ethhdr *eh = (struct ethhdr*)(skb->data);
	u_int32_t src_mac =  (eh->h_source[0] & 0xff) + (eh->h_source[1] & 0xff) + ((eh->h_source[2] & 0xff) << 24)
	  + ((eh->h_source[3] & 0xff) << 16) + ((eh->h_source[4] & 0xff) << 8) + (eh->h_source[5] & 0xff);

	if(debug) printk(KERN_INFO "PF_RING: [src_mac=%u]\n", src_mac);

	fwd_pkt |= is_set_bit_bitmask(&pfr->mac_bitmask, src_mac);

	if(!fwd_pkt) {
	  u_int32_t dst_mac =  (eh->h_dest[0] & 0xff) + (eh->h_dest[1] & 0xff) + ((eh->h_dest[2] & 0xff) << 24)
	    + ((eh->h_dest[3] & 0xff) << 16) + ((eh->h_dest[4] & 0xff) << 8) + (eh->h_dest[5] & 0xff);

	  if(debug) printk(KERN_INFO "PF_RING: [dst_mac=%u]\n", dst_mac);

	  fwd_pkt |= is_set_bit_bitmask(&pfr->mac_bitmask, dst_mac);

	  if(is_ip_pkt && (!fwd_pkt)) {
	    fwd_pkt |= is_set_bit_bitmask(&pfr->ip_bitmask, hdr->ipv4_src);

	    if(!fwd_pkt) {
	      fwd_pkt |= is_set_bit_bitmask(&pfr->ip_bitmask, hdr->ipv4_dst);

	      if((!fwd_pkt) && ((hdr->l3_proto == IPPROTO_TCP)
				|| (hdr->l3_proto == IPPROTO_UDP))) {
		fwd_pkt |= is_set_bit_bitmask(&pfr->port_bitmask, hdr->l4_src_port);
		if(!fwd_pkt) fwd_pkt |= is_set_bit_bitmask(&pfr->port_bitmask, hdr->l4_dst_port);
	      }

	      if(!fwd_pkt) fwd_pkt |= is_set_bit_bitmask(&pfr->proto_bitmask, hdr->l3_proto);
	    }
	  }
	}
      }
    } else
      fwd_pkt = 1;

    if(fwd_pkt && (pfr->acsm != NULL)) {
      if((hdr->payload_offset > 0) && ((skb->len+skb->mac_len) > hdr->payload_offset)) {
	char *payload = (skb->data-displ+hdr->payload_offset);
	int payload_len = skb->len /* + skb->mac_len */ - hdr->payload_offset;

	if((payload_len > 0) 
	   && ((hdr->l4_src_port == 80) || (hdr->l4_dst_port == 80))) {
	  int rc;
	  
	  if(0) {
	    char buf[1500];
	    
	    memcpy(buf, payload, payload_len);
	    buf[payload_len] = '\0';
	    printk("[%s]\n", payload);
	  }

	  /* printk("Tring to match pattern [len=%d][%s]\n", payload_len, payload); */
	  rc = acsmSearch2(pfr->acsm, payload, payload_len, MatchFound, (void *)0) ? 1 : 0;

	  // printk("Match result: %d\n", fwd_pkt);
	  if(rc) {
	    printk("Pattern matched!\n");
	  } else {
	    fwd_pkt = 0;
	  }
	} else
	  fwd_pkt = 0;
      }	else
	fwd_pkt = 0;
    }

    if(fwd_pkt) {
      memcpy(&bucket[sizeof(struct pcap_pkthdr)], skb->data-displ, hdr->caplen);

#if defined(RING_DEBUG)
      {
	static unsigned int lastLoss = 0;

	if(pfr->slots_info->tot_lost
	   && (lastLoss != pfr->slots_info->tot_lost)) {
	  printk("add_skb_to_ring(%d): [data_len=%d]"
		 "[hdr.caplen=%d][skb->len=%d]"
		 "[pcap_pkthdr=%d][removeIdx=%d]"
		 "[loss=%lu][page=%u][slot=%u]\n",
		 idx-1, pfr->slots_info->data_len, hdr->caplen, skb->len,
		 sizeof(struct pcap_pkthdr),
		 pfr->slots_info->remove_idx,
		 (long unsigned int)pfr->slots_info->tot_lost,
		 pfr->insert_page_id, pfr->insert_slot_id);

	  lastLoss = pfr->slots_info->tot_lost;
	}
      }
#endif

      write_lock(&pfr->ring_index_lock);
      if(idx == pfr->slots_info->tot_slots)
	pfr->slots_info->insert_idx = 0;
      else
	pfr->slots_info->insert_idx = idx;

      pfr->slots_info->tot_insert++;
      theSlot->slot_state = 1;
      write_unlock(&pfr->ring_index_lock);
    }
  } else {
    write_lock(&pfr->ring_index_lock);
    pfr->slots_info->tot_lost++;
    write_unlock(&pfr->ring_index_lock);

#if defined(RING_DEBUG)
    printk("add_skb_to_ring(skb): packet lost [loss=%lu]"
	   "[removeIdx=%u][insertIdx=%u]\n",
	   (long unsigned int)pfr->slots_info->tot_lost,
	   pfr->slots_info->remove_idx, pfr->slots_info->insert_idx);
#endif
  }

  if(fwd_pkt) {

    /* wakeup in case of poll() */
    if(waitqueue_active(&pfr->ring_slots_waitqueue))
      wake_up_interruptible(&pfr->ring_slots_waitqueue);
  }
}

/* ********************************** */

static u_int hash_skb(struct ring_cluster *cluster_ptr,
		      struct sk_buff *skb, u_char recv_packet) {
  u_int idx;
  int displ;
  struct iphdr *ip;

  if(cluster_ptr->hashing_mode == cluster_round_robin) {
    idx = cluster_ptr->hashing_id++;
  } else {
    /* Per-flow clustering */
    if(skb->len > sizeof(struct iphdr)+sizeof(struct tcphdr)) {
      if(recv_packet)
	displ = 0;
      else
	displ = SKB_DISPLACEMENT;

      /*
	skb->data+displ

	Always points to to the IP part of the packet
      */

      ip = (struct iphdr*)(skb->data+displ);

      idx = ip->saddr+ip->daddr+ip->protocol;

      if(ip->protocol == IPPROTO_TCP) {
	struct tcphdr *tcp = (struct tcphdr*)(skb->data+displ
					      +sizeof(struct iphdr));
	idx += tcp->source+tcp->dest;
      } else if(ip->protocol == IPPROTO_UDP) {
	struct udphdr *udp = (struct udphdr*)(skb->data+displ
					      +sizeof(struct iphdr));
	idx += udp->source+udp->dest;
      }
    } else
      idx = skb->len;
  }

  return(idx % cluster_ptr->num_cluster_elements);
}

/* ********************************** */

static int skb_ring_handler(struct sk_buff *skb,
			    u_char recv_packet,
			    u_char real_skb /* 1=skb 0=faked skb */) {
  struct sock *skElement;
  int rc = 0;
  struct list_head *ptr;
  struct ring_cluster *cluster_ptr;

#ifdef PROFILING
  uint64_t rdt = _rdtsc(), rdt1, rdt2;
#endif

  if((!skb) /* Invalid skb */
     || ((!enable_tx_capture) && (!recv_packet))) {
    /*
      An outgoing packet is about to be sent out
      but we decided not to handle transmitted
      packets.
    */
    return(0);
  }

#if defined(RING_DEBUG)
  if(0) {
    printk("skb_ring_handler() [len=%d][dev=%s]\n", skb->len,
	   skb->dev->name == NULL ? "<NULL>" : skb->dev->name);
  }
#endif

#ifdef PROFILING
  rdt1 = _rdtsc();
#endif

  /* [1] Check unclustered sockets */
  for (ptr = ring_table.next; ptr != &ring_table; ptr = ptr->next) {
    struct ring_opt *pfr;
    struct ring_element *entry;

    entry = list_entry(ptr, struct ring_element, list);

    read_lock(&ring_mgmt_lock);
    skElement = entry->sk;
    pfr = ring_sk(skElement);
    read_unlock(&ring_mgmt_lock);

    if((pfr != NULL)
       && (pfr->cluster_id == 0 /* No cluster */)
       && (pfr->ring_slots != NULL)
       && ((pfr->ring_netdev == skb->dev) || ((skb->dev->flags & IFF_SLAVE) && pfr->ring_netdev == skb->dev->master))) {
      /* We've found the ring where the packet can be stored */
      read_lock(&ring_mgmt_lock);
      add_skb_to_ring(skb, pfr, recv_packet, real_skb);
      read_unlock(&ring_mgmt_lock);

      rc = 1; /* Ring found: we've done our job */
    }
  }

  /* [2] Check socket clusters */
  cluster_ptr = ring_cluster_list;

  while(cluster_ptr != NULL) {
    struct ring_opt *pfr;

    if(cluster_ptr->num_cluster_elements > 0) {
      u_int skb_hash = hash_skb(cluster_ptr, skb, recv_packet);

      read_lock(&ring_mgmt_lock);
      skElement = cluster_ptr->sk[skb_hash];
      read_unlock(&ring_mgmt_lock);

      if(skElement != NULL) {
	pfr = ring_sk(skElement);

	if((pfr != NULL)
	   && (pfr->ring_slots != NULL)
	   && ((pfr->ring_netdev == skb->dev) || ((skb->dev->flags & IFF_SLAVE) && pfr->ring_netdev == skb->dev->master))) {
	  /* We've found the ring where the packet can be stored */
          read_lock(&ring_mgmt_lock);
	  add_skb_to_ring(skb, pfr, recv_packet, real_skb);
          read_unlock(&ring_mgmt_lock);

	  rc = 1; /* Ring found: we've done our job */
	}
      }
    }

    cluster_ptr = cluster_ptr->next;
  }

#ifdef PROFILING
  rdt1 = _rdtsc()-rdt1;
#endif

#ifdef PROFILING
  rdt2 = _rdtsc();
#endif

  if(transparent_mode) rc = 0;

  if((rc != 0) && real_skb)
    dev_kfree_skb(skb); /* Free the skb */

#ifdef PROFILING
  rdt2 = _rdtsc()-rdt2;
  rdt = _rdtsc()-rdt;

#if defined(RING_DEBUG)
  printk("# cycles: %d [lock costed %d %d%%][free costed %d %d%%]\n",
	 (int)rdt, rdt-rdt1,
	 (int)((float)((rdt-rdt1)*100)/(float)rdt),
	 rdt2,
	 (int)((float)(rdt2*100)/(float)rdt));
#endif
#endif

  return(rc); /*  0 = packet not handled */
}

/* ********************************** */

struct sk_buff skb;

static int buffer_ring_handler(struct net_device *dev,
			       char *data, int len) {

#if defined(RING_DEBUG)
  printk("buffer_ring_handler: [dev=%s][len=%d]\n",
	 dev->name == NULL ? "<NULL>" : dev->name, len);
#endif

  /* BD - API changed for time keeping */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,14))
  skb.dev = dev, skb.len = len, skb.data = data,
    skb.data_len = len, skb.stamp.tv_sec = 0; /* Calculate the time */
#else
  skb.dev = dev, skb.len = len, skb.data = data,
    skb.data_len = len, skb.tstamp.off_sec = 0; /* Calculate the time */
#endif

  skb_ring_handler(&skb, 1, 0 /* fake skb */);

  return(0);
}

/* ********************************** */

static int ring_create(struct socket *sock, int protocol) {
  struct sock *sk;
  struct ring_opt *pfr;
  int err;

#if defined(RING_DEBUG)
  printk("RING: ring_create()\n");
#endif

  /* Are you root, superuser or so ? */
  if(!capable(CAP_NET_ADMIN))
    return -EPERM;

  if(sock->type != SOCK_RAW)
    return -ESOCKTNOSUPPORT;

  if(protocol != htons(ETH_P_ALL))
    return -EPROTONOSUPPORT;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
  MOD_INC_USE_COUNT;
#endif

  err = -ENOMEM;

  // BD: -- broke this out to keep it more simple and clear as to what the
  // options are.
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
  sk = sk_alloc(PF_RING, GFP_KERNEL, 1, NULL);
#else
  // BD: API changed in 2.6.12, ref:
  // http://svn.clkao.org/svnweb/linux/revision/?rev=28201
  sk = sk_alloc(PF_RING, GFP_ATOMIC, &ring_proto, 1);
#endif
#else
  /* Kernel 2.4 */
  sk = sk_alloc(PF_RING, GFP_KERNEL, 1);
#endif

  if (sk == NULL)
    goto out;

  sock->ops = &ring_ops;
  sock_init_data(sock, sk);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
  sk_set_owner(sk, THIS_MODULE);
#endif
#endif

  err = -ENOMEM;
  ring_sk(sk) = ring_sk_datatype(kmalloc(sizeof(*pfr), GFP_KERNEL));

  if (!(pfr = ring_sk(sk))) {
    sk_free(sk);
    goto out;
  }
  memset(pfr, 0, sizeof(*pfr));
  init_waitqueue_head(&pfr->ring_slots_waitqueue);
  pfr->ring_index_lock = RW_LOCK_UNLOCKED;
  atomic_set(&pfr->num_ring_slots_waiters, 0);
  init_blooms(pfr);
  pfr->acsm = NULL;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
  sk->sk_family       = PF_RING;
  sk->sk_destruct     = ring_sock_destruct;
#else
  sk->family          = PF_RING;
  sk->destruct        = ring_sock_destruct;
  sk->num             = protocol;
#endif

  ring_insert(sk);

#if defined(RING_DEBUG)
  printk("RING: ring_create() - created\n");
#endif

  return(0);
 out:
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
  MOD_DEC_USE_COUNT;
#endif
  return err;
}

/* *********************************************** */

static int ring_release(struct socket *sock)
{
  struct sock *sk = sock->sk;
  struct ring_opt *pfr = ring_sk(sk);

  if(!sk)  return 0;

#if defined(RING_DEBUG)
  printk("RING: called ring_release\n");
#endif

#if defined(RING_DEBUG)
  printk("RING: ring_release entered\n");
#endif

  /*
    The calls below must be placed outside the
    write_lock_irq...write_unlock_irq block.
  */
  sock_orphan(sk);
  ring_proc_remove(ring_sk(sk));

  write_lock_irq(&ring_mgmt_lock);
  ring_remove(sk);
  sock->sk = NULL;

  /* Free the ring buffer */
  if(pfr->ring_memory) {
    struct page *page, *page_end;

    page_end = virt_to_page(pfr->ring_memory + (PAGE_SIZE << pfr->order) - 1);
    for(page = virt_to_page(pfr->ring_memory); page <= page_end; page++)
      ClearPageReserved(page);

    free_pages(pfr->ring_memory, pfr->order);
  }

  free_bitmask(&pfr->mac_bitmask);
  free_bitmask(&pfr->vlan_bitmask);
  free_bitmask(&pfr->ip_bitmask); free_bitmask(&pfr->twin_ip_bitmask);
  free_bitmask(&pfr->port_bitmask); free_bitmask(&pfr->twin_port_bitmask);
  free_bitmask(&pfr->proto_bitmask);

  if(pfr->acsm != NULL) acsmFree2(pfr->acsm);

  kfree(pfr);
  ring_sk(sk) = NULL;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
  skb_queue_purge(&sk->sk_write_queue);
#endif

  sock_put(sk);
  write_unlock_irq(&ring_mgmt_lock);

#if defined(RING_DEBUG)
  printk("RING: ring_release leaving\n");
#endif

  return 0;
}

/* ********************************** */
/*
 * We create a ring for this socket and bind it to the specified device
 */
static int packet_ring_bind(struct sock *sk, struct net_device *dev)
{
  u_int the_slot_len;
  u_int32_t tot_mem;
  struct ring_opt *pfr = ring_sk(sk);
  struct page *page, *page_end;

  if(!dev) return(-1);

#if defined(RING_DEBUG)
  printk("RING: packet_ring_bind(%s) called\n", dev->name);
#endif

  /* **********************************************

  *************************************
  *                                   *
  *        FlowSlotInfo               *
  *                                   *
  ************************************* <-+
  *        FlowSlot                   *   |
  *************************************   |
  *        FlowSlot                   *   |
  *************************************   +- num_slots
  *        FlowSlot                   *   |
  *************************************   |
  *        FlowSlot                   *   |
  ************************************* <-+

  ********************************************** */

  the_slot_len = sizeof(u_char)    /* flowSlot.slot_state */
#ifdef RING_MAGIC
    + sizeof(u_char)
#endif
    + sizeof(struct pcap_pkthdr)
    + bucket_len      /* flowSlot.bucket */;

  tot_mem = sizeof(FlowSlotInfo) + num_slots*the_slot_len;

  /*
    Calculate the value of the order parameter used later.
    See http://www.linuxjournal.com/article.php?sid=1133
  */
  for(pfr->order = 0;(PAGE_SIZE << pfr->order) < tot_mem; pfr->order++)  ;

  /*
    We now try to allocate the memory as required. If we fail
    we try to allocate a smaller amount or memory (hence a
    smaller ring).
  */
  while((pfr->ring_memory = __get_free_pages(GFP_ATOMIC, pfr->order)) == 0)
    if(pfr->order-- == 0)
      break;

  if(pfr->order == 0) {
    printk("RING: ERROR not enough memory for ring\n");
    return(-1);
  } else {
    printk("RING: succesfully allocated %lu KB [tot_mem=%d][order=%ld]\n",
	   PAGE_SIZE >> (10 - pfr->order), tot_mem, pfr->order);
  }

  tot_mem = PAGE_SIZE << pfr->order;
  memset((char*)pfr->ring_memory, 0, tot_mem);

  /* Now we need to reserve the pages */
  page_end = virt_to_page(pfr->ring_memory + (PAGE_SIZE << pfr->order) - 1);
  for(page = virt_to_page(pfr->ring_memory); page <= page_end; page++)
    SetPageReserved(page);

  pfr->slots_info = (FlowSlotInfo*)pfr->ring_memory;
  pfr->ring_slots = (char*)(pfr->ring_memory+sizeof(FlowSlotInfo));

  pfr->slots_info->version     = RING_FLOWSLOT_VERSION;
  pfr->slots_info->slot_len    = the_slot_len;
  pfr->slots_info->data_len    = bucket_len;
  pfr->slots_info->tot_slots   = (tot_mem-sizeof(FlowSlotInfo))/the_slot_len;
  pfr->slots_info->tot_mem     = tot_mem;
  pfr->slots_info->sample_rate = sample_rate;

  printk("RING: allocated %d slots [slot_len=%d][tot_mem=%u]\n",
	 pfr->slots_info->tot_slots, pfr->slots_info->slot_len,
	 pfr->slots_info->tot_mem);

#ifdef RING_MAGIC
  {
    int i;

    for(i=0; i<pfr->slots_info->tot_slots; i++) {
      unsigned long idx = i*pfr->slots_info->slot_len;
      FlowSlot *slot = (FlowSlot*)&pfr->ring_slots[idx];
      slot->magic = RING_MAGIC_VALUE; slot->slot_state = 0;
    }
  }
#endif

  pfr->insert_page_id = 1, pfr->insert_slot_id = 0;

  /*
    IMPORTANT
    Leave this statement here as last one. In fact when
    the ring_netdev != NULL the socket is ready to be used.
  */
  pfr->ring_netdev = dev;

  return(0);
}

/* ************************************* */

/* Bind to a device */
static int ring_bind(struct socket *sock,
		     struct sockaddr *sa, int addr_len)
{
  struct sock *sk=sock->sk;
  struct net_device *dev = NULL;

#if defined(RING_DEBUG)
  printk("RING: ring_bind() called\n");
#endif

  /*
   *	Check legality
   */
  if (addr_len != sizeof(struct sockaddr))
    return -EINVAL;
  if (sa->sa_family != PF_RING)
    return -EINVAL;

  /* Safety check: add trailing zero if missing */
  sa->sa_data[sizeof(sa->sa_data)-1] = '\0';

#if defined(RING_DEBUG)
  printk("RING: searching device %s\n", sa->sa_data);
#endif

  if((dev = __dev_get_by_name(sa->sa_data)) == NULL) {
#if defined(RING_DEBUG)
    printk("RING: search failed\n");
#endif
    return(-EINVAL);
  } else
    return(packet_ring_bind(sk, dev));
}

/* ************************************* */

static int ring_mmap(struct file *file,
		     struct socket *sock,
		     struct vm_area_struct *vma)
{
  struct sock *sk = sock->sk;
  struct ring_opt *pfr = ring_sk(sk);
  unsigned long size, start;
  u_int pagesToMap;
  char *ptr;

#if defined(RING_DEBUG)
  printk("RING: ring_mmap() called\n");
#endif

  if(pfr->ring_memory == 0) {
#if defined(RING_DEBUG)
    printk("RING: ring_mmap() failed: mapping area to an unbound socket\n");
#endif
    return -EINVAL;
  }

  size = (unsigned long)(vma->vm_end-vma->vm_start);

  if(size % PAGE_SIZE) {
#if defined(RING_DEBUG)
    printk("RING: ring_mmap() failed: len is not multiple of PAGE_SIZE\n");
#endif
    return(-EINVAL);
  }

  /* if userspace tries to mmap beyond end of our buffer, fail */
  if(size > pfr->slots_info->tot_mem) {
#if defined(RING_DEBUG)
    printk("proc_mmap() failed: area too large [%ld > %d]\n", size, pfr->slots_info->tot_mem);
#endif
    return(-EINVAL);
  }

  pagesToMap = size/PAGE_SIZE;

#if defined(RING_DEBUG)
  printk("RING: ring_mmap() called. %d pages to map\n", pagesToMap);
#endif

#if defined(RING_DEBUG)
  printk("RING: mmap [slot_len=%d][tot_slots=%d] for ring on device %s\n",
	 pfr->slots_info->slot_len, pfr->slots_info->tot_slots,
	 pfr->ring_netdev->name);
#endif

  /* we do not want to have this area swapped out, lock it */
  vma->vm_flags |= VM_LOCKED;
  start = vma->vm_start;

  /* Ring slots start from page 1 (page 0 is reserved for FlowSlotInfo) */
  ptr = (char*)(start+PAGE_SIZE);

  if(remap_page_range(
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
		      vma,
#endif
		      start,
		      __pa(pfr->ring_memory),
		      PAGE_SIZE*pagesToMap, vma->vm_page_prot)) {
#if defined(RING_DEBUG)
    printk("remap_page_range() failed\n");
#endif
    return(-EAGAIN);
  }

#if defined(RING_DEBUG)
  printk("proc_mmap(pagesToMap=%d): success.\n", pagesToMap);
#endif

  return 0;
}

/* ************************************* */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
static int ring_recvmsg(struct kiocb *iocb, struct socket *sock,
			struct msghdr *msg, size_t len, int flags)
#else
  static int ring_recvmsg(struct socket *sock, struct msghdr *msg, int len,
			  int flags, struct scm_cookie *scm)
#endif
{
  FlowSlot* slot;
  struct ring_opt *pfr = ring_sk(sock->sk);
  u_int32_t queued_pkts, num_loops = 0;

#if defined(RING_DEBUG)
  printk("ring_recvmsg called\n");
#endif

  slot = get_remove_slot(pfr);

  while((queued_pkts = num_queued_pkts(pfr)) < MIN_QUEUED_PKTS) {
    wait_event_interruptible(pfr->ring_slots_waitqueue, 1);

#if defined(RING_DEBUG)
    printk("-> ring_recvmsg returning %d [queued_pkts=%d][num_loops=%d]\n",
	   slot->slot_state, queued_pkts, num_loops);
#endif

    if(queued_pkts > 0) {
      if(num_loops++ > MAX_QUEUE_LOOPS)
	break;
    }
  }

#if defined(RING_DEBUG)
  if(slot != NULL)
    printk("ring_recvmsg is returning [queued_pkts=%d][num_loops=%d]\n",
	   queued_pkts, num_loops);
#endif

  return(queued_pkts);
}

/* ************************************* */

unsigned int ring_poll(struct file * file,
		       struct socket *sock, poll_table *wait)
{
  FlowSlot* slot;
  struct ring_opt *pfr = ring_sk(sock->sk);

#if defined(RING_DEBUG)
  printk("poll called\n");
#endif

  slot = get_remove_slot(pfr);

  if((slot != NULL) && (slot->slot_state == 0))
    poll_wait(file, &pfr->ring_slots_waitqueue, wait);

#if defined(RING_DEBUG)
  printk("poll returning %d\n", slot->slot_state);
#endif

  if((slot != NULL) && (slot->slot_state == 1))
    return(POLLIN | POLLRDNORM);
  else
    return(0);
}

/* ************************************* */

int add_to_cluster_list(struct ring_cluster *el,
			struct sock *sock) {

  if(el->num_cluster_elements == CLUSTER_LEN)
    return(-1); /* Cluster full */

  ring_sk_datatype(ring_sk(sock))->cluster_id = el->cluster_id;
  el->sk[el->num_cluster_elements] = sock;
  el->num_cluster_elements++;
  return(0);
}

/* ************************************* */

int remove_from_cluster_list(struct ring_cluster *el,
			     struct sock *sock) {
  int i, j;

  for(i=0; i<CLUSTER_LEN; i++)
    if(el->sk[i] == sock) {
      el->num_cluster_elements--;

      if(el->num_cluster_elements > 0) {
	/* The cluster contains other elements */
	for(j=i; j<CLUSTER_LEN-1; j++)
	  el->sk[j] = el->sk[j+1];

	el->sk[CLUSTER_LEN-1] = NULL;
      } else {
	/* Empty cluster */
	memset(el->sk, 0, sizeof(el->sk));
      }

      return(0);
    }

  return(-1); /* Not found */
}

/* ************************************* */

static int remove_from_cluster(struct sock *sock,
			       struct ring_opt *pfr)
{
  struct ring_cluster *el;

#if defined(RING_DEBUG)
  printk("--> remove_from_cluster(%d)\n", pfr->cluster_id);
#endif

  if(pfr->cluster_id == 0 /* 0 = No Cluster */)
    return(0); /* Noting to do */

  el = ring_cluster_list;

  while(el != NULL) {
    if(el->cluster_id == pfr->cluster_id) {
      return(remove_from_cluster_list(el, sock));
    } else
      el = el->next;
  }

  return(-EINVAL); /* Not found */
}

/* ************************************* */

static int add_to_cluster(struct sock *sock,
			  struct ring_opt *pfr,
			  u_short cluster_id)
{
  struct ring_cluster *el;

#ifndef RING_DEBUG
  printk("--> add_to_cluster(%d)\n", cluster_id);
#endif

  if(cluster_id == 0 /* 0 = No Cluster */) return(-EINVAL);

  if(pfr->cluster_id != 0)
    remove_from_cluster(sock, pfr);

  el = ring_cluster_list;

  while(el != NULL) {
    if(el->cluster_id == cluster_id) {
      return(add_to_cluster_list(el, sock));
    } else
      el = el->next;
  }

  /* There's no existing cluster. We need to create one */
  if((el = kmalloc(sizeof(struct ring_cluster), GFP_KERNEL)) == NULL)
    return(-ENOMEM);

  el->cluster_id = cluster_id;
  el->num_cluster_elements = 1;
  el->hashing_mode = cluster_per_flow; /* Default */
  el->hashing_id   = 0;

  memset(el->sk, 0, sizeof(el->sk));
  el->sk[0] = sock;
  el->next = ring_cluster_list;
  ring_cluster_list = el;
  pfr->cluster_id = cluster_id;

  return(0); /* 0 = OK */
}

/* ************************************* */

/* Code taken/inspired from core/sock.c */
static int ring_setsockopt(struct socket *sock,
			   int level, int optname,
			   char *optval, int optlen)
{
  struct ring_opt *pfr = ring_sk(sock->sk);
  int val, found, ret = 0;
  u_int cluster_id, do_enable;
  char devName[8], bloom_filter[256], aho_pattern[256];

  if(pfr == NULL) return(-EINVAL);

  if (get_user(val, (int *)optval))
    return -EFAULT;

  found = 1;

  switch(optname)
    {
    case SO_ATTACH_FILTER:
      ret = -EINVAL;
      if (optlen == sizeof(struct sock_fprog)) {
	unsigned int fsize;
	struct sock_fprog fprog;
	struct sk_filter *filter;

	ret = -EFAULT;

	/*
	  NOTE

	  Do not call copy_from_user within a held
	  splinlock (e.g. ring_mgmt_lock) as this caused
	  problems when certain debugging was enabled under
	  2.6.5 -- including hard lockups of the machine.
	*/
	if(copy_from_user(&fprog, optval, sizeof(fprog)))
	  break;

	fsize = sizeof(struct sock_filter) * fprog.len;
	filter = kmalloc(fsize, GFP_KERNEL);

	if(filter == NULL) {
	  ret = -ENOMEM;
	  break;
	}

	if(copy_from_user(filter->insns, fprog.filter, fsize))
	  break;

	filter->len = fprog.len;

	if(sk_chk_filter(filter->insns, filter->len) != 0) {
	  /* Bad filter specified */
	  kfree(filter);
	  pfr->bpfFilter = NULL;
	  break;
	}

	/* get the lock, set the filter, release the lock */
	write_lock(&ring_mgmt_lock);
	pfr->bpfFilter = filter;
	write_unlock(&ring_mgmt_lock);
      }
      ret = 0;
      break;

    case SO_DETACH_FILTER:
      write_lock(&ring_mgmt_lock);
      found = 1;
      if(pfr->bpfFilter != NULL) {
	kfree(pfr->bpfFilter);
	pfr->bpfFilter = NULL;
	write_unlock(&ring_mgmt_lock);
	break;
      }
      ret = -ENONET;
      break;

    case SO_ADD_TO_CLUSTER:
      if (optlen!=sizeof(val))
	return -EINVAL;

      if (copy_from_user(&cluster_id, optval, sizeof(cluster_id)))
	return -EFAULT;

      write_lock(&ring_mgmt_lock);
      ret = add_to_cluster(sock->sk, pfr, cluster_id);
      write_unlock(&ring_mgmt_lock);
      break;

    case SO_REMOVE_FROM_CLUSTER:
      write_lock(&ring_mgmt_lock);
      ret = remove_from_cluster(sock->sk, pfr);
      write_unlock(&ring_mgmt_lock);
      break;

    case SO_SET_REFLECTOR:
      if(optlen >= (sizeof(devName)-1))
	return -EINVAL;

      if(optlen > 0) {
	if(copy_from_user(devName, optval, optlen))
	  return -EFAULT;
      }

      devName[optlen] = '\0';

#if defined(RING_DEBUG)
      printk("+++ SO_SET_REFLECTOR(%s)\n", devName);
#endif

      write_lock(&ring_mgmt_lock);
      pfr->reflector_dev = dev_get_by_name(devName);
      write_unlock(&ring_mgmt_lock);

#if defined(RING_DEBUG)
      if(pfr->reflector_dev != NULL)
	printk("SO_SET_REFLECTOR(%s): succeded\n", devName);
      else
	printk("SO_SET_REFLECTOR(%s): device unknown\n", devName);
#endif
      break;

    case SO_SET_BLOOM:
      if(optlen >= (sizeof(bloom_filter)-1))
	return -EINVAL;

      if(optlen > 0) {
	if(copy_from_user(bloom_filter, optval, optlen))
	  return -EFAULT;
      }

      bloom_filter[optlen] = '\0';

      write_lock(&ring_mgmt_lock);
      handle_bloom_filter_rule(pfr, bloom_filter);
      write_unlock(&ring_mgmt_lock);
      break;

    case SO_SET_STRING:
      if(optlen >= (sizeof(aho_pattern)-1))
	return -EINVAL;

      if(optlen > 0) {
	if(copy_from_user(aho_pattern, optval, optlen))
	  return -EFAULT;
      }

      aho_pattern[optlen] = '\0';

      write_lock(&ring_mgmt_lock);
      if(pfr->acsm != NULL) acsmFree2(pfr->acsm);
      if(optlen > 0) {
#if 1
	if((pfr->acsm = acsmNew2()) != NULL) {
	  int nc=1 /* case sensitive */, i = 0;

	  pfr->acsm->acsmFormat = ACF_BANDED;
	  acsmAddPattern2(pfr->acsm,  (unsigned char*)aho_pattern,
			  (int)strlen(aho_pattern), nc, 0, 0,(void*)aho_pattern, i);
	  acsmCompile2(pfr->acsm);
	}
#else
	pfr->acsm =  kmalloc (10, GFP_KERNEL); /* TEST */
#endif
      }
      write_unlock(&ring_mgmt_lock);
      break;

    case SO_TOGGLE_BLOOM_STATE:
      if(optlen >= (sizeof(bloom_filter)-1))
	return -EINVAL;

      if(optlen > 0) {
	if(copy_from_user(&do_enable, optval, optlen))
	  return -EFAULT;
      }

      write_lock(&ring_mgmt_lock);
      if(do_enable)
	pfr->bitmask_enabled = 1;
      else
	pfr->bitmask_enabled = 0;
      write_unlock(&ring_mgmt_lock);
      printk("SO_TOGGLE_BLOOM_STATE: bloom bitmask %s\n",
	     pfr->bitmask_enabled ? "enabled" : "disabled");
      break;

    case SO_RESET_BLOOM_FILTERS:
      if(optlen >= (sizeof(bloom_filter)-1))
	return -EINVAL;

      if(optlen > 0) {
	if(copy_from_user(&do_enable, optval, optlen))
	  return -EFAULT;
      }

      write_lock(&ring_mgmt_lock);
      reset_bloom_filters(pfr);
      write_unlock(&ring_mgmt_lock);
      break;

    default:
      found = 0;
      break;
    }

  if(found)
    return(ret);
  else
    return(sock_setsockopt(sock, level, optname, optval, optlen));
}

/* ************************************* */

static int ring_ioctl(struct socket *sock,
		      unsigned int cmd, unsigned long arg)
{
  switch(cmd)
    {
#ifdef CONFIG_INET
    case SIOCGIFFLAGS:
    case SIOCSIFFLAGS:
    case SIOCGIFCONF:
    case SIOCGIFMETRIC:
    case SIOCSIFMETRIC:
    case SIOCGIFMEM:
    case SIOCSIFMEM:
    case SIOCGIFMTU:
    case SIOCSIFMTU:
    case SIOCSIFLINK:
    case SIOCGIFHWADDR:
    case SIOCSIFHWADDR:
    case SIOCSIFMAP:
    case SIOCGIFMAP:
    case SIOCSIFSLAVE:
    case SIOCGIFSLAVE:
    case SIOCGIFINDEX:
    case SIOCGIFNAME:
    case SIOCGIFCOUNT:
    case SIOCSIFHWBROADCAST:
      return(inet_dgram_ops.ioctl(sock, cmd, arg));
#endif

    default:
      return -ENOIOCTLCMD;
    }

  return 0;
}

/* ************************************* */

static struct proto_ops ring_ops = {
  .family	=	PF_RING,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
  .owner	=	THIS_MODULE,
#endif

  /* Operations that make no sense on ring sockets. */
  .connect	=	sock_no_connect,
  .socketpair	=	sock_no_socketpair,
  .accept	=	sock_no_accept,
  .getname	=	sock_no_getname,
  .listen	=	sock_no_listen,
  .shutdown	=	sock_no_shutdown,
  .sendpage	=	sock_no_sendpage,
  .sendmsg	=	sock_no_sendmsg,
  .getsockopt	=	sock_no_getsockopt,

  /* Now the operations that really occur. */
  .release	=	ring_release,
  .bind		=	ring_bind,
  .mmap		=	ring_mmap,
  .poll		=	ring_poll,
  .setsockopt	=	ring_setsockopt,
  .ioctl	=	ring_ioctl,
  .recvmsg	=	ring_recvmsg,
};

/* ************************************ */

static struct net_proto_family ring_family_ops = {
  .family	=	PF_RING,
  .create	=	ring_create,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
  .owner	=	THIS_MODULE,
#endif
};

// BD: API changed in 2.6.12, ref:
// http://svn.clkao.org/svnweb/linux/revision/?rev=28201
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,11))
static struct proto ring_proto = {
  .name		=	"PF_RING",
  .owner	=	THIS_MODULE,
  .obj_size	=	sizeof(struct sock),
};
#endif

/* ************************************ */

static void __exit ring_exit(void)
{
  struct list_head *ptr;
  struct ring_element *entry;

  for(ptr = ring_table.next; ptr != &ring_table; ptr = ptr->next) {
    entry = list_entry(ptr, struct ring_element, list);
    kfree(entry);
  }

  while(ring_cluster_list != NULL) {
    struct ring_cluster *next = ring_cluster_list->next;
    kfree(ring_cluster_list);
    ring_cluster_list = next;
  }

  set_skb_ring_handler(NULL);
  set_buffer_ring_handler(NULL);
  sock_unregister(PF_RING);
  ring_proc_term();
  printk("PF_RING shut down.\n");
}

/* ************************************ */

static int __init ring_init(void)
{
  printk("Welcome to PF_RING %s\n(C) 2004-07 L.Deri <deri@ntop.org>\n",
	 RING_VERSION);

  INIT_LIST_HEAD(&ring_table);
  ring_cluster_list = NULL;

  sock_register(&ring_family_ops);

  set_skb_ring_handler(skb_ring_handler);
  set_buffer_ring_handler(buffer_ring_handler);

  if(get_buffer_ring_handler() != buffer_ring_handler) {
    printk("PF_RING: set_buffer_ring_handler FAILED\n");

    set_skb_ring_handler(NULL);
    set_buffer_ring_handler(NULL);
    sock_unregister(PF_RING);
    return -1;
  } else {
    printk("PF_RING: bucket length    %d bytes\n", bucket_len);
    printk("PF_RING: ring slots       %d\n", num_slots);
    printk("PF_RING: sample rate      %d [1=no sampling]\n", sample_rate);
    printk("PF_RING: capture TX       %s\n",
	   enable_tx_capture ? "Yes [RX+TX]" : "No [RX only]");
    printk("PF_RING: transparent mode %s\n",
	   transparent_mode ? "Yes" : "No");

    printk("PF_RING initialized correctly.\n");

    ring_proc_init();
    return 0;
  }
}

module_init(ring_init);
module_exit(ring_exit);
MODULE_LICENSE("GPL");

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
MODULE_ALIAS_NETPROTO(PF_RING);
#endif
