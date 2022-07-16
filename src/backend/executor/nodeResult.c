/*-------------------------------------------------------------------------
 *
 * nodeResult.c
 *	  support for constant nodes needing special code.
 *
 * DESCRIPTION
 *
 *		Result nodes are used in queries where no relations are scanned.
 *		Examples of such queries are:
 *
 *				select 1 * 2
 *
 *				insert into emp values ('mike', 15000)
 *
 *		(Remember that in an INSERT or UPDATE, we need a plan tree that
 *		generates the new rows.)
 *
 *		Result nodes are also used to optimise queries with constant
 *		qualifications (ie, quals that do not depend on the scanned data),
 *		such as:
 *
 *				select * from emp where 2 > 1
 *
 *		In this case, the plan generated is
 *
 *						Result	(with 2 > 1 qual)
 *						/
 *				   SeqScan (emp.*)
 *
 *		At runtime, the Result node evaluates the constant qual once,
 *		which is shown by EXPLAIN as a One-Time Filter.  If it's
 *		false, we can return an empty result set without running the
 *		controlled plan at all.  If it's true, we run the controlled
 *		plan normally and pass back the results.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeResult.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeResult.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/* ----------------------------------------------------------------
 *		ExecResult(node)
 *
 *		returns the tuples from the outer plan which satisfy the
 *		qualification clause.  Since result nodes with right
 *		subtrees are never planned, we ignore the right subtree
 *		entirely (for now).. -cim 10/7/89
 *
 *		The qualification containing only constant clauses are
 *		checked first before any processing is done. It always returns
 *		'nil' if the constant qualification is not satisfied.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecResult(PlanState *pstate)
{
	ResultState *node = castNode(ResultState, pstate);
	TupleTableSlot *outerTupleSlot;
	PlanState  *outerPlan;
	ExprContext *econtext;

	CHECK_FOR_INTERRUPTS();

	econtext = node->ps.ps_ExprContext;

	/*
	 * check constant qualifications like (2 > 1), if not already done
	 *
	 * 判断 rs_checkqual 是否为真, 为真表示需要进行常量表达式计算
	 */
	if (node->rs_checkqual)
	{
		bool		qualResult = ExecQual(node->resconstantqual, econtext);

		/* 计算完成后将 rs_checkqual 设置为假,下次执行不会再进行计算 */
		node->rs_checkqual = false;
		/* 常量表达式计算结果为假,表示没有满足条件的结果,则 Result 节点直接返回NULL */
		if (!qualResult)
		{
			node->rs_done = true;
			return NULL;
		}
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * if rs_done is true then it means that we were asked to return a
	 * constant tuple and we already did the last time ExecResult() was
	 * called, OR that we failed the constant qual check. Either way, now we
	 * are through.
	 */
	/* 常量表达式计算结果为真,则会检查是否有左子树,有则从其中获取元组 */
	while (!node->rs_done)
	{
		outerPlan = outerPlanState(node);

		if (outerPlan != NULL)
		{
			/*
			 * retrieve tuples from the outer plan until there are no more.
			 */
			outerTupleSlot = ExecProcNode(outerPlan);

			if (TupIsNull(outerTupleSlot))
				return NULL;

			/*
			 * prepare to compute projection expressions, which will expect to
			 * access the input tuples as varno OUTER.
			 */
			econtext->ecxt_outertuple = outerTupleSlot;
		}
		else
		{
			/*
			 * if we don't have an outer plan, then we are just generating the
			 * results from a constant target list.  Do it only once.
			 */
			/* 没有左子树表示为 VALUES 子句, 则直接设置 rs_done 为真,因为只有一个元组需要
			 * 输出(这里的 VALUES 子句中只允许有一个元组)
			 */
			node->rs_done = true;
		}

		/* form the result tuple using ExecProject(), and return it */
		/* 最后对结果元组执行投影操作并返回 */
		return ExecProject(node->ps.ps_ProjInfo);
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecResultMarkPos
 * ----------------------------------------------------------------
 */
void
ExecResultMarkPos(ResultState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	if (outerPlan != NULL)
		ExecMarkPos(outerPlan);
	else
		elog(DEBUG2, "Result nodes do not support mark/restore");
}

/* ----------------------------------------------------------------
 *		ExecResultRestrPos
 * ----------------------------------------------------------------
 */
void
ExecResultRestrPos(ResultState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	if (outerPlan != NULL)
		ExecRestrPos(outerPlan);
	else
		elog(ERROR, "Result nodes do not support mark/restore");
}

/* ----------------------------------------------------------------
 *		ExecInitResult
 *
 *		Creates the run-time state information for the result node
 *		produced by the planner and initializes outer relations
 *		(child nodes).
 * ----------------------------------------------------------------
 */
ResultState *
ExecInitResult(Result *node, EState *estate, int eflags)
{
	ResultState *resstate;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_MARK | EXEC_FLAG_BACKWARD)) ||
		   outerPlan(node) != NULL);

	/*
	 * create state structure
	 */
	resstate = makeNode(ResultState);
	resstate->ps.plan = (Plan *) node;
	resstate->ps.state = estate;
	resstate->ps.ExecProcNode = ExecResult;

	/* result 节点是否已经处理完所有元组 */
	resstate->rs_done = false;
	/* 检查是否有常量表达式,有则将执行状态节点的 rs_checkqual 设置为真,否则设置为假 */
	resstate->rs_checkqual = (node->resconstantqual == NULL) ? false : true;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &resstate->ps);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &resstate->ps);

	/*
	 * initialize child expressions
	 */
	resstate->ps.qual =
		ExecInitQual(node->plan.qual, (PlanState *) resstate);
	resstate->resconstantqual =
		ExecInitQual((List *) node->resconstantqual, (PlanState *) resstate);

	/*
	 * initialize child nodes
	 */
	outerPlanState(resstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * we don't use inner plan
	 */
	/* 保证无右子节点 */
	Assert(innerPlan(node) == NULL);

	/*
	 * initialize tuple type and projection info
	 */
	ExecAssignResultTypeFromTL(&resstate->ps);
	ExecAssignProjectionInfo(&resstate->ps, NULL);

	return resstate;
}

/* ----------------------------------------------------------------
 *		ExecEndResult
 *
 *		frees up storage allocated through C routines
 * ----------------------------------------------------------------
 */
void
ExecEndResult(ResultState *node)
{
	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * shut down subplans
	 */
	ExecEndNode(outerPlanState(node));
}

void
ExecReScanResult(ResultState *node)
{
	node->rs_done = false;
	node->rs_checkqual = (node->resconstantqual == NULL) ? false : true;

	/*
	 * If chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->ps.lefttree &&
		node->ps.lefttree->chgParam == NULL)
		ExecReScan(node->ps.lefttree);
}
