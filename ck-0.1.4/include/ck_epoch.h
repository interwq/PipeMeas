/*
 * Copyright 2011 Samy Al Bahra.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _CK_EPOCH_H
#define _CK_EPOCH_H

/*
 * The implementation here is inspired from the work described in:
 *   Fraser, K. 2004. Practical Lock-Freedom. PhD Thesis, University
 *   of Cambridge Computing Laboratory.
 */

#include <ck_cc.h>
#include <ck_pr.h>
#include <ck_stack.h>
#include <stdbool.h>

/*
 * CK_EPOCH_LENGTH must be a power of 2.
 */
#ifndef CK_EPOCH_LENGTH
#define CK_EPOCH_LENGTH 4
#endif

struct ck_epoch_entry;
typedef struct ck_epoch_entry ck_epoch_entry_t;
typedef void (*ck_epoch_destructor_t)(ck_epoch_entry_t *);

struct ck_epoch_entry {
	ck_epoch_destructor_t destroy;
	ck_stack_entry_t stack_entry;
};

/*
 * Return pointer to ck_epoch_entry container object.
 */
#define CK_EPOCH_CONTAINER(T, M, N) CK_CC_CONTAINER(struct ck_epoch_entry, T, M, N)

struct ck_epoch;
struct ck_epoch_record {
	unsigned int active;
	unsigned int epoch;
	ck_stack_t pending[CK_EPOCH_LENGTH];
	unsigned int n_pending;
	unsigned int status;
	unsigned int delta;
	unsigned int n_peak;
	uint64_t     n_reclamations;
	struct ck_epoch *global;
	ck_stack_entry_t record_next;
} CK_CC_CACHELINE;
typedef struct ck_epoch_record ck_epoch_record_t;

struct ck_epoch {
	unsigned int epoch;
	char pad[CK_MD_CACHELINE - sizeof(unsigned int)];
	ck_stack_t records;
	unsigned int threshold;
	unsigned int n_free;
};
typedef struct ck_epoch ck_epoch_t;

CK_CC_INLINE static void
ck_epoch_read_begin(ck_epoch_record_t *record)
{

	/*
	 * Only observe new epoch if thread is not recursing into a read
	 * section.
	 */
	if (record->active == 0) {
		unsigned int g_epoch = ck_pr_load_uint(&record->global->epoch);
		g_epoch &= CK_EPOCH_LENGTH - 1;
		ck_pr_store_uint(&record->epoch, g_epoch);
	}

	ck_pr_store_uint(&record->active, record->active + 1);
	ck_pr_fence_store();
	return;
}

CK_CC_INLINE static void
ck_epoch_end(ck_epoch_record_t *record)
{

	ck_pr_fence_store();
	ck_pr_store_uint(&record->active, record->active - 1);
	return;
}

CK_CC_INLINE static void
ck_epoch_retire(ck_epoch_record_t *record, ck_epoch_entry_t *entry, ck_epoch_destructor_t destroy)
{

	entry->destroy = destroy;
	ck_stack_push_spnc(&record->pending[record->epoch], &entry->stack_entry);
	record->n_pending += 1;

	if (record->n_pending > record->n_peak)
		record->n_peak = record->n_pending;

	return;
}

void ck_epoch_init(ck_epoch_t *, unsigned int);
ck_epoch_record_t *ck_epoch_recycle(ck_epoch_t *);
void ck_epoch_register(ck_epoch_t *, ck_epoch_record_t *);
void ck_epoch_unregister(ck_epoch_record_t *);
void ck_epoch_tick(ck_epoch_t *, ck_epoch_record_t *);
bool ck_epoch_reclaim(ck_epoch_record_t *);
void ck_epoch_write_begin(ck_epoch_record_t *);
void ck_epoch_free(ck_epoch_record_t *, ck_epoch_entry_t *, ck_epoch_destructor_t);
void ck_epoch_purge(ck_epoch_record_t *record);

#endif /* _CK_EPOCH_H */
