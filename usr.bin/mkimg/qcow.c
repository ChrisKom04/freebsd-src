/*-
 * Copyright (c) 2014 Marcel Moolenaar
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

#include <sys/cdefs.h>
#include <sys/errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "endian.h"
#include "image.h"
#include "format.h"
#include "mkimg.h"

/* Default cluster sizes. */
#define	QCOW1_CLSTR_LOG2SZ	12	/* 4KB */
#define	QCOW2_CLSTR_LOG2SZ	16	/* 64KB */

/* Flag bits in cluster offsets */
#define	QCOW_CLSTR_COMPRESSED	(1ULL << 62)
#define	QCOW_CLSTR_COPIED	(1ULL << 63)

struct qcow_header {
	uint32_t	magic;
#define	QCOW_MAGIC		0x514649fb
	uint32_t	version;
#define	QCOW_VERSION_1		1
#define	QCOW_VERSION_2		2
	uint64_t	path_offset;
	uint32_t	path_length;
	uint32_t	clstr_log2sz;	/* v2 only */
	uint64_t	disk_size;
	union {
		struct {
			uint8_t		clstr_log2sz;
			uint8_t		l2_log2sz;
			uint16_t	_pad;
			uint32_t	encryption;
			uint64_t	l1_offset;
		} v1;
		struct {
			uint32_t	encryption;
			uint32_t	l1_entries;
			uint64_t	l1_offset;
			uint64_t	refcnt_offset;
			uint32_t	refcnt_clstrs;
			uint32_t	snapshot_count;
			uint64_t	snapshot_offset;
		} v2;
	} u;
};

struct qcow_info {
	uint64_t	clstr_imgsz;
	uint64_t	clstr_l2tblsz;
	uint64_t	clstr_l1tblsz;
	uint64_t	clstr_rctblsz;
	uint64_t	clstr_rcblks;
	uint64_t	nclstrs;
	uint64_t	ofsflags;

	u_int		l1clno;
	u_int		l2clno;
	u_int		rcclno;
};

static u_int clstr_log2sz;

static uint64_t
round_clstr(uint64_t ofs)
{
	uint64_t clstrsz;

	clstrsz = 1UL << clstr_log2sz;
	return ((ofs + clstrsz - 1) & ~(clstrsz - 1));
}

static int
qcow_resize(lba_t imgsz, u_int version)
{
	uint64_t imagesz;

	switch (version) {
	case QCOW_VERSION_1:
		clstr_log2sz = QCOW1_CLSTR_LOG2SZ;
		break;
	case QCOW_VERSION_2:
		clstr_log2sz = QCOW2_CLSTR_LOG2SZ;
		break;
	default:
		assert(0);
	}

	imagesz = round_clstr(imgsz * secsz);

	if (verbose)
		fprintf(stderr, "QCOW: image size = %ju, cluster size = %u\n",
			(uintmax_t)imagesz, (u_int)(1ULL << clstr_log2sz));

	return (image_set_size(imagesz / secsz));
}

static int
qcow1_resize(lba_t imgsz)
{

	return (qcow_resize(imgsz, QCOW_VERSION_1));
}

static int
qcow2_resize(lba_t imgsz)
{

	return (qcow_resize(imgsz, QCOW_VERSION_2));
}

static struct qcow_info *
qcow_init(u_int version)
{
	struct qcow_info *info;
	uint64_t imagesz, nclstrs, clstr_rcblks, clstr_rctblsz, n;
	lba_t blk_imgsz;

	info = calloc(1, sizeof(struct qcow_info));
	if (info == NULL)
		return (NULL);
	
	blk_imgsz = image_get_size();
	imagesz = blk_imgsz * secsz;
	info->clstr_imgsz = imagesz >> clstr_log2sz;
	info->clstr_l2tblsz = round_clstr(info->clstr_imgsz * 8) >> clstr_log2sz;
	info->clstr_l1tblsz = round_clstr(info->clstr_l2tblsz * 8) >> clstr_log2sz;

	info->l1clno = 1;
	switch (version) {
	case QCOW_VERSION_1:
		info->l2clno = info->l1clno + info->clstr_l1tblsz;
		break;
	case QCOW_VERSION_2:
		nclstrs = info->clstr_imgsz + info->clstr_l2tblsz + info->clstr_l1tblsz + 1;
		clstr_rcblks = clstr_rctblsz = 0;
		do {
			n = clstr_rcblks + clstr_rctblsz;
			clstr_rcblks = round_clstr((nclstrs + n) * 2) >> clstr_log2sz;
			clstr_rctblsz = round_clstr(clstr_rcblks * 8) >> clstr_log2sz;
		} while (n < (clstr_rcblks + clstr_rctblsz));

		info->ofsflags = QCOW_CLSTR_COPIED;
		info->clstr_rctblsz = clstr_rctblsz;
		info->rcclno = info->l1clno + info->clstr_l1tblsz;
		info->l2clno = info->rcclno + info->clstr_rctblsz;
		info->nclstrs = 1 + info->clstr_l1tblsz + info->clstr_rctblsz;
	}

	return (info);
}

static int
qcow_header(int fd, u_int version, struct qcow_info *info)
{
	struct qcow_header *hdr;
	int error;

	error = 0;
	hdr = calloc(1, (1ULL << clstr_log2sz));
	if (hdr == NULL)
		return (errno);

	be32enc(&hdr->magic, QCOW_MAGIC);
	be32enc(&hdr->version, version);
	be64enc(&hdr->disk_size, info->clstr_imgsz << clstr_log2sz);
	switch (version) {
	case QCOW_VERSION_1:
		hdr->u.v1.clstr_log2sz = clstr_log2sz;
		hdr->u.v1.l2_log2sz = clstr_log2sz - 3;
		be64enc(&hdr->u.v1.l1_offset, (1ULL << clstr_log2sz) * info->l1clno);
		break;
	case QCOW_VERSION_2:
		be32enc(&hdr->clstr_log2sz, clstr_log2sz);
		be32enc(&hdr->u.v2.l1_entries, info->clstr_l2tblsz);
		be64enc(&hdr->u.v2.l1_offset, (1ULL << clstr_log2sz) * info->l1clno);
		be64enc(&hdr->u.v2.refcnt_offset, (1ULL << clstr_log2sz) * info->rcclno);
		be32enc(&hdr->u.v2.refcnt_clstrs, info->clstr_rctblsz);
		break;
	default:
		assert(0);
	}

	if (sparse_write(fd, hdr, 1ULL << clstr_log2sz) < 0)
		error = errno;

	free(hdr);
	return (error);
}

static int
qcow_l1tbl(int fd, struct qcow_info *info, uint64_t *ofs, uint64_t **tbl)
{
	uint64_t *l1tbl;
	uint64_t blk, n, reps;
	u_int blk_clstrsz, l1idx;
	int error;

	blk_clstrsz = (1ULL << clstr_log2sz) / secsz;
	l1tbl = calloc(info->clstr_l1tblsz, 1ULL << clstr_log2sz);
	if (l1tbl == NULL)
		return (ENOMEM);

	reps = info->clstr_imgsz;
	for (n = 0; n < reps; n++) {
		blk = n * blk_clstrsz;
		if (image_data(blk, blk_clstrsz)) {
			info->nclstrs++;
			l1idx = n >> (clstr_log2sz - 3);
			if (l1tbl[l1idx] == 0) {
				be64enc(l1tbl + l1idx, *ofs + info->ofsflags);
				*ofs += (1ULL << clstr_log2sz);
				info->nclstrs++;
			}
		}
	}

	error = 0;
	if (sparse_write(fd, l1tbl, (1ULL << clstr_log2sz) * info->clstr_l1tblsz) < 0)
		error = errno;

	*tbl = l1tbl;
	return (error);
}

static void
qcow_rcblks_calc(struct qcow_info *info)
{
	uint64_t clstr_rcblks, n;

	clstr_rcblks = 0;
	do {
		n = clstr_rcblks;
		clstr_rcblks = round_clstr((info->nclstrs + n) * 2) >> clstr_log2sz;
	} while (n < clstr_rcblks);

	info->clstr_rcblks = clstr_rcblks;
}

static int
qcow_rctbl(int fd, struct qcow_info *info, uint64_t *ofs)
{
	uint64_t *rctbl;
	uint64_t n, reps;
	int error;

	error = 0;
	reps = info->clstr_rcblks;
	rctbl = calloc(info->clstr_rctblsz, 1ULL << clstr_log2sz);
	if (rctbl == NULL)
		return (errno);

	for (n = 0; n < reps; n++) {
		be64enc(rctbl + n, *ofs);
		*ofs += (1ULL << clstr_log2sz);
		info->nclstrs++;
	}
	if (sparse_write(fd, rctbl, info->clstr_rctblsz * (1ULL << clstr_log2sz)) < 0)
		error = errno;

	free(rctbl);
	return (error);
}

static int
qcow_l2tbl(int fd, struct qcow_info *info, uint64_t *l1tbl, uint64_t *ofs)
{
	uint64_t *l2tbl;
	uint64_t l1idx, l2idx;
	lba_t blk, blkofs, blk_imgsz;
	u_int blk_clstrsz;
	int error;

	error = 0;
	blk_clstrsz = (1ULL << clstr_log2sz) / secsz;
	blk_imgsz = info->clstr_imgsz * blk_clstrsz;
	l2tbl = malloc(1ULL << clstr_log2sz);
	if (l2tbl == NULL)
		return (ENOMEM);

	for (l1idx = 0; l1idx < info->clstr_l2tblsz; l1idx++) {
		if (l1tbl[l1idx] == 0)
			continue;
		memset(l2tbl, 0, 1ULL << clstr_log2sz);
		blkofs = (lba_t)l1idx * blk_clstrsz << (clstr_log2sz - 3);
		for (l2idx = 0; l2idx < (1ULL << (clstr_log2sz - 3)); l2idx++) {
			blk = blkofs + (lba_t)l2idx * blk_clstrsz;
			if (blk >= blk_imgsz)
				break;
			if (image_data(blk, blk_clstrsz)) {
				be64enc(l2tbl + l2idx, *ofs + info->ofsflags);
				*ofs += (1ULL << clstr_log2sz);
			}
		}
		if (sparse_write(fd, l2tbl, (1ULL << clstr_log2sz)) < 0) {
			error = errno;
			break;
		}
	}

	free(l2tbl);
	return (error);
}

static int
qcow_rcblks(int fd, struct qcow_info *info)
{
	uint16_t *rcblk;
	uint64_t n;
	int error;

	error = 0;
	rcblk = calloc(1, 1ULL << clstr_log2sz);
	if (rcblk == NULL)
		return (ENOMEM);

	for (n = 0; n < info->nclstrs; n++) {
		be16enc(rcblk + n % (1ULL << (clstr_log2sz - 1)), 1);
		if ((n + 1) % (1ULL << (clstr_log2sz - 1)) == 0) {
			if (sparse_write(fd, rcblk, (1ULL << clstr_log2sz)) < 0) {
				error = errno;
				goto out;
			}
			memset(rcblk, 0, 1ULL << clstr_log2sz);
		}
	}

	if (n % (1ULL << (clstr_log2sz - 1)) != 0) {
		if (sparse_write(fd, rcblk, (1ULL << clstr_log2sz)) < 0)
			error = errno;
	}

out:
	free(rcblk);
	return (error);
}

static int
qcow_copyout(int fd, struct qcow_info *info)
{
	uint64_t n;
	lba_t blk;
	u_int blk_clstrsz;
	int error;

	error = 0;
	blk_clstrsz = (1ULL << clstr_log2sz) / secsz;
	for (n = 0; n < info->clstr_imgsz; n++) {
		blk = n * blk_clstrsz;
		if (image_data(blk, blk_clstrsz)) {
			error = image_copyout_region(fd, blk, blk_clstrsz);
			if (error != 0)
				break;
		}
	}
	if (!error)
		error = image_copyout_done(fd);

	return (error);
}

static int
qcow_write(int fd, u_int version)
{
	struct qcow_info *info;
	uint64_t *l1tbl;
	uint64_t ofs;
	int error;

	l1tbl = NULL;
	info = qcow_init(version);
	if (info == NULL) {
		error = errno;
		goto out;
	}

	error = qcow_header(fd, version, info);
	if (error != 0)
		goto out;

	ofs = info->l2clno * (1ULL << clstr_log2sz);
	error = qcow_l1tbl(fd, info, &ofs, &l1tbl);
	if (error != 0)
		goto out;

	if (version != QCOW_VERSION_1) {
		qcow_rcblks_calc(info);
		error = qcow_rctbl(fd, info, &ofs);
		if (error != 0)
			goto out;
	}

	error = qcow_l2tbl(fd, info, l1tbl, &ofs);
	if (error != 0)
		goto out;

	if (version != QCOW_VERSION_1) {
		error = qcow_rcblks(fd, info);
		if (error != 0)
			goto out;
	}

	error = qcow_copyout(fd, info);

out:
	if (info != NULL)
		free(info);
	if (l1tbl != NULL)
		free(l1tbl);

	return (error);
}

static int
qcow1_write(int fd)
{

	return (qcow_write(fd, QCOW_VERSION_1));
}

static int
qcow2_write(int fd)
{

	return (qcow_write(fd, QCOW_VERSION_2));
}

static struct mkimg_format qcow1_format = {
	.name = "qcow",
	.description = "QEMU Copy-On-Write, version 1",
	.resize = qcow1_resize,
	.write = qcow1_write,
};
FORMAT_DEFINE(qcow1_format);

static struct mkimg_format qcow2_format = {
	.name = "qcow2",
	.description = "QEMU Copy-On-Write, version 2",
	.resize = qcow2_resize,
	.write = qcow2_write,
};
FORMAT_DEFINE(qcow2_format);
