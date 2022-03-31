/*-------------------------------------------------------------------------
 *
 * memnodes.h
 *	  POSTGRES memory context node definitions.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/memnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEMNODES_H
#define MEMNODES_H

#include "nodes/nodes.h"

/*
 * MemoryContextCounters
 *		Summarization state for MemoryContextStats collection.
 *
 * The set of counters in this struct is biased towards AllocSet; if we ever
 * add any context types that are based on fundamentally different approaches,
 * we might need more or different counters here.  A possible API spec then
 * would be to print only nonzero counters, but for now we just summarize in
 * the format historically used by AllocSet.
 */
typedef struct MemoryContextCounters
{
	Size		nblocks;		/* Total number of malloc blocks */
	Size		freechunks;		/* Total number of free chunks */
	Size		totalspace;		/* Total bytes requested from malloc */
	Size		freespace;		/* The unused portion of totalspace */
} MemoryContextCounters;

/*
 * MemoryContext
 *		A logical context in which memory allocations occur.
 *
 * MemoryContext itself is an abstract type that can have multiple
 * implementations, though for now we have only AllocSetContext.
 * The function pointers in MemoryContextMethods define one specific
 * implementation of MemoryContext --- they are a virtual function table
 * in C++ terms.
 *
 * Node types that are actual implementations of memory contexts must
 * begin with the same fields as MemoryContext.
 *
 * Note: for largely historical reasons, typedef MemoryContext is a pointer
 * to the context struct rather than the struct type itself.
 */

typedef struct MemoryContextMethods
{
	void	   *(*alloc) (MemoryContext context, Size size); // 分配内存
	/* call this free_p in case someone #define's free() */
	void		(*free_p) (MemoryContext context, void *pointer); // 释放内存
	void	   *(*realloc) (MemoryContext context, void *pointer, Size size); // 重新分配
	void		(*init) (MemoryContext context); // 初始化内存上下文
	void		(*reset) (MemoryContext context); // 重置内存上下文
	void		(*delete_context) (MemoryContext context); // 删除内存上下文
	Size		(*get_chunk_space) (MemoryContext context, void *pointer); // 检查内存片段的大小
	bool		(*is_empty) (MemoryContext context); // 检查内存上下文是否为空
	void		(*stats) (MemoryContext context, int level, bool print, // 打印内存上下文状态
						  MemoryContextCounters *totals);
#ifdef MEMORY_CONTEXT_CHECKING
	void		(*check) (MemoryContext context); // 检查所有内存片段
#endif
} MemoryContextMethods;


typedef struct MemoryContextData
{
	// 内存节点类型
	NodeTag		type;			/* identifies exact kind of context */
	/* these two fields are placed here to minimize alignment wastage: */
	bool		isReset;		/* T = no space alloced since last reset */
	bool		allowInCritSection; /* allow palloc in critical section */
	// 内存处理函数
	MemoryContextMethods *methods;	/* virtual function table */
	// 父节点指针
	MemoryContext parent;		/* NULL if no parent (toplevel context) */
	// 第一个孩子节点指针
	MemoryContext firstchild;	/* head of linked list of children */
	// 下面两个为兄弟节点
	MemoryContext prevchild;	/* previous child of same parent */
	MemoryContext nextchild;	/* next child of same parent */
	// 节点名称
	char	   *name;			/* context name (just for debugging) */
	MemoryContextCallback *reset_cbs;	/* list of reset/delete callbacks */
} MemoryContextData;

/* utils/palloc.h contains typedef struct MemoryContextData *MemoryContext */
// 全局变量 AllocSetMethods 中指定了 AllocSetContext 实现的操作函数，在 aset.c 中

/*
 * MemoryContextIsValid
 *		True iff memory context is valid.
 *
 * Add new context types to the set accepted by this macro.
 */
#define MemoryContextIsValid(context) \
	((context) != NULL && \
	 (IsA((context), AllocSetContext) || IsA((context), SlabContext)))

#endif							/* MEMNODES_H */
