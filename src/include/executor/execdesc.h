/*-------------------------------------------------------------------------
 *
 * execdesc.h
 *	  plan and query descriptor accessor macros used by the executor
 *	  and related modules.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/execdesc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECDESC_H
#define EXECDESC_H

#include "nodes/execnodes.h"
#include "tcop/dest.h"


/* ----------------
 *		query descriptor:
 *
 *	a QueryDesc encapsulates everything that the executor
 *	needs to execute the query.
 *
 *	For the convenience of SQL-language functions, we also support QueryDescs
 *	containing utility statements; these must not be passed to the executor
 *	however.
 * ---------------------
 * 与 plannedStmt 关系见 plannodes_querydesc.png
 * 
 * 执行器初始化时, ExecutorStart 会根据查询计划树构造执行器全局状态(estate)以及计划节点
 * 执行状态(planstate)。在查询计划树的执行过程中,执行器将使用 planstate 来记录计划节点执
 * 行状态和数据, 并使用全局状态记录中的 es_tupleTable 字段在节点间传递结果元组。执行器的
 * 清理函数 ExecutorEnd 将回收执行器全局状态和计划节点执行状态
 */
typedef struct QueryDesc
{
	/* These fields are provided by CreateQueryDesc */
	CmdType		operation;		/* CMD_SELECT, CMD_UPDATE, etc. */
	/* 查询计划树 */
	PlannedStmt *plannedstmt;	/* planner's output (could be utility, too) */
	const char *sourceText;		/* source text of the query */
	Snapshot	snapshot;		/* snapshot to use for query */
	Snapshot	crosscheck_snapshot;	/* crosscheck for RI update/delete */
	DestReceiver *dest;			/* the destination for tuple output */
	ParamListInfo params;		/* param values being passed in */
	QueryEnvironment *queryEnv; /* query environment passed in */
	int			instrument_options; /* OR of InstrumentOption flags */

	/* These fields are set by ExecutorStart */
	TupleDesc	tupDesc;		/* descriptor for result tuples */
	/* 执行器全局状态 */
	EState	   *estate;			/* executor's query-wide state */
	/* 计划节点执行状态 */
	PlanState  *planstate;		/* tree of per-plan-node state */

	/* This field is set by ExecutorRun */
	bool		already_executed;	/* true if previously executed */

	/* This is always set NULL by the core system, but plugins can change it */
	struct Instrumentation *totaltime;	/* total time spent in ExecutorRun */
} QueryDesc;

/* in pquery.c */
extern QueryDesc *CreateQueryDesc(PlannedStmt *plannedstmt,
				const char *sourceText,
				Snapshot snapshot,
				Snapshot crosscheck_snapshot,
				DestReceiver *dest,
				ParamListInfo params,
				QueryEnvironment *queryEnv,
				int instrument_options);

extern void FreeQueryDesc(QueryDesc *qdesc);

#endif							/* EXECDESC_H  */
