/*-------------------------------------------------------------------------
 *
 * pg_tablespace.h
 *	  definition of the system "tablespace" relation (pg_tablespace)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_tablespace.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TABLESPACE_H
#define PG_TABLESPACE_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_tablespace definition.  cpp turns this into
 *		typedef struct FormData_pg_tablespace
 *	主要控制数据分布在不同的磁盘位置
 * ----------------
 */
#define TableSpaceRelationId  1213

/* 参考 http://www.postgres.cn/docs/10/catalog-pg-tablespace.html */
CATALOG(pg_tablespace,1213) BKI_SHARED_RELATION
{
	NameData	spcname;		/* tablespace name */
	/* 创建表空间的用户 */
	Oid			spcowner;		/* owner of tablespace */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* 表空间访问控制权限 */
	aclitem		spcacl[1];		/* access permissions */
	/* 表空间的物理位置（操作系统路径） */
	text		spcoptions[1];	/* per-tablespace options */
#endif
} FormData_pg_tablespace;

/* ----------------
 *		Form_pg_tablespace corresponds to a pointer to a tuple with
 *		the format of pg_tablespace relation.
 * ----------------
 */
typedef FormData_pg_tablespace *Form_pg_tablespace;

/* ----------------
 *		compiler constants for pg_tablespace
 * ----------------
 */

#define Natts_pg_tablespace				4
#define Anum_pg_tablespace_spcname		1
#define Anum_pg_tablespace_spcowner		2
#define Anum_pg_tablespace_spcacl		3
#define Anum_pg_tablespace_spcoptions	4

DATA(insert OID = 1663 ( pg_default PGUID _null_ _null_ ));
DATA(insert OID = 1664 ( pg_global	PGUID _null_ _null_ ));

#define DEFAULTTABLESPACE_OID 1663
#define GLOBALTABLESPACE_OID 1664

#endif							/* PG_TABLESPACE_H */
