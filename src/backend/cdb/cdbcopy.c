/*--------------------------------------------------------------------------
*
* cdbcopy.c
*	 Rrovides routines that executed a COPY command on an MPP cluster. These
*	 routines are called from the backend COPY command whenever MPP is in the
*	 default dispatch mode.
*
* Copyright (c) 2005-2008, Greenplum inc
*
*--------------------------------------------------------------------------
*/

#include "postgres.h"
#include "gp-libpq-fe.h"
#include "gp-libpq-int.h"
#include "miscadmin.h"
#include "cdb/cdbconn.h"
#include "cdb/cdbcopy.h"
#include "cdb/cdbdisp_query.h"
#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbfts.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbsrlz.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"
#include "commands/copy.h"
#include "tcop/tcopprot.h"
#include "utils/faultinjector.h"
#include "utils/memutils.h"

/*
 * Create a cdbCopy object that includes all the cdb
 * information and state needed by the backend COPY.
 */
CdbCopy *
makeCdbCopy(bool is_copy_in)
{
	CdbCopy    *c;
	int			seg;

	c = palloc(sizeof(CdbCopy));

	/* fresh start */
	c->total_segs = 0;
	c->primary_writer = NULL;
	c->remote_data_err = false;	
	c->io_errors = false;
	c->copy_in = is_copy_in;
	c->skip_ext_partition = false;
	c->outseglist = NIL;
	c->partitions = NULL;
	c->ao_segnos = NIL;
	initStringInfo(&(c->err_msg));
	initStringInfo(&(c->err_context));
	initStringInfo(&(c->copy_out_buf));	
	
	/* init total_segs */
	c->total_segs = getgpsegmentCount();
	c->aotupcounts = NULL;

	Assert(c->total_segs >= 0);

	/* Initialize the state of each segment database */
	c->segdb_state = (SegDbState **) palloc((c->total_segs) * sizeof(SegDbState *));

	for (seg = 0; seg < c->total_segs; seg++)
	{
		c->segdb_state[seg] = (SegDbState *) palloc(2 * sizeof(SegDbState));
		c->segdb_state[seg][0] = SEGDB_IDLE;	/* Primary can't be OUT */
	}

	/* init gangs */
	c->primary_writer = allocateWriterGang();
	
	/* init seg list for copy out */
	if (!c->copy_in)
	{
		int i;
		for (i = 0; i < c->total_segs; i++)
			c->outseglist = lappend_int(c->outseglist, i);
	}

	return c;
}


/*
 * starts a copy command on a specific segment database.
 * 
 * may pg_throw via elog/ereport.
 */
void
cdbCopyStart(CdbCopy *c, char *copyCmd)
{
	int			seg;
	MemoryContext oldcontext;
	CdbDispatcherState ds = {NULL, NULL};
	List	   *parsetree_list;
	Node	   *parsetree = NULL;
	List	   *querytree_list;
	char	   *serializedQuerytree;
	int			serializedQuerytree_len;
	Query	   *q = makeNode(Query);
	
	/* clean err message */
	c->err_msg.len = 0;
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;
	
	/*
	 * A context it is safe to build parse trees in.
	 * We don't want them in TopMemory, as the trees 
	 * should only last for this one statement
	 */
	oldcontext = MemoryContextSwitchTo(MessageContext);

	QueryContext = CurrentMemoryContext;
	
	/* dispatch copy command to both primary and mirror writer gangs */
	
	/*
	 * Let's parse the copy command into a query tree, serialize it, and
	 * send it down to the QE or DA.
	 * 
	 * Note that we can't just use the original CopyStmt node in this routine,
	 * but we need to use the re-created command that copy.c prepared for us as
	 * it may be different from the original command in several cases (such as
	 * COPY into tables where a default value is evaluated on the QD).
	 */
	
	/*
	 * parse it to a raw parsetree 
	 */
	
	parsetree_list = pg_parse_query(copyCmd);
	
	/*
	 * Assume it will be one statement node, not multiple ones.
	 */

	parsetree = (Node *) linitial(parsetree_list);
	Assert(nodeTag(parsetree) == T_CopyStmt);
	
	/*
	 * Ok, we have a raw parse tree of the copy stmt.
	 * 
	 * I don't think analyze and rewrite will do much to it,
	 * but it will at least package it up as a query node,
	 * which we need for serializing.  And if the copy
	 * statement has a "select" statement embedded, this
	 * is essential.
	 */
	
	querytree_list = pg_analyze_and_rewrite(parsetree, copyCmd,
													NULL, 0);
	
	/*
	 * Again, assume it is one query node, not multiple
	 */
	q = (Query *)linitial(querytree_list);
	
	
	Assert(IsA(q,Query));
	Assert(q->commandType == CMD_UTILITY);
	Assert(q->utilityStmt != NULL);
	Assert(IsA(q->utilityStmt,CopyStmt));
	
	q->querySource = QSRC_ORIGINAL;

	q->canSetTag = true;

	/* add in partitions for dispatch */
	((CopyStmt *)q->utilityStmt)->partitions = c->partitions;
	
	/* add in AO segno map for dispatch */
	((CopyStmt *)q->utilityStmt)->ao_segnos = c->ao_segnos;

	((CopyStmt *)q->utilityStmt)->skip_ext_partition = c->skip_ext_partition;
	
	MemoryContextSwitchTo(oldcontext);

	/*
	 * serialized the stmt tree, and dispatch it ....
	 */
	serializedQuerytree = serializeNode((Node *) q, &serializedQuerytree_len, NULL /*uncompressed_size*/);

	Assert(serializedQuerytree != NULL);
	
	dtmPreCommand("CdbCopy", copyCmd, NULL,
			c->copy_in, /* needs 2-phase */
			true, /* want snapshot */
			false /* in cursor */);
	
	cdbdisp_dispatchCommand(copyCmd, serializedQuerytree, serializedQuerytree_len, 
								false 		/* cancelonError */, 
								c->copy_in 	/* need2phase */, 
								true 		/* withSnapshot */,
								&ds);

	SIMPLE_FAULT_INJECTOR(CdbCopyStartAfterDispatch);

	/*
	 * Wait for all QEs to finish. If not all of our QEs were successful,
	 * report the error and throw up.
	 */
	cdbdisp_finishCommand(&ds, NULL, NULL);

	/* fill in CdbCopy structure */
	for (seg = 0; seg < c->total_segs; seg++)
	{
		c->segdb_state[seg][0] = SEGDB_COPY;	/* we be jammin! */
	}

	return;
}

/*
 * sends data to a copy command on a specific segment (usually
 * the hash result of the data value).
 */
void
cdbCopySendData(CdbCopy *c, int target_seg, const char *buffer,
				int nbytes)
{
	SegmentDatabaseDescriptor *q;
	Gang	   *gp;
	int			result;

	/* clean err message */
	c->err_msg.len = 0;
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;
		/*
		 * NOTE!! note that another DELIM was added, for the buf_converted
		 * in the code above. I didn't do it because it's broken right now
		 */

	gp = c->primary_writer;

	q = getSegmentDescriptorFromGang(gp, target_seg);
		
	/* transmit the COPY data */
	result = PQputCopyData(q->conn, buffer, nbytes);

	if (result != 1)
	{
		if (result == 0)
			appendStringInfo(&(c->err_msg),
							 "Failed to send data to segment %d, attempt blocked\n",
							 target_seg);

		if (result == -1)
			appendStringInfo(&(c->err_msg),
							 "Failed to send data to segment %d: %s\n",
							 target_seg, PQerrorMessage(q->conn));

		c->io_errors = true;
	}
}

/*
 * sends data to a copy command on a specific segment (usually
 * the hash result of the data value).
 */
void
cdbCopySendDataSingle(CdbCopy *c, int target_seg, const char *buffer,
				int nbytes)
{
	SegmentDatabaseDescriptor *q;
	Gang	   *gp;
	int			result;

	/* clean err message */
	c->err_msg.len = 0;
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;
 
	gp = c->primary_writer;
	
	Assert(gp);

	q = getSegmentDescriptorFromGang(gp, target_seg);

	/* transmit the COPY data */
	elog(DEBUG4,"PQputCopyData to segment %d\n", target_seg);
	result = PQputCopyData(q->conn, buffer, nbytes);

	if (result != 1)
	{
		if (result == 0)
			appendStringInfo(&(c->err_msg),
							 "Failed to send data to segment %d, attempt blocked\n",
							 target_seg);

		if (result == -1)
			appendStringInfo(&(c->err_msg),
							 "Failed to send data to segment %d: %s\n",
							 target_seg, PQerrorMessage(q->conn));

		c->io_errors = true;
	}
 
}

/*
 * gets a chunk of rows of data from a copy command.
 * returns boolean true if done. Caller should still
 * empty the leftovers in the outbuf in that case.
 */
bool cdbCopyGetData(CdbCopy *c, bool copy_cancel, uint64 *rows_processed)
{
	SegmentDatabaseDescriptor *q;
	Gang	    *gp;
	PGresult   *res;
	int			nbytes;

	/* clean err message */
	c->err_msg.len = 0;
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;

	/* clean out buf data */
	c->copy_out_buf.len = 0;
	c->copy_out_buf.data[0] = '\0';
	c->copy_out_buf.cursor = 0;
	
	gp = c->primary_writer;

	/*
	 * MPP-7712: we used to issue the cancel-requests for each *row* we got back
	 * from each segment -- this is potentially millions of cancel-requests.
	 * Cancel requests consist of an out-of-band connection to the segment-postmaster,
	 * this is *not* a lightweight operation!
	 */
	if (copy_cancel)
	{
		ListCell   *cur;
		
		/* iterate through all the segments that still have data to give */
		foreach(cur, c->outseglist)
		{
			int			source_seg = lfirst_int(cur);

			q = getSegmentDescriptorFromGang(gp, source_seg);

			/* send a query cancel request to that segdb */
			PQrequestCancel(q->conn);
		}
	}

	/*
	 * Collect data rows from the segments that still have rows to 
	 * give until chunk minimum size is reached
	 */
	while (c->copy_out_buf.len < COPYOUT_CHUNK_SIZE)
	{
		ListCell   *cur;
		
		/* iterate through all the segments that still have data to give */
		foreach(cur, c->outseglist)
		{
			int			source_seg = lfirst_int(cur);
			char		*buffer;

			q = getSegmentDescriptorFromGang(gp, source_seg);

			/* get 1 row of COPY data */
			nbytes = PQgetCopyData(q->conn, &buffer, false);

			/* 
			 * SUCCESS -- got a row of data 
			 */
			if (nbytes > 0 && buffer) 
			{
				/* append the data row to the data chunk */
				appendBinaryStringInfo(&(c->copy_out_buf), buffer, nbytes);

				/* increment the rows processed counter for the end tag */
				(*rows_processed)++;

				PQfreemem(buffer);
			}

			/* 
			 * DONE -- Got all the data rows from this segment, or a cancel request. 
			 */
			else if (nbytes == -1)
			{
				/*
				 * Fetch any error status existing on completion of the COPY command.
				 */
				while (NULL != (res = PQgetResult(q->conn)))
				{
					/* if the COPY command had an error */
					if (PQresultStatus(res) == PGRES_FATAL_ERROR)
					{
						appendStringInfo(&(c->err_msg), "Error from segment %d: %s\n",
										 source_seg, PQresultErrorMessage(res));
						c->remote_data_err = true;
					}

					/* free the PGresult object */
					PQclear(res);
				}

				/* remove the segment that completed sending data from the list*/
				c->outseglist = list_delete_int(c->outseglist, source_seg);

				/* no more segments with data on the list. we are done */
				if (list_length(c->outseglist) == 0)
					return true; /* done */

				/* start over from first seg as we just changes the seg list */
				break;
			}

			/* 
			 * ERROR!
			 */
			else
			{
				/* should never happen since we are blocking. Don't bother to try again, exit with error. */
				if (nbytes == 0)
					appendStringInfo(&(c->err_msg),
									 "Failed to get data from segment %d, attempt blocked\n",
									 source_seg);

				if (nbytes == -2)
				{
					appendStringInfo(&(c->err_msg),
									 "Failed to get data from segment %d: %s\n",
									 source_seg, PQerrorMessage(q->conn));

					/* remove the segment that completed sending data from the list*/
					c->outseglist = list_delete_int(c->outseglist, source_seg);

					/* no more segments with data on the list. we are done */
					if (list_length(c->outseglist) == 0)
						return true; /* done */

					/* start over from first seg as we just changes the seg list */
					break;
				}

				c->io_errors = true;
			}
		}

		if (c->copy_out_buf.len > COPYOUT_CHUNK_SIZE)
			break;
	}

	return false;
}

/*
 * Process the results from segments after sending the end of copy command.
 */
static void
processCopyEndResults(CdbCopy *c,
					  SegmentDatabaseDescriptor *db_descriptors,
					  int *results,
					  int size,
					  SegmentDatabaseDescriptor **failedSegDBs,
					  bool *err_header,
					  bool *first_error,
					  int *failed_count,
					  int *total_rows_rejected)
{
	SegmentDatabaseDescriptor *q;
	int seg;
	PGresult *res;
	int			segment_rows_rejected = 0; /* num of rows rejected by this QE */

	for (seg = 0; seg < size; seg ++)
	{
		int result = results[seg];
		q = &db_descriptors[seg];
		
		/* get command end status */
		if (result == 0)
		{
			/* attempt blocked */

			/*
			 * CDB TODO: Can this occur?  The libpq documentation says,
			 * "this case is only possible if the connection is in
			 * nonblocking mode... wait for write-ready and try again",
			 * i.e., the proper response would be to retry, not error out.
			 */
			if (!(*err_header))
				appendStringInfo(&(c->err_msg),
								 "Failed to complete COPY on the following:\n");
			*err_header = true;

			appendStringInfo(&(c->err_msg), "primary segment %d, dbid %d, attempt blocked\n", 
							 seg, q->segment_database_info->dbid);
			c->io_errors = true;
		}

		/*
		 * Fetch any error status existing on completion of the COPY command.
		 * It is critical that for any connection that had an asynchronous
		 * command sent thru it, we call PQgetResult until it returns NULL.
		 * Otherwise, the next time a command is sent to that connection,
		 * it will return an error that there's a command pending.
		 */
		while ((res = PQgetResult(q->conn)) != NULL && PQstatus(q->conn) != CONNECTION_BAD)
		{
			elog(DEBUG1,"PQgetResult got status %d seg %d    ",
				 PQresultStatus(res), q->segindex);
			/* if the COPY command had a data error */
			if (PQresultStatus(res) == PGRES_FATAL_ERROR)
			{
				/*
				 * Always append error from the primary. Append error from
				 * mirror only if its primary didn't have an error.
				 *
				 * For now, we only report the first error we get from the
				 * QE's.
				 *
				 * We get the error message in pieces so that we could append
				 * whoami to the primary error message only.
				 */
				if (*first_error)
				{
					char   *pri = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
					char   *dtl = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
					char   *ctx = PQresultErrorField(res, PG_DIAG_CONTEXT);

					if (pri)
						appendStringInfo(&(c->err_msg), "%s", pri);
					else
						appendStringInfo(&(c->err_msg), "Unknown primary error.");

					if (q->whoami)
						appendStringInfo(&(c->err_msg), "  (%s)", q->whoami);

					if (dtl)
						appendStringInfo(&(c->err_msg), "\n%s", dtl);

					/*
					 * note that due to cdb_tidy_message() in elog.c "If more than
					 * one line, move lines after the first to errdetail" so we save
					 * the context in another stringInfo and fetch it in the error
					 * callback in copy.c, so it wouldn't appear as DETAIL...
					 */
					if (ctx)
						appendStringInfo(&(c->err_context), "%s", ctx);
						
					/* Indicate that the err_msg already was filled with one error */
					*first_error = false;
				}
				c->remote_data_err = true;
			}

			/*
			 * If we are still in copy mode, tell QE to stop it.  COPY_IN
			 * protocol has a way to say 'end of copy' but COPY_OUT doesn't.
			 * We have no option but sending cancel message and consume
			 * the output until the state transition to non-COPY.
			 */
			if (PQresultStatus(res) == PGRES_COPY_IN)
			{
				elog(LOG, "Segment still in copy in, retrying the putCopyEnd");
				result = PQputCopyEnd(q->conn, NULL);
			}
			else if (PQresultStatus(res) == PGRES_COPY_OUT)
			{
				char	   *buffer = NULL;

				elog(LOG, "Segment still in copy out, canceling QE");
				/*
				 * I'm a bit worried about sending a cancel, as if this
				 * is a success case the QE gets inconsistent state than
				 * QD.  But this code path is mostly for error handling
				 * and in a success case we wouldn't see COPY_OUT here.
				 * It's not clear what to do if this cancel failed,
				 * since this is not a path we can error out.  FATAL maybe
				 * the way, but I leave it for now.
				 */
				PQrequestCancel(q->conn);

				/* Need to consume data from QE until he recognizes cancel. */
				PQgetCopyData(q->conn, &buffer, false);
				if (buffer)
					free(buffer);
			}

			/* in SREH mode, check if this seg rejected (how many) rows */
			if (res->numRejected > 0)
				segment_rows_rejected = res->numRejected;

			/* Get AO tuple counts */
			c->aotupcounts = PQprocessAoTupCounts(c->partitions, c->aotupcounts, res->aotupcounts, res->naotupcounts);
			/* free the PGresult object */
			PQclear(res);
		}

		/* Finished with this segment db. */
		c->segdb_state[seg][0] = SEGDB_DONE;

		/* 
		 * add number of rows rejected from this segment to the 
		 * total of rejected rows. Only count from primary segs.
		 */
		if (segment_rows_rejected > 0)
			*total_rows_rejected += segment_rows_rejected;
		
		segment_rows_rejected = 0;
		
		/* Lost the connection? */
		if (PQstatus(q->conn) == CONNECTION_BAD)
		{
			if (!*(err_header))
				appendStringInfo(&(c->err_msg),
								 "ERROR - Failed to complete COPY on the following:\n");
			*err_header = true;
			
			/* command error */
			c->io_errors = true;
			appendStringInfo(&(c->err_msg), "Primary segment %d, dbid %d, with error: %s\n",
							 seg, q->segment_database_info->dbid, PQerrorMessage(q->conn));
			
			/* Free the PGconn object. */
			PQfinish(q->conn);
			q->conn = NULL;
			
			/* Let FTS deal with it! */
			failedSegDBs[*failed_count] = q;
			(*failed_count)++;
		}
	}
}

					  

/*
 * ends the copy command on all segment databases.
 * c->err_msg will include the error msg(s) if any.
 *
 * returns the number of rows rejected by QE's. Normally 
 * will be 0 however in single row error handling mode 
 * could be larger than 0.
 */
int
cdbCopyEnd(CdbCopy *c)
{
	SegmentDatabaseDescriptor *q;
	SegmentDatabaseDescriptor **failedSegDBs;
	Gang	   *gp;
	int			*results;			/* final result of COPY command execution */
	int			seg;

	int			failed_count = 0;
	int			total_rows_rejected = 0; /* total num rows rejected by all QEs */
	bool		err_header = false;
	bool		first_error = true;
	struct	SegmentDatabaseDescriptor *db_descriptors;
	int size;

	/* clean err message */
	c->err_msg.len = 0;
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;

	/* allocate a failed segment database pointer array */
	failedSegDBs = (SegmentDatabaseDescriptor **) palloc(c->total_segs * 2 * sizeof(SegmentDatabaseDescriptor *));

	gp = c->primary_writer;
	db_descriptors = gp->db_descriptors;
	size = gp->size;

	/* results from each segment */
	results = (int *)palloc0(sizeof(int) * size);

	for (seg = 0; seg < size; seg++)
	{
		q = &db_descriptors[seg];
		elog(DEBUG1, "PQputCopyEnd seg %d    ",q->segindex);
		/* end this COPY command */
		results[seg] = PQputCopyEnd(q->conn, NULL);
	}

	processCopyEndResults(c, db_descriptors, results, size,
						  failedSegDBs, &err_header,
						  &first_error, &failed_count, &total_rows_rejected);

	/* If lost contact with segment db, try to reconnect. */
	if (failed_count > 0)
	{
		elog(LOG, "%s", c->err_msg.data);
		elog(LOG, "COPY passes failed segment(s) information to FTS");
		FtsHandleNetFailure(failedSegDBs, failed_count);
	}

	pfree(results);
	pfree(failedSegDBs);

	return total_rows_rejected;
}


/*
 * start a global transaction for COPY.
 */
void
cdbCopyStartTransaction(void)
{
	/* since we don't use cdbdisp's dispatch services, we need to explicitly
	 * kick off a BEGIN if its the real start of a transaction
	 */
	sendDtxExplicitBegin();
	
	/* this txn is gonna be dirty */
	dtmPreCommand("cdbCopyStartTransaction", "(none)", NULL, /* needs two-phase */ true, /* withSnapshot */ true, /* inCursor */ false );
}
