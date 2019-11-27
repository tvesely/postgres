/*-------------------------------------------------------------------------
 *
 * zedstoream_inspect.c
 *	  Debugging functions, for viewing ZedStore page contents
 *
 * These should probably be moved to contrib/, but it's handy to have them
 * here during development.
 *
 * Example queries
 * ---------------
 *
 * How many pages of each type a table has?
 *
 * select count(*), pg_zs_page_type('t_zedstore', g)
 *   from generate_series(0, pg_table_size('t_zedstore') / 8192 - 1) g group by 2;
 *
 *  count | pg_zs_page_type
 * -------+-----------------
 *      1 | META
 *   3701 | BTREE
 *      6 | UNDO
 * (3 rows)
 *
 * Compression ratio of B-tree leaf pages (other pages are not compressed):
 *
 * select sum(uncompressedsz::numeric) / sum(totalsz) as compratio
 *   from pg_zs_btree_pages('t_zedstore') ;
 *      compratio
 * --------------------
 *  3.6623829559208134
 * (1 row)
 *
 * Per column compression ratio and number of pages:
 *
 * select attno, count(*), sum(uncompressedsz::numeric) / sum(totalsz) as
 * compratio from pg_zs_btree_pages('t_zedstore') group by attno order by
 * attno;
 *
 *  attno | count |       compratio
 * -------+-------+------------------------
 *      0 |   395 | 1.00000000000000000000
 *      1 |    56 |     1.0252948766341260
 *      2 |     3 |    38.7542309420398383
 * (3 rows)
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/zedstore/zedstoream_inspect.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "miscadmin.h"

#include "access/relscan.h"
#include "access/table.h"
#include "access/zedstore_internal.h"
#include "access/zedstore_undorec.h"
#include "commands/vacuum.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/rel.h"

Datum		pg_zs_page_type(PG_FUNCTION_ARGS);
Datum		pg_zs_undo_pages(PG_FUNCTION_ARGS);
Datum		pg_zs_btree_pages(PG_FUNCTION_ARGS);
Datum		pg_zs_toast_pages(PG_FUNCTION_ARGS);
Datum		pg_zs_meta_page(PG_FUNCTION_ARGS);

Datum
pg_zs_page_type(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	uint64		pageno = PG_GETARG_INT64(1);
	Relation	rel;
	uint16		zs_page_id;
	Buffer		buf;
	Page		page;
	char	   *result;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use zedstore inspection functions"))));

	rel = table_open(relid, AccessShareLock);

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	buf = ReadBuffer(rel, pageno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	zs_page_id = *((uint16 *) ((char *) page + BLCKSZ - sizeof(uint16)));

	UnlockReleaseBuffer(buf);

	table_close(rel, AccessShareLock);

	switch (zs_page_id)
	{
		case ZS_META_PAGE_ID:
			result = "META";
			break;
		case ZS_BTREE_PAGE_ID:
			result = "BTREE";
			break;
		case ZS_UNDO_PAGE_ID:
			result = "UNDO";
			break;
		case ZS_TOAST_PAGE_ID:
			result = "TOAST";
			break;
		case ZS_FREE_PAGE_ID:
			result = "FREE";
			break;
		default:
			result = psprintf("UNKNOWN 0x%04x", zs_page_id);
	}

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 *  blkno int8
 *  nrecords int4
 *  freespace int4
 *  firstrecptr int8
 *  lastrecptr int8
 */
Datum
pg_zs_undo_pages(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	Buffer		metabuf;
	Page		metapage;
	ZSMetaPageOpaque *metaopaque;
	BlockNumber firstblk;
	BlockNumber blkno;
	char	   *ptr;
	char	   *endptr;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use zedstore inspection functions"))));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	rel = table_open(relid, AccessShareLock);

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	/*
	 * Get the current oldest undo page from the metapage.
	 */
	metabuf = ReadBuffer(rel, ZS_META_BLK);
	metapage = BufferGetPage(metabuf);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metaopaque = (ZSMetaPageOpaque *) PageGetSpecialPointer(metapage);

	firstblk = metaopaque->zs_undo_head;

	UnlockReleaseBuffer(metabuf);

	/*
	 * Loop through UNDO records, starting from the oldest page.
	 */
	blkno = firstblk;
	while (blkno != InvalidBlockNumber)
	{
		Datum		values[5];
		bool		nulls[5];
		Buffer		buf;
		Page		page;
		ZSUndoPageOpaque *opaque;
		int			nrecords;
		ZSUndoRecPtr firstptr = {0, 0, 0};
		ZSUndoRecPtr lastptr = {0, 0, 0};

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		CHECK_FOR_INTERRUPTS();

		/* Read the UNDO page */
		buf = ReadBuffer(rel, blkno);
		page = BufferGetPage(buf);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		opaque = (ZSUndoPageOpaque *) PageGetSpecialPointer(page);

		if (opaque->zs_page_id != ZS_UNDO_PAGE_ID)
		{
			elog(WARNING, "unexpected page id on UNDO page %u", blkno);
			break;
		}

		/* loop through all records on the page */
		endptr = (char *) page + ((PageHeader) page)->pd_lower;
		ptr = (char *) page + SizeOfPageHeaderData;
		nrecords = 0;
		while (ptr < endptr)
		{
			ZSUndoRec  *undorec = (ZSUndoRec *) ptr;

			Assert(undorec->undorecptr.blkno == blkno);

			lastptr = undorec->undorecptr;
			if (nrecords == 0)
				firstptr = lastptr;
			nrecords++;

			ptr += undorec->size;
		}

		values[0] = Int64GetDatum(blkno);
		values[1] = Int32GetDatum(nrecords);
		values[2] = Int32GetDatum(PageGetExactFreeSpace(page));
		values[3] = Int64GetDatum(firstptr.counter);
		values[4] = Int64GetDatum(lastptr.counter);

		blkno = opaque->next;
		UnlockReleaseBuffer(buf);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	tuplestore_donestoring(tupstore);

	table_close(rel, AccessShareLock);

	return (Datum) 0;
}

/*
 *  blkno int8
 *  tid int8
 *  total_size int8
 *  prev int8
 *  next int8
 *  decompressed_size uint32
 *  is_compressed bool
 */
Datum
pg_zs_toast_pages(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	BlockNumber blkno;
	BlockNumber nblocks;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use zedstore inspection functions"))));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	rel = table_open(relid, AccessShareLock);

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	nblocks = RelationGetNumberOfBlocks(rel);

	/* scan all blocks in physical order */
	for (blkno = 1; blkno < nblocks; blkno++)
	{
		Datum		values[8];
		bool		nulls[8];
		Buffer		buf;
		Page		page;
		ZSToastPageOpaque *opaque;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		CHECK_FOR_INTERRUPTS();

		/* Read the page */
		buf = ReadBuffer(rel, blkno);
		page = BufferGetPage(buf);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * we're only interested in toast pages.
		 */
		if (PageGetSpecialSize(page) != MAXALIGN(sizeof(ZSToastPageOpaque)))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		opaque = (ZSToastPageOpaque *) PageGetSpecialPointer(page);
		if (opaque->zs_page_id != ZS_TOAST_PAGE_ID)
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		values[0] = Int64GetDatum(blkno);
		if (opaque->zs_tid)
		{
			values[1] = Int64GetDatum(opaque->zs_tid);
			values[2] = Int64GetDatum(opaque->zs_total_size);
		}
		values[3] = Int64GetDatum(opaque->zs_slice_offset);
		values[4] = Int64GetDatum(opaque->zs_prev);
		values[5] = Int64GetDatum(opaque->zs_next);
		values[6] = Int32GetDatum(opaque->zs_decompressed_size);
		values[7] = BoolGetDatum(opaque->zs_is_compressed);

		UnlockReleaseBuffer(buf);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	tuplestore_donestoring(tupstore);

	table_close(rel, AccessShareLock);

	return (Datum) 0;
}

/*
 * attno int4
 * chunkno int4
 * upperstream bool
 * compressed bool
 * attbyval bool
 * attlen int4
 *
 * chunk_cursor int4
 * chunk_len int4
 * num_elems int4
 *
 * firsttid zstid
 * lasttid zstid
 *
 * tids[] zstid
 * datums[] bytea
 * isnulls[] bool
 */
Datum
pg_zs_dump_attstreams(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	BlockNumber blkno = PG_GETARG_INT64(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					(errmsg("must be superuser to use zedstore inspection functions"))));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	rel = table_open(relid, AccessShareLock);

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot access temporary tables of other sessions")));

	Datum		values[14];
	bool		nulls[14];

	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	int 		chunkno;
	bool		upperstream;
	bool		attbyval;
	int16 		attlen;
	int 		chunk_start;
	PageHeader	phdr;

	attstream_decoder 	decoder;

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	CHECK_FOR_INTERRUPTS();

	/* Read the page */
	buf = ReadBuffer(rel, blkno);
	page = BufferGetPage(buf);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * we're only interested in B-tree pages. (Presumably, most of the
	 * pages in the relation are b-tree pages, so it makes sense to scan
	 * the whole relation in physical order)
	 */
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(ZSBtreePageOpaque)))
	{
		UnlockReleaseBuffer(buf);
		table_close(rel, AccessShareLock);
		PG_RETURN_NULL();
	}

	opaque = (ZSBtreePageOpaque *) PageGetSpecialPointer(page);
	if (opaque->zs_page_id != ZS_BTREE_PAGE_ID ||
		opaque->zs_attno == ZS_META_ATTRIBUTE_NUM ||
		opaque->zs_level != 0)
	{
		UnlockReleaseBuffer(buf);
		table_close(rel, AccessShareLock);
		PG_RETURN_NULL();
	}

	attbyval = rel->rd_att->attrs[opaque->zs_attno - 1].attbyval;
	attlen = rel->rd_att->attrs[opaque->zs_attno - 1].attlen;

	phdr = (PageHeader) page;
	ZSAttStream *streams[2];
	int			nstreams = 0;

	if (phdr->pd_lower - SizeOfPageHeaderData > SizeOfZSAttStreamHeader)
	{
		streams[nstreams++] =  (ZSAttStream *) (((char *) page) + SizeOfPageHeaderData);
	}

	if (phdr->pd_special - phdr->pd_upper > SizeOfZSAttStreamHeader)
	{
		upperstream = nstreams;
		streams[nstreams++] =  (ZSAttStream *) (((char *) page) + phdr->pd_upper);
	}

	for (int i = 0; i < nstreams; i++)
	{
		ZSAttStream *stream = streams[i];
		bytea *chunk;
		zstid prevtid;
		zstid firsttid;
		zstid lasttid;

		init_attstream_decoder(&decoder, attbyval, attlen);
		decode_attstream_begin(&decoder, stream);

		chunkno = 0;
		chunk_start = decoder.pos;

		while (get_attstream_chunk_cont(&decoder, &prevtid, &firsttid, &lasttid, &chunk))
		{
			values[0] = Int16GetDatum(opaque->zs_attno);
			values[1] = Int32GetDatum(chunkno);
			chunkno++;

			values[2] = BoolGetDatum(upperstream == i);
			values[3] = BoolGetDatum((stream->t_flags & ATTSTREAM_COMPRESSED) != 0);
			values[4] = BoolGetDatum(attbyval);
			values[5] = Int16GetDatum(attlen);

			values[6] = Int32GetDatum(chunk_start);
			values[7] = Int32GetDatum(decoder.pos - chunk_start);
			chunk_start = decoder.pos;

			values[8] = ZSTidGetDatum(prevtid);
			values[9] = ZSTidGetDatum(firsttid);
			values[10] = ZSTidGetDatum(lasttid);
			values[11] = PointerGetDatum(chunk);

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}

	UnlockReleaseBuffer(buf);
	table_close(rel, AccessShareLock);

	destroy_attstream_decoder(&decoder);

	return (Datum) 0;
}

Datum
pg_zs_decode_chunk(PG_FUNCTION_ARGS)
{
	bool attbyval = PG_GETARG_BOOL(0);
	int attlen = PG_GETARG_INT16(1);
	zstid prevtid = PG_GETARG_ZSTID(2);
	zstid lasttid = PG_GETARG_ZSTID(3);
	bytea *chunk = PG_GETARG_BYTEA_P(4);
	attstream_decoder decoder;
	Datum		values[4];
	bool		nulls[4];
	ZSAttStream *attstream = palloc(SizeOfZSAttStreamHeader + VARSIZE_ANY_EXHDR(chunk));
	TupleDesc	tupdesc;
	HeapTuple tuple;

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	attstream->t_decompressed_size = VARSIZE_ANY_EXHDR(chunk);
	attstream->t_decompressed_bufsize = VARSIZE_ANY_EXHDR(chunk);
	attstream->t_size =  SizeOfZSAttStreamHeader + VARSIZE_ANY_EXHDR(chunk);
	attstream->t_flags = 0;
	attstream->t_lasttid = lasttid;
	memcpy(attstream->t_payload, VARDATA_ANY(chunk), VARSIZE_ANY_EXHDR(chunk));

	init_attstream_decoder(&decoder, attbyval, attlen);
	decode_attstream_begin(&decoder, attstream);
	decoder.prevtid = prevtid;

	if (!decode_attstream_cont(&decoder))
		PG_RETURN_NULL();
	else
	{
		ArrayBuildState *astate_tids = NULL;
		ArrayBuildState *astate_datums = NULL;
		ArrayBuildState *astate_isnulls = NULL;

		for (int i = 0; i < decoder.num_elements; i++)
		{

			bytea *attr_data;

			astate_tids = accumArrayResult(astate_tids,
			                               ZSTidGetDatum(decoder.tids[i]),
			                               false,
			                               ZSTIDOID,
			                               CurrentMemoryContext);
			if (decoder.isnulls[i])
			{
				astate_datums = accumArrayResult(astate_datums,
				                                 (Datum) 0,
				                                 true,
				                                 BYTEAOID,
				                                 CurrentMemoryContext);
			}
			else
			{
				/*
				 * Fixed length, attribute by value
				 */
				if (attbyval && attlen > 0)
				{
					attr_data = (bytea *) palloc(attlen + VARHDRSZ);
					SET_VARSIZE(attr_data, attlen + VARHDRSZ);
					memcpy(VARDATA(attr_data), &decoder.datums[i], attlen);
				}
				else if (!attbyval && attlen > 0)
				{
					attr_data = (bytea *) palloc(attlen + VARHDRSZ);
					SET_VARSIZE(attr_data, attlen + VARHDRSZ);
					memcpy(VARDATA(attr_data),
					       DatumGetPointer(decoder.datums[i]),
					       attlen);
				}
				else if (attlen < 0)
				{
					int len;
					len       =
						VARSIZE_ANY_EXHDR(DatumGetPointer(decoder.datums[i]));
					attr_data = (bytea *) palloc(len + VARHDRSZ);
					SET_VARSIZE(attr_data, len + VARHDRSZ);
					memcpy(VARDATA(attr_data),
					       VARDATA_ANY(DatumGetPointer(decoder.datums[i])),
					       len);
				}
				astate_datums = accumArrayResult(astate_datums,
				                                 PointerGetDatum(attr_data),
				                                 false,
				                                 BYTEAOID,
				                                 CurrentMemoryContext);
			}
			astate_isnulls = accumArrayResult(astate_isnulls,
			                                  BoolGetDatum(decoder.isnulls[i]),
			                                  false,
			                                  BOOLOID,
			                                  CurrentMemoryContext);
		}

		values[0] = Int32GetDatum(decoder.num_elements);
		values[1] = PointerGetDatum(makeArrayResult(astate_tids, CurrentMemoryContext));
		values[2] = PointerGetDatum(makeArrayResult(astate_datums, CurrentMemoryContext));
		values[3] = PointerGetDatum(makeArrayResult(astate_isnulls, CurrentMemoryContext));
	}

	destroy_attstream_decoder(&decoder);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 *  blkno int8
 *  nextblk int8
 *  attno int4
 *  level int4
 *
 *  lokey int8
 *  hikey int8

 *  nitems int4
 *  ncompressed int4
 *  totalsz int4
 *  uncompressedsz int4
 *  freespace int4
 */
Datum
pg_zs_btree_pages(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	BlockNumber blkno;
	BlockNumber nblocks;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use zedstore inspection functions"))));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	rel = table_open(relid, AccessShareLock);

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	nblocks = RelationGetNumberOfBlocks(rel);

	/* scan all blocks in physical order */
	for (blkno = 1; blkno < nblocks; blkno++)
	{
		Datum		values[11];
		bool		nulls[11];
		Buffer		buf;
		Page		page;
		ZSBtreePageOpaque *opaque;
		int			nitems;
		int			ncompressed;
		int			totalsz;
		int			uncompressedsz;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		CHECK_FOR_INTERRUPTS();

		/* Read the page */
		buf = ReadBuffer(rel, blkno);
		page = BufferGetPage(buf);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * we're only interested in B-tree pages. (Presumably, most of the
		 * pages in the relation are b-tree pages, so it makes sense to scan
		 * the whole relation in physical order)
		 */
		if (PageGetSpecialSize(page) != MAXALIGN(sizeof(ZSBtreePageOpaque)))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		opaque = (ZSBtreePageOpaque *) PageGetSpecialPointer(page);
		if (opaque->zs_page_id != ZS_BTREE_PAGE_ID)
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		nitems = 0;
		ncompressed = 0;
		totalsz = 0;
		uncompressedsz = 0;
		if (opaque->zs_level == 0)
		{
			/* meta leaf page */
			if (opaque->zs_attno == ZS_META_ATTRIBUTE_NUM) {
				OffsetNumber maxoff;
				OffsetNumber off;

				maxoff = PageGetMaxOffsetNumber(page);
				for (off = FirstOffsetNumber; off <= maxoff; off++)
				{
					ItemId iid = PageGetItemId(page, off);

					ZSTidArrayItem
						*item = (ZSTidArrayItem *) PageGetItem(page, iid);

					nitems++;
					totalsz += item->t_size;

					uncompressedsz += item->t_size;
				}
			}
			/* attribute leaf page */
			else
			{
				PageHeader	phdr = (PageHeader) page;
				ZSAttStream *streams[2];
				int			nstreams = 0;

				if (phdr->pd_lower - SizeOfPageHeaderData > SizeOfZSAttStreamHeader)
				{
					streams[nstreams++] =  (ZSAttStream *) (((char *) page) + SizeOfPageHeaderData);
				}

				if (phdr->pd_special - phdr->pd_upper > SizeOfZSAttStreamHeader)
				{
					streams[nstreams++] =  (ZSAttStream *) (((char *) page) + phdr->pd_upper);
				}

				for (int i = 0; i < nstreams; i++)
				{
					ZSAttStream *stream = streams[i];

					totalsz += stream->t_size;
					/*
					 *  FIXME: this is wrong. We currently don't calculate the
					 *  number of items in the stream
					 */
					nitems++;
					if ((stream->t_flags & ATTSTREAM_COMPRESSED) != 0)
					{
						ncompressed++;
						uncompressedsz += stream->t_decompressed_size;
					}
					else
					{
						uncompressedsz += stream->t_size;
					}
				}
			}
		}
		else
		{
			/* internal page */
			nitems = ZSBtreeInternalPageGetNumItems(page);
		}
		values[0] = Int64GetDatum(blkno);
		values[1] = Int64GetDatum(opaque->zs_next);
		values[2] = Int32GetDatum(opaque->zs_attno);
		values[3] = Int32GetDatum(opaque->zs_level);
		values[4] = Int64GetDatum(opaque->zs_lokey);
		values[5] = Int64GetDatum(opaque->zs_hikey);
		values[6] = Int32GetDatum(nitems);
		if (opaque->zs_level == 0)
		{
			values[7] = Int32GetDatum(ncompressed);
			values[8] = Int32GetDatum(totalsz);
			values[9] = Int32GetDatum(uncompressedsz);
		}
		else
		{
			nulls[7] = true;
			nulls[8] = true;
			nulls[9] = true;
		}
		values[10] = Int32GetDatum(PageGetExactFreeSpace(page));

		UnlockReleaseBuffer(buf);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	tuplestore_donestoring(tupstore);

	table_close(rel, AccessShareLock);

	return (Datum) 0;
}

/*
 *  blkno int8
 *  undo_head int8
 *  undo_tail int8
 *  undo_tail_first_counter int8
 *  undo_oldestpointer_counter int8
 *  undo_oldestpointer_blkno int8
 *  undo_oldestpointer_offset int8
 *  fpm_head int8
 *  flags int4
 */
Datum
pg_zs_meta_page(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	TupleDesc	tupdesc;
	Datum		values[9];
	bool		nulls[9];
	Buffer		buf;
	Page		page;
	ZSMetaPageOpaque *opaque;
	HeapTuple	tuple;
	Datum		result;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use zedstore inspection functions"))));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));


	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	CHECK_FOR_INTERRUPTS();

	/* open the metapage */
	rel = table_open(relid, AccessShareLock);

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	/* Read the page */
	buf = ReadBuffer(rel, ZS_META_BLK);
	page = BufferGetPage(buf);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(ZSMetaPageOpaque)))
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "Bad page special size");
	}
	opaque = (ZSMetaPageOpaque *) PageGetSpecialPointer(page);
	if (opaque->zs_page_id != ZS_META_PAGE_ID)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "The zs_page_id does not match ZS_META_PAGE_ID. Got: %d",
			 opaque->zs_page_id);
	}

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	values[0] = Int64GetDatum(ZS_META_BLK);
	values[1] = Int64GetDatum(opaque->zs_undo_head);
	values[2] = Int64GetDatum(opaque->zs_undo_tail);
	values[3] = Int64GetDatum(opaque->zs_undo_tail_first_counter);
	values[4] = Int64GetDatum(opaque->zs_undo_oldestptr.counter);
	values[5] = Int64GetDatum(opaque->zs_undo_oldestptr.blkno);
	values[6] = Int32GetDatum(opaque->zs_undo_oldestptr.offset);
	values[7] = Int64GetDatum(opaque->zs_fpm_head);
	values[8] = Int32GetDatum(opaque->zs_flags);

	UnlockReleaseBuffer(buf);

	table_close(rel, AccessShareLock);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}
