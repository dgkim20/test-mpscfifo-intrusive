/*
 * This software is released into the public domain.
 *
 * A MpscFifo is a wait free/thread safe multi-producer
 * single consumer first in first out queue. This algorithm
 * is from Dimitry Vyukov's non intrusive MPSC code here:
 *   http://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue
 *
 * The fifo has a head and tail, the elements are added
 * to the head of the queue and removed from the tail.
 */

#define NDEBUG

#define _DEFAULT_SOURCE

#define DELAY 0

#ifndef NDEBUG
#define COUNT
#endif

#include "mpscfifo.h"
#include "dpf.h"

#include <sys/types.h>
#include <pthread.h>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>

/**
 * @see mpscfifo.h
 */
MpscFifo_t* initMpscFifo(MpscFifo_t* pQ) {
  DPF(LDR "initMpscFifo:*pQ=%p\n", ldr(), pQ);
  pQ->pHead = &pQ->stub;
  pQ->pTail = &pQ->stub;
  pQ->count = 0;
  pQ->msgs_processed = 0;
  pQ->stub.pNext = NULL;
  pQ->stub.pPool = NULL;
  pQ->stub.pRspQ = NULL;
  pQ->stub.arg1 = 11110001;
  pQ->stub.arg2 = 11110002;;
  return pQ;
}

/**
 * @see mpscfifo.h
 */
uint64_t deinitMpscFifo(MpscFifo_t* pQ) {
  uint64_t msgs_processed = pQ->msgs_processed;
  pQ->pHead = NULL;
  pQ->pTail = NULL;
  pQ->count = 0;
  pQ->msgs_processed = 0;
  DPF(LDR "deinitMpscFifo:-pQ=%p msgs_processed=%lu\n", ldr(), pQ, msgs_processed);
  return msgs_processed;
}

#if USE_ATOMIC_TYPES

/**
 * @see mpscifo.h
 */
void add(MpscFifo_t* pQ, Msg_t* pMsg) {
  DPF(LDR "add:+pQ=%p count=%d msg=%p pool=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
  DPF(LDR "add: pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
  pMsg->pNext = NULL;
  Msg_t* pPrev = __atomic_exchange_n((Msg_t**)&pQ->pHead, pMsg, __ATOMIC_ACQ_REL);
  // rmv will stall spinning if preempted at this critical spot

#if DELAY != 0
  usleep(DELAY);
#endif

  pPrev->pNext = pMsg;

#ifdef COUNT
  if (pMsg != &pQ->stub) {
    // Don't count adding the stub
    pQ->count += 1;
  }
#endif

  DPF(LDR "add: pQ=%p count=%d pPrev=%p pPrev->pNext=%p\n", ldr(), pQ, pQ->count, pPrev, pPrev->pNext);
  DPF(LDR "add: pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
  DPF(LDR "add:-pQ=%p count=%d msg=%p pool=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
}

#else

// Not using __ATOMIC types and streamlined with no comments or debug
void add(MpscFifo_t* pQ, Msg_t* pMsg) {
  // Not using __ATOMIC types and streamlined with no comments or debug
  pMsg->pNext = NULL;
  Msg_t* pPrev = __atomic_exchange_n(&pQ->pHead, pMsg, __ATOMIC_ACQ_REL);
  // rmv will stall spinning if preempted at this critical spot
  __atomic_store_n(&pPrev->pNext, pMsg, __ATOMIC_RELEASE);
}

#endif

#if USE_ATOMIC_TYPES

/**
 * @see mpscifo.h
 */
Msg_t* rmv_non_stalling(MpscFifo_t* pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  DPF(LDR "rmv_non_stalling:0+pQ=%p count=%d pTail=%p pNext=%p\n", ldr(), pQ, pQ->count, pTail, pNext);
  if (pTail == &pQ->stub) {
    // Nothing has been removed since Q was last empty
    if (pNext == NULL) {
      // Queue is empty
      DPF(LDR "rmv_non_stalling:1-NO MSGS pQ=%p count=%d msg=NULL\n", ldr(), pQ, pQ->count);
      return NULL;
    }
    // Advance tail to real "tail"
    pQ->pTail = pNext;
    pTail = pNext;
    pNext = pNext->pNext;
  }

  // We know there is at least 1 msg on the fifo and
  // pTail is pointing directly at that msg. Also pHead
  // is pointing at the same msg if there is only one msg
  // and otherwise there is more that one msg.
  //
  // Since this is non_stalling we may not be able to return
  // pTail if a producer was preempted so will return NULL
  // and the caller will need to call this or rmv to in
  // the future.
  if (pNext != NULL) {
    // Not empty and there are more elements
    pQ->pTail = pNext;
    DPF(LDR "rmv_non_stalling:2 got msg fifo has at least 1 more element pQ=%p count=%d\n", ldr(), pQ, pQ->count);
  } else {
    // We've reached the end of the fifo as known by pQ->pTail and
    // two conditions now exist either this is the last element
    // or the producer was preempted.
    if (pTail != pQ->pHead) {
      // First pHead != pTail then we know we were unlucky and a
      // producer was preempted. So we're not really empty but
      // since this is non-stalling we'll return NULL.
      DPF(LDR "rmv_non_stalling:3 got msg preempted not really 'empty' pQ=%p count=%d msg=NULL\n", ldr(), pQ, pQ->count);
      pTail = NULL;
    } else {
      // Second pHead == pTail then this is the last element
      // in which case we'll add stub to signify the Q if empty.
      // BUT now there are two more conditions.
      add(pQ, &pQ->stub);
      pNext = pTail->pNext;
      if (pNext != NULL) {
        pQ->pTail = pNext;
        DPF(LDR "rmv_non_stalling:4 got msg, Q is now EMPTY pQ=%p count=%d pTail=%p &pQ->stub=%p\n", ldr(), pQ, pQ->count, pQ->pTail, &pQ->stub);
      } else {
        // While we added our node another thread also added a node
        // and the fifo isn't complete (?). Therefore we can't
        // remove this element until the other producer finishes
        // and since this is non-stalling return NULL.
        pTail = NULL;
        DPF(LDR "rmv_non_stalling:5 got msg but preempted and not EMPTY pQ=%p count=%d\n", ldr(), pQ, pQ->count);
      }
    }
  }
  if (pTail != NULL) {
    pQ->msgs_processed += 1;
#ifdef COUNT
    pQ->count -= 1;
#endif
  }
  DPF(LDR "rmv_non_stalling:6-pQ=%p count=%d msg=%p\n", ldr(), pQ, pQ->count, pTail);
  return pTail;
}

#else

// Not using __ATOMIC types and streamlined with no comments or debug
Msg_t* rmv_non_stalling(MpscFifo_t* pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
  if (pTail == &pQ->stub) {
    if (pNext == NULL) {
      return NULL;
    }
    pQ->pTail = pNext;
    pTail = pNext;
    pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
  }

  if (pNext == NULL) {
    Msg_t* pHead = __atomic_load_n(&pQ->pHead, __ATOMIC_ACQUIRE);
    if (pTail != pHead) {
      pTail = NULL;
    } else {
      add(pQ, &pQ->stub);
      pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
      if (pNext == NULL) {
        pTail = NULL;
      }
    }
  }
  if (pTail != NULL) {
    pQ->msgs_processed += 1;
    pQ->pTail = pNext;
  }
  return pTail;
}

#endif

#if USE_ATOMIC_TYPES

/**
 * @see mpscifo.h
 */
Msg_t* rmv(MpscFifo_t* pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  DPF(LDR "rmv:0+pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
  if (pTail == &pQ->stub) {
    // Nothing has been removed since Q was last empty
    if (pNext == NULL) {
      // Queue is empty
      DPF(LDR "rmv:1-NO MSGS pQ=%p count=%d msg=NULL\n", ldr(), pQ, pQ->count);
      return NULL;
    }
    // Advance tail to real "tail"
    pQ->pTail = pNext;
    pTail = pNext;
    pNext = pTail->pNext;
    DPF(LDR "rmv:2 adv to real 'tail' pQ=%p count=%d pTail=%p pNext=%p\n", ldr(), pQ, pQ->count, pTail, pNext);
  }

  // We know there is at least 1 msg on the fifo and
  // pTail is pointing directly at that msg. Also pHead
  // is pointing at the same msg if there is only one msg
  // and otherwise there is more that one msg.
  DPF(LDR "rmv:3 pTail is the message we'll be returning pQ=%p count=%d pTail=%p pNext=%p\n", ldr(), pQ, pQ->count, pTail, pNext);
  if (pNext == NULL) {
    // We've reached the end of the fifo as known by pQ->pTail and
    // two conditions now exist, either this is the last element or
    // a producer was preempted and pNext hasn't yet been updated.
    // We can tell the difference by testing if pTail != pHead.
    DPF(LDR "rmv:3 pTail is last element or preempted pQ=%p count=%d pTail=%p pNext=%p\n", ldr(), pQ, pQ->count, pTail, pNext);
    if (pTail == pQ->pHead) {
      // Since they are equal we know that at the moment there
      // is only one msg on the fifo and that when we remove
      // it the fifo will be empty.
      //
      // Therefore we need to add the stub to the fifo for this case.
      // and then stall below incase we were racing with another
      // producer that may have been interrupted.
      DPF(LDR "rmv:4 pTail is only msg so add stub to pQ=%p pQ->count=%d pTail=%p pNext=%p\n", ldr(), pQ, pQ->count, pTail, pNext);
      add(pQ, &pQ->stub);
    } else {
      // pTail != pHead so there is more than one element but
      // since pNext == NULL we need to stall below waiting for
      // the producer to finish setting its pNext.
    }

    DPF(LDR "rmv:5 before stalling until pNext != NULL, pQ=%p count=%d pTail=%p pNext=%p\n", ldr(), pQ, pQ->count, pTail, pNext);
    // Stall waiting for producer to update pTail->pNext
    pNext = pTail->pNext;
    while (pNext == NULL) {
      sched_yield();
      pNext = pTail->pNext;
    }
    DPF(LDR "rmv:6  after  stalling now  pNext != NULL,  pQ=%p count=%d pTail=%p pNext=%p\n", ldr(), pQ, pQ->count, pTail, pNext);
  } else {
    // pNext != NULL and and there is more than one msg so we're golden.
  }

  // All paths above guranttee that pNext != NULL and we
  // can remove pTail by setting pQ->pTail = pNext.
  pQ->msgs_processed += 1;
  pQ->pTail = pNext;
#ifdef COUNT
  pQ->count -= 1; // Decrement number of msgs
#endif

  // Print state approximate state at this time, note these can change!!!
  DPF(LDR "rmv:7 got msg pQ=%p count=%d pQ->pHead=%p pQ->pHead->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext);
  DPF(LDR "rmv:8 got msg pQ=%p count=%d pQ->pTail=%p pQ->pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pTail, pQ->pTail->pNext);
  DPF(LDR "rmv:9-got msg pQ=%p count=%d msg=%p pool=%p pNext=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pTail, pTail->pPool, pTail->pNext, pTail->arg1, pTail->arg2);
  return pTail;
}

#else

Msg_t* rmv(MpscFifo_t* pQ) {
  // Not using __ATOMIC types and streamlined with no comments or debug
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
  if (pTail == &pQ->stub) {
    if (pNext == NULL) {
      return NULL;
    }
    pQ->pTail = pNext;
    pTail = pNext;
    pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
  }

  if (pNext == NULL) {
    Msg_t* pHead = __atomic_load_n(&pQ->pHead, __ATOMIC_ACQUIRE);
    if (pTail == pHead) {
      add(pQ, &pQ->stub);
    }

    pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
    while (pNext == NULL) {
      sched_yield();
      pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
    }
  }

  pQ->msgs_processed += 1;
  pQ->pTail = pNext;
  return pTail;
}

#endif

#if USE_ATOMIC_TYPES

/**
 * @see mpscifo.h
 */
Msg_t* rmv_no_dbg_on_empty(MpscFifo_t* pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if ((pNext == NULL) && (pTail == &pQ->stub)) {
    // Q is "empty"
    return NULL;
  } else {
    return rmv(pQ);
  }
}

#else

Msg_t* rmv_no_dbg_on_empty(MpscFifo_t* pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
  if ((pNext == NULL) && (pTail == &pQ->stub)) {
    // Q is "empty"
    return NULL;
  } else {
    return rmv(pQ);
  }
}

#endif

/**
 * @see mpscfifo.h
 */
void ret_msg(Msg_t* pMsg) {
  if ((pMsg != NULL) && (pMsg->pPool != NULL)) {
    add(pMsg->pPool, pMsg);
  } else {
    if (pMsg == NULL) {
      DPF(LDR "ret_msg:#No msg msg=%p\n", ldr(), pMsg);
    } else {
      DPF(LDR "ret_msg:#No pool msg=%p pool=%p arg1=%lu arg2=%lu\n",
          ldr(), pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
    }
  }
}

/**
 * @see mpscfifo.h
 */
void send_rsp_or_ret(Msg_t* msg, uint64_t arg1) {
  if (msg->pRspQ != NULL) {
    MpscFifo_t* pRspQ = msg->pRspQ;
    msg->pRspQ = NULL;
    msg->arg1 = arg1;
    DPF(LDR "send_rsp_or_ret: send pRspQ=%p msg=%p pool=%p arg1=%lu arg2=%lu\n",
        ldr(), pRspQ, msg, msg->pPool, msg->arg1, msg->arg2);
    add(pRspQ, msg);
  } else {
    DPF(LDR "send_rsp_or_ret: no RspQ ret msg=%p pool=%p arg1=%lu arg2=%lu\n",
        ldr(), msg, msg->pPool, msg->arg1, msg->arg2);
    ret_msg(msg);
  }
}
