/*-------------------------------------------------------------------------
 *
 * pg_class.h
 *	  definition of the system "relation" relation (pg_class)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_class.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CLASS_H
#define PG_CLASS_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_class definition.  cpp turns this into
 *		typedef struct FormData_pg_class
 * 存储表以及表类似结构的数据库对象信息
 * pg_class表记载表和几乎所有有字段或者是那些类似表的东西
 * ----------------
 */
#define RelationRelationId	1259
#define RelationRelation_Rowtype_Id  83
/* 参考 http://www.postgres.cn/docs/10/catalog-pg-class.html */
CATALOG(pg_class,1259) BKI_BOOTSTRAP BKI_ROWTYPE_OID(83) BKI_SCHEMA_MACRO
{
	/* 表、索引、视图等的名字。*/
	NameData	relname;		/* class name */
	/* 包含该关系的名字空间的OID schema 的 OID */
	Oid			relnamespace;	/* OID of namespace containing this class */
	Oid			reltype;		/* OID of entry in pg_type for table's
								 * implicit row type */
	Oid			reloftype;		/* OID of entry in pg_type for underlying
								 * composite type */
	Oid			relowner;		/* class owner */
	/* 如果这是一个索引，表示索引使用的访问方法（B树、哈希等）*/
	Oid			relam;			/* index access method; 0 if not an index */
	/* 该关系的磁盘文件的名字，0表示这是一个“映射”关系，其磁盘文件名取决于低层状态 ）*/
	Oid			relfilenode;	/* identifier of physical storage file */

	/* relfilenode == 0 means it is a "mapped" relation, see relmapper.c */
	/* 该关系所存储的表空间。如果为0，使用数据库的默认表空间。（如果关系无磁盘文件时无意义））*/
	Oid			reltablespace;	/* identifier of table space for relation */
	/* 该表磁盘表示的尺寸，以页面计（页面尺寸为BLCKSZ）。这只是一个由规划器使用的估计值
	   它被VACUUM、ANALYZE以及一些DDL命令（如CREATE INDEX）所更新 
	*/
	int32		relpages;		/* # of blocks (not always up-to-date) */
	/* 表中的行数。这只是一个由规划器使用的估计值。它被VACUUM、ANALYZE以及一些DDL命令（如CREATE INDEX）所更新*/
	float4		reltuples;		/* # of tuples (not always up-to-date) */
	/* 在表的可见性映射表中被标记为全可见的页数。这只是一个由规划器使用的估计值。）*/
	/* 它被VACUUM、ANALYZE以及一些DDL命令（如CREATE INDEX）所更新。*/
	int32		relallvisible;	/* # of all-visible blocks (not always
								 * up-to-date) */
	/* 与该表相关联的TOAST表的OID，如果没有则为0。TOAST表将大属性“线外”存储在一个二级表中。）*/
	Oid			reltoastrelid;	/* OID of toast table; 0 if none */
	/* 如果这是一个表并且其上建有（或最近建有）索引则为真 ）*/
	bool		relhasindex;	/* T if has (or has had) any indexes */
	/* 如果该表在集簇中的所有数据库间共享则为真。只有某些系统目录（如pg_database）是共享的 ）*/
	bool		relisshared;	/* T if shared across databases */
	/* p = 永久表，u = 无日志表， t = 临时表 */
	char		relpersistence; /* see RELPERSISTENCE_xxx constants below */
	/* r = 普通表，i = 索引， S = 序列，t = TOAST 表，v = 视图， m = 物化视图， c = 组合类型， f = 外部表， p = 分区表*/
	char		relkind;		/* see RELKIND_xxx constants below */
	/* 关系中用户列的数目（系统列不计算在内）。在pg_attribute中必须有这么多对应的项。 ）*/
	/* 另请参阅pg_attribute.attnum。 ）*/
	int16		relnatts;		/* number of user attributes */

	/*
	 * Class pg_attribute must contain exactly "relnatts" user attributes
	 * (with attnums ranging from 1 to relnatts) for this class.  It may also
	 * contain entries with negative attnums for system attributes.
	 */
	/* 表上CHECK约束的数目，参见pg_constraint目录 ）*/
	int16		relchecks;		/* # of CHECK constraints for class */
	/* 如果为关系的每一行生成一个OID则为真 */
	bool		relhasoids;		/* T if we generate OIDs for rows of rel */
	/* 如果表有（或曾有）一个主键则为真 */
	bool		relhaspkey;		/* has (or has had) PRIMARY KEY index */
	/* 如果表有（或曾有）规则则为真，参见 pg_rewrite目录 */
	bool		relhasrules;	/* has (or has had) any rules */
	/* 如果表有（或曾有）触发器则为真，参见 pg_trigger目录 */
	bool		relhastriggers; /* has (or has had) any TRIGGERs */
	/* 如果表有（或曾有）任何继承子女则为真 */
	bool		relhassubclass; /* has (or has had) derived classes */
	/* 如果表上启用了行级安全性则为真，参见 pg_policy目录 */
	bool		relrowsecurity; /* row security is enabled or not */
	bool		relforcerowsecurity;	/* row security forced for owners or
										 * not */
	bool		relispopulated; /* matview currently holds query results */
	char		relreplident;	/* see REPLICA_IDENTITY_xxx constants  */
	bool		relispartition; /* is relation a partition? */
	/* 在此之前的所有事务ID在表中已经被替换为一个永久的（“冻结的”) 事务ID。
		 这用于跟踪表是否需要被清理，以便阻止事务ID回卷或者允许pg_xact被收缩。
	 	如果该关系不是一个表则为0（InvalidTransactionId）
	*/
	TransactionId relfrozenxid; /* all Xids < this are frozen in this rel */
	/* 在此之前的多事务ID在表中已经被替换为一个事务ID。这被用于跟踪表是否需要被清理，
	   以阻止 多事务ID回卷或者允许pg_multixact被收缩。如果关系不是一个表则 为0（InvalidMultiXactId）
	*/
	TransactionId relminmxid;	/* all multixacts in this rel are >= this.
								 * this is really a MultiXactId */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* NOTE: These fields are not present in a relcache entry's rd_rel field. */
	aclitem		relacl[1];		/* access permissions */
	text		reloptions[1];	/* access-method-specific options */
	pg_node_tree relpartbound;	/* partition bound node tree */
#endif
} FormData_pg_class;

/* Size of fixed part of pg_class tuples, not counting var-length fields */
#define CLASS_TUPLE_SIZE \
	 (offsetof(FormData_pg_class,relminmxid) + sizeof(TransactionId))

/* ----------------
 *		Form_pg_class corresponds to a pointer to a tuple with
 *		the format of pg_class relation.
 * ----------------
 */
typedef FormData_pg_class *Form_pg_class;

/* ----------------
 *		compiler constants for pg_class
 * ----------------
 */

#define Natts_pg_class						33
#define Anum_pg_class_relname				1
#define Anum_pg_class_relnamespace			2
#define Anum_pg_class_reltype				3
#define Anum_pg_class_reloftype				4
#define Anum_pg_class_relowner				5
#define Anum_pg_class_relam					6
#define Anum_pg_class_relfilenode			7
#define Anum_pg_class_reltablespace			8
#define Anum_pg_class_relpages				9
#define Anum_pg_class_reltuples				10
#define Anum_pg_class_relallvisible			11
#define Anum_pg_class_reltoastrelid			12
#define Anum_pg_class_relhasindex			13
#define Anum_pg_class_relisshared			14
#define Anum_pg_class_relpersistence		15
#define Anum_pg_class_relkind				16
#define Anum_pg_class_relnatts				17
#define Anum_pg_class_relchecks				18
#define Anum_pg_class_relhasoids			19
#define Anum_pg_class_relhaspkey			20
#define Anum_pg_class_relhasrules			21
#define Anum_pg_class_relhastriggers		22
#define Anum_pg_class_relhassubclass		23
#define Anum_pg_class_relrowsecurity		24
#define Anum_pg_class_relforcerowsecurity	25
#define Anum_pg_class_relispopulated		26
#define Anum_pg_class_relreplident			27
#define Anum_pg_class_relispartition		28
#define Anum_pg_class_relfrozenxid			29
#define Anum_pg_class_relminmxid			30
#define Anum_pg_class_relacl				31
#define Anum_pg_class_reloptions			32
#define Anum_pg_class_relpartbound			33

/* ----------------
 *		initial contents of pg_class
 *
 * NOTE: only "bootstrapped" relations need to be declared here.  Be sure that
 * the OIDs listed here match those given in their CATALOG macros, and that
 * the relnatts values are correct.
 * ----------------
 */

/*
 * Note: "3" in the relfrozenxid column stands for FirstNormalTransactionId;
 * similarly, "1" in relminmxid stands for FirstMultiXactId
 */
DATA(insert OID = 1247 (  pg_type		PGNSP 71 0 PGUID 0 0 0 0 0 0 0 f f p r 30 0 t f f f f f f t n f 3 1 _null_ _null_ _null_));
DESCR("");
DATA(insert OID = 1249 (  pg_attribute	PGNSP 75 0 PGUID 0 0 0 0 0 0 0 f f p r 22 0 f f f f f f f t n f 3 1 _null_ _null_ _null_));
DESCR("");
DATA(insert OID = 1255 (  pg_proc		PGNSP 81 0 PGUID 0 0 0 0 0 0 0 f f p r 29 0 t f f f f f f t n f 3 1 _null_ _null_ _null_));
DESCR("");
DATA(insert OID = 1259 (  pg_class		PGNSP 83 0 PGUID 0 0 0 0 0 0 0 f f p r 33 0 t f f f f f f t n f 3 1 _null_ _null_ _null_));
DESCR("");


#define		  RELKIND_RELATION		  'r'	/* ordinary table */
#define		  RELKIND_INDEX			  'i'	/* secondary index */
#define		  RELKIND_SEQUENCE		  'S'	/* sequence object */
#define		  RELKIND_TOASTVALUE	  't'	/* for out-of-line values */
#define		  RELKIND_VIEW			  'v'	/* view */
#define		  RELKIND_MATVIEW		  'm'	/* materialized view */
#define		  RELKIND_COMPOSITE_TYPE  'c'	/* composite type */
#define		  RELKIND_FOREIGN_TABLE   'f'	/* foreign table */
#define		  RELKIND_PARTITIONED_TABLE 'p' /* partitioned table */

#define		  RELPERSISTENCE_PERMANENT	'p' /* regular table */
#define		  RELPERSISTENCE_UNLOGGED	'u' /* unlogged permanent table */
#define		  RELPERSISTENCE_TEMP		't' /* temporary table */

/* default selection for replica identity (primary key or nothing) */
#define		  REPLICA_IDENTITY_DEFAULT	'd'
/* no replica identity is logged for this relation */
#define		  REPLICA_IDENTITY_NOTHING	'n'
/* all columns are logged as replica identity */
#define		  REPLICA_IDENTITY_FULL		'f'
/*
 * an explicitly chosen candidate key's columns are used as replica identity.
 * Note this will still be set if the index has been dropped; in that case it
 * has the same meaning as 'd'.
 */
#define		  REPLICA_IDENTITY_INDEX	'i'

#endif							/* PG_CLASS_H */
