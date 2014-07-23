/*
 * Copyright 2008+ Evgeniy Polyakov <zbr@ioremap.net>
 *
 * This file is part of Elliptics.
 * 
 * Elliptics is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Elliptics is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with Elliptics.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _XOPEN_SOURCE 600

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <eblob/blob.h>

#include "elliptics/packet.h"
#include "elliptics/interface.h"
#include "elliptics/backends.h"

#include "common.h"

/*
 * FIXME: __unused is used internally by glibc, so it may cause conflicts.
 */
#ifndef __unused
#define __unused	__attribute__ ((unused))
#endif

#if EBLOB_ID_SIZE != DNET_ID_SIZE
#error "EBLOB_ID_SIZE must be equal to DNET_ID_SIZE"
#endif


struct eblob_read_params {
	int			fd;
	int			pad;
	uint64_t		offset;
};

static int eblob_read_params_compare(const void *p1, const void *p2)
{
	const struct eblob_read_params *r1 = p1;
	const struct eblob_read_params *r2 = p2;
	int ret;

	ret = r1->fd - r2->fd;
	if (ret != 0)
		return ret;

	if (r1->offset > r2->offset)
		return 1;
	if (r1->offset < r2->offset)
		return -1;

	return 0;
}

struct eblob_backend_config {
	struct eblob_config		data;
	struct eblob_backend		*eblob;

	pthread_mutex_t			last_read_lock;
	int64_t				vm_total;		/* squared in bytes */
	int				random_access;
	int				last_read_index;
	struct eblob_read_params	last_reads[100];
};

/* Pre-callback that formats arguments and calls ictl->callback */
static int blob_iterate_callback(struct eblob_disk_control *dc,
		struct eblob_ram_control *rctl __unused,
		void *data, void *priv, void *thread_priv __unused)
{
	struct dnet_iterator_ctl *ictl = priv;
	struct dnet_ext_list elist;
	uint64_t size;
	int err;

	assert(dc != NULL);
	assert(data != NULL);

	size = dc->data_size;
	dnet_ext_list_init(&elist);

	/* If it's an extended record - extract header, move data pointer */
	if (dc->flags & BLOB_DISK_CTL_EXTHDR) {
		err = dnet_ext_list_extract((void *)&data, &size, &elist,
				DNET_EXT_DONT_FREE_ON_DESTROY);
		if (err != 0)
			goto err;
	}

	err = ictl->callback(ictl->callback_private, (struct dnet_raw_id *)&dc->key,
			data, size, &elist);

err:
	dnet_ext_list_destroy(&elist);
	return err;
}

/* Eblob-specific data/metadata iterator */
static int blob_iterate(struct eblob_backend_config *c, struct dnet_iterator_ctl *ictl)
{
	/* Sanity */
	assert(c != NULL);
	assert(ictl != NULL);

	/* Init iterator config */
	struct eblob_backend *b = c->eblob;
	struct eblob_iterate_control eictl = {
		.priv = ictl,
		.b = b,
		.log = c->data.log,
		.flags = EBLOB_ITERATE_FLAGS_ALL | EBLOB_ITERATE_FLAGS_READONLY,
		.iterator_cb = {
			.iterator = blob_iterate_callback,
		},
	};

	return eblob_iterate(b, &eictl);
}

static int blob_write(struct eblob_backend_config *c, void *state,
		struct dnet_cmd *cmd, void *data)
{
	struct dnet_ext_list elist;
	struct dnet_io_attr *io = data;
	struct eblob_backend *b = c->eblob;
	struct eblob_write_control wc = { .data_fd = -1 };
	struct eblob_key key;
	struct dnet_ext_list_hdr ehdr;
	uint64_t flags = BLOB_DISK_CTL_EXTHDR;
	uint64_t fd_offset;
	static const size_t ehdr_size = sizeof(struct dnet_ext_list_hdr);
	int err;

	dnet_backend_log(DNET_LOG_NOTICE, "%s: EBLOB: blob-write: WRITE: start: offset: %llu, size: %llu, ioflags: 0x%x.\n",
		dnet_dump_id_str(io->id), (unsigned long long)io->offset, (unsigned long long)io->size, io->flags);

	dnet_convert_io_attr(io);

	dnet_ext_list_init(&elist);
	dnet_ext_io_to_list(io, &elist);
	dnet_ext_list_to_hdr(&elist, &ehdr);

	data += sizeof(struct dnet_io_attr);

	if (io->flags & DNET_IO_FLAGS_COMPRESS) {
		err = -ENOTSUP;
		goto err_out_exit;
	}

	if (io->flags & DNET_IO_FLAGS_APPEND)
		flags |= BLOB_DISK_CTL_APPEND;

	if (io->flags & DNET_IO_FLAGS_NOCSUM)
		flags |= BLOB_DISK_CTL_NOCSUM;

	memcpy(key.id, io->id, EBLOB_ID_SIZE);

	if (io->flags & DNET_IO_FLAGS_PREPARE) {
		err = eblob_write_prepare(b, &key, io->num + ehdr_size, flags);
		if (err) {
			dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-write: eblob_write_prepare: "
					"size: %" PRIu64 ": %s %d\n", dnet_dump_id_str(io->id),
					io->num + ehdr_size, strerror(-err), err);
			goto err_out_exit;
		}

		dnet_backend_log(DNET_LOG_NOTICE, "%s: EBLOB: blob-write: eblob_write_prepare: "
				"size: %" PRIu64 ": Ok\n", dnet_dump_id_str(io->id), io->num + ehdr_size);
	}

	if (io->size) {
		const struct eblob_iovec iov[2] = {
			{ .offset = 0, .size = ehdr_size, .base = &ehdr },
			{ .offset = ehdr_size + io->offset, .size = io->size, .base = data },
		};

		if (io->flags & DNET_IO_FLAGS_PLAIN_WRITE) {
			err = eblob_plain_writev(b, &key, iov, 2, flags);
		} else {
			err = eblob_writev_return(b, &key, iov, 2, flags, &wc);
		}

		if (err) {
			dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-write: WRITE: %d: %s\n",
				dnet_dump_id_str(io->id), err, strerror(-err));
			goto err_out_exit;
		}

		dnet_backend_log(DNET_LOG_NOTICE, "%s: EBLOB: blob-write: WRITE: Ok: "
				"offset: %" PRIu64 ", size: %" PRIu64 ".\n",
				dnet_dump_id_str(io->id), io->offset, io->size);
	}

	if (io->flags & DNET_IO_FLAGS_COMMIT) {
		if (io->flags & DNET_IO_FLAGS_PLAIN_WRITE) {
			err = eblob_write_commit(b, &key, io->num + ehdr_size, flags);
			if (err) {
				dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-write: eblob_write_commit: "
						"size: %" PRIu64 ": %s %d\n", dnet_dump_id_str(io->id),
						io->num, strerror(-err), err);
				goto err_out_exit;
			}

			dnet_backend_log(DNET_LOG_NOTICE, "%s: EBLOB: blob-write: eblob_write_commit: "
					"size: %" PRIu64 ": Ok\n", dnet_dump_id_str(io->id), io->num);
		}
	}

	if (!err && wc.data_fd == -1) {
		err = eblob_read_return(b, &key, EBLOB_READ_NOCSUM, &wc);
		if (err) {
			dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-write: eblob_read: "
					"size: %" PRIu64 ": %s %d\n", dnet_dump_id_str(io->id),
					io->num, strerror(-err), err);
			goto err_out_exit;
		}
	}

	if (io->flags & DNET_IO_FLAGS_WRITE_NO_FILE_INFO) {
		cmd->flags |= DNET_FLAGS_NEED_ACK;
		err = 0;
		goto err_out_exit;
	}

	fd_offset = wc.ctl_data_offset + sizeof(struct eblob_disk_control);
	if (wc.flags & BLOB_DISK_CTL_EXTHDR)
		fd_offset += ehdr_size;

	err = dnet_send_file_info_ts(state, cmd, wc.data_fd, fd_offset, wc.size, &elist.timestamp);
	if (err) {
		dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-write: dnet_send_file_info: "
				"fd: %d, offset: %" PRIu64 ", offset-within-fd: %" PRIu64 ", size: %" PRIu64 ": %s %d\n",
				dnet_dump_id_str(io->id), wc.data_fd, wc.offset, fd_offset, wc.size,
				strerror(-err), err);
		goto err_out_exit;
	}

	dnet_backend_log(DNET_LOG_INFO, "%s: EBLOB: blob-write: fd: %d, offset: %" PRIu64 ", offset-within-fd: %" PRIu64 ", size: %" PRIu64 "\n",
			dnet_dump_id_str(io->id), wc.data_fd, wc.offset, fd_offset, wc.size);

err_out_exit:
	dnet_ext_list_destroy(&elist);
	return err;
}


static int blob_read(struct eblob_backend_config *c, void *state, struct dnet_cmd *cmd, void *data, int last)
{
	struct dnet_ext_list elist;
	struct dnet_io_attr *io = data;
	struct eblob_backend *b = c->eblob;
	struct eblob_key key;
	struct eblob_write_control wc;
	uint64_t offset = 0, size = 0;
	enum eblob_read_flavour csum = EBLOB_READ_CSUM;
	int err, fd = -1, on_close = 0;

	dnet_ext_list_init(&elist);
	dnet_convert_io_attr(io);

	memcpy(key.id, io->id, EBLOB_ID_SIZE);

	if (io->flags & DNET_IO_FLAGS_NOCSUM)
		csum = EBLOB_READ_NOCSUM;

	err = eblob_read_return(b, &key, csum, &wc);
	if (err == 0) {
		/* Existing entry */
		offset = wc.data_offset;
		size = wc.total_data_size;
		fd = wc.data_fd;

		/* Existing new-format entry */
		if ((wc.flags & BLOB_DISK_CTL_EXTHDR) != 0) {
			struct dnet_ext_list_hdr ehdr;

			err = dnet_ext_hdr_read(&ehdr, fd, offset);
			if (err != 0)
				goto err_out_exit;
			dnet_ext_hdr_to_list(&ehdr, &elist);
			dnet_ext_list_to_io(&elist, io);

			/* Take into an account extended header */
			size -= sizeof(struct dnet_ext_list_hdr);
			offset += sizeof(struct dnet_ext_list_hdr);
		}
	} else {
		dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-read-fd: READ: %d: %s\n",
			dnet_dump_id_str(io->id), err, strerror(-err));
		goto err_out_exit;
	}

	io->total_size = size;

	if (io->offset) {
		if (io->offset >= size) {
			err = -E2BIG;
			goto err_out_exit;
		}
		offset += io->offset;
		size -= io->offset;
	}

	if (io->size != 0 && size > io->size)
		size = io->size;
	else
		io->size = size;

	if (size && last)
		cmd->flags &= ~DNET_FLAGS_NEED_ACK;

	if (fd >= 0) {
		struct eblob_read_params *p, *prev;
		int i;

		pthread_mutex_lock(&c->last_read_lock);
		p = &c->last_reads[c->last_read_index];

		if (++c->last_read_index >= (int)ARRAY_SIZE(c->last_reads)) {
			int64_t tmp;
			int64_t mult = 1;
			int64_t mean = 0;
			int old_ra;

			qsort(c->last_reads, ARRAY_SIZE(c->last_reads), sizeof(struct eblob_read_params), eblob_read_params_compare);

			prev = &c->last_reads[0];
			tmp = prev->offset;

			for (i = 1; i < (int)ARRAY_SIZE(c->last_reads); ++i) {
				p = &c->last_reads[i];

				if (p->fd != prev->fd)
					mult++;

				tmp += p->offset * mult;
				prev = p;
			}

			/* found mean offset */
			mean = tmp / ARRAY_SIZE(c->last_reads);

			/* calculating mean squared error */
			tmp = 0;
			for (i = 0; i < (int)ARRAY_SIZE(c->last_reads); ++i) {
				p = &c->last_reads[i];

				tmp += ((int64_t)p->offset - mean) * ((int64_t)p->offset - mean);
			}
			tmp /= ARRAY_SIZE(c->last_reads);

			/*
			 * tmp and vm_total are squared, so if this check is true,
			 * mean offset difference (error) is more than 25% of RAM
			 */
			old_ra = c->random_access;
			if (tmp > c->vm_total / 16)
				c->random_access = 1;
			else
				c->random_access = 0;

			if (old_ra != c->random_access) {
				dnet_backend_log(DNET_LOG_ERROR, "EBLOB: switch RA %d -> %d, offset MSE: %llu, squared VM total: %llu\n",
						old_ra, c->random_access, (unsigned long long)tmp, (unsigned long long)c->vm_total);
			}

			c->last_read_index = 0;
		}

		p->fd = fd;
		p->offset = offset;
		pthread_mutex_unlock(&c->last_read_lock);
	}

	if (c->random_access)
		on_close = DNET_IO_REQ_FLAGS_CACHE_FORGET;

	err = dnet_send_read_data(state, cmd, io, NULL, fd, offset, on_close);

err_out_exit:
	dnet_ext_list_destroy(&elist);
	return err;
}

struct eblob_read_range_priv {
	void			*state;
	struct dnet_cmd		*cmd;
	struct eblob_range_request	*keys;
	uint64_t		keys_size;
	uint64_t		keys_cnt;
	uint32_t		flags;
};

static int blob_cmp_range_request(const void *req1, const void *req2)
{
	return memcmp(((struct eblob_range_request *)(req1))->record_key, ((struct eblob_range_request *)(req2))->record_key, EBLOB_ID_SIZE);
}

static int blob_read_range_callback(struct eblob_range_request *req)
{
	struct eblob_read_range_priv *p = req->priv;
	struct dnet_io_attr io;
	int err;

	if (req->requested_offset > req->record_size) {
		err = 0;
		goto err_out_exit;
	}

	if (!(p->flags & DNET_IO_FLAGS_NODATA)) {
		struct eblob_write_control wc;

		io.flags = 0;
		io.size = req->record_size - req->requested_offset;
		io.offset = req->requested_offset;

		/* FIXME: This is slow! */
		err = eblob_read_return(req->back, (struct eblob_key *)req->record_key,
				EBLOB_READ_NOCSUM, &wc);
		if (err)
			goto err_out_exit;

		if (wc.flags & BLOB_DISK_CTL_EXTHDR) {
			struct dnet_ext_list_hdr ehdr;
			struct dnet_ext_list elist;

			err = dnet_ext_hdr_read(&ehdr, req->record_fd, req->record_offset);
			if (err != 0)
				goto err_out_exit;

			dnet_ext_hdr_to_list(&ehdr, &elist);
			dnet_ext_list_to_io(&elist, &io);

			io.offset += sizeof(struct dnet_ext_list_hdr);
			io.size -= sizeof(struct dnet_ext_list_hdr);
		}

		memcpy(io.id, req->record_key, DNET_ID_SIZE);
		memcpy(io.parent, req->end, DNET_ID_SIZE);

		err = dnet_send_read_data(p->state, p->cmd, &io, NULL, req->record_fd,
				req->record_offset + io.offset, 0);
		if (!err)
			req->current_pos++;
	} else {
		req->current_pos++;
		err = 0;
	}

err_out_exit:
	return err;
}

static int blob_del_range_callback(struct eblob_range_request *req)
{
	struct eblob_key key;
	int err;

	dnet_backend_log(DNET_LOG_DEBUG, "%s: EBLOB: blob-read-range: DEL\n",
			dnet_dump_id_str(req->record_key));

	memcpy(key.id, req->record_key, EBLOB_ID_SIZE);
	err = eblob_remove(req->back, &key);
	if (err) {
		dnet_backend_log(DNET_LOG_DEBUG, "%s: EBLOB: blob-read-range: DEL: err: %d\n",
				dnet_dump_id_str(req->record_key), err);
	}

	return err;
}

static int blob_range_callback(struct eblob_range_request *req)
{
	struct eblob_read_range_priv *p = req->priv;
	int len = 10;
	char start_id[len*2+1], end_id[len*2+1], cur_id[2*len+1];
	int err = 0;

	dnet_dump_id_len_raw(req->start, len, start_id);
	dnet_dump_id_len_raw(req->end, len, end_id);
	dnet_dump_id_len_raw(req->record_key, len, cur_id);

	dnet_backend_log(DNET_LOG_NOTICE, "%s: EBLOB: blob-range: limit: %llu [%llu, %llu]: "
			"start: %s, end: %s: io record/requested: offset: %llu/%llu, size: %llu/%llu\n",
			cur_id,
			(unsigned long long)req->current_pos,
			(unsigned long long)req->requested_limit_start, (unsigned long long)req->requested_limit_num,
			start_id, end_id,
			(unsigned long long)req->record_offset, (unsigned long long)req->requested_offset,
			(unsigned long long)req->record_size, (unsigned long long)req->requested_size);

	if (req->requested_offset > req->record_size) {
		err = 0;
		goto err_out_exit;
	}

	if (p->keys_size == p->keys_cnt) {
		/* On first pass allocate 1000, otherwise double allocation size */
		p->keys_size = p->keys_size ? p->keys_size * 2 : 1000;
		p->keys = realloc(p->keys, sizeof(struct eblob_range_request) * p->keys_size);
		if (p->keys == NULL) {
			err = -ENOMEM;
			dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-del-range: can't (re-)allocate memory, "
					"new size: %" PRIu64 "\n", cur_id, p->keys_size);
			goto err_out_exit;
		}
	}

	memcpy(&p->keys[p->keys_cnt], req, sizeof(struct eblob_range_request));
	dnet_dump_id_len_raw(p->keys[p->keys_cnt].record_key, len, cur_id);
	dnet_backend_log(DNET_LOG_DEBUG, "%s: count: %llu\n", cur_id, (unsigned long long)(p->keys_cnt));
	p->keys_cnt++;

	if (!err)
		req->current_pos++;
err_out_exit:
	return err;
}

static int blob_read_range(struct eblob_backend_config *c, void *state, struct dnet_cmd *cmd, void *data)
{
	struct eblob_read_range_priv p;
	struct dnet_io_attr *io = data;
	struct eblob_backend *b = c->eblob;
	struct eblob_range_request req;
	uint64_t i, start_from = 0;
	int err;

	memset(&p, 0, sizeof(p));

	p.cmd = cmd;
	p.state = state;
	p.keys = NULL;
	p.keys_size= 0;
	p.keys_cnt = 0;
	p.flags = io->flags;

	dnet_convert_io_attr(io);

	memset(&req, 0, sizeof(req));

	memcpy(req.start, io->id, EBLOB_ID_SIZE);
	memcpy(req.end, io->parent, EBLOB_ID_SIZE);
	req.requested_offset = io->offset;
	req.requested_size = io->size;
	req.requested_limit_start = 0;
	req.requested_limit_num = ~0ULL;

	req.callback = blob_range_callback;
	req.back = b;
	req.priv = &p;

	err = eblob_read_range(&req);
	if (err) {
		dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-read-range: %d: %s\n",
			dnet_dump_id_str(io->id), err, strerror(-err));
		goto err_out_exit;
	}

	if ((cmd->cmd == DNET_CMD_READ_RANGE) && (cmd->flags & DNET_ATTR_SORT)) {
		dnet_backend_log(DNET_LOG_DEBUG, "Sorting keys before sending\n");
		qsort(p.keys, p.keys_cnt, sizeof(struct eblob_range_request), &blob_cmp_range_request);
	}

	if (cmd->cmd == DNET_CMD_READ_RANGE) {
		start_from = io->start;
	}

	for (i = start_from; i < p.keys_cnt; ++i) {
		switch(cmd->cmd) {
			case DNET_CMD_READ_RANGE:
				if ((io->num > 0) && (i >= (io->num + start_from)))
					break;
				dnet_backend_log(DNET_LOG_DEBUG, "%s: EBLOB: blob-read-range: READ\n",
						dnet_dump_id_str(p.keys[i].record_key));
				err = blob_read_range_callback(&p.keys[i]);
				break;
			case DNET_CMD_DEL_RANGE:
				dnet_backend_log(DNET_LOG_DEBUG, "%s: EBLOB: blob-read-range: DEL\n",
						dnet_dump_id_str(p.keys[i].record_key));
				err = blob_del_range_callback(&p.keys[i]);
				break;
		}

		if (err) {
			dnet_backend_log(DNET_LOG_DEBUG, "%s: EBLOB: blob-read-range: err: %d\n",
					dnet_dump_id_str(p.keys[i].record_key), err);
			goto err_out_exit;
		}
	}

	if (req.current_pos) {
		struct dnet_io_attr r;

		memcpy(&r, io, sizeof(struct dnet_io_attr));
		r.num = req.current_pos - start_from;
		r.offset = r.size = 0;

		err = dnet_send_read_data(state, cmd, &r, NULL, -1, 0, 0);
	}

err_out_exit:
	if (p.keys)
		free(p.keys);

	return err;
}

static int blob_del(struct eblob_backend_config *c, struct dnet_cmd *cmd)
{
	struct eblob_key key;
	int err;

	memcpy(key.id, cmd->id.id, EBLOB_ID_SIZE);

	err = eblob_remove(c->eblob, &key);
	if (err) {
		dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-del: REMOVE: %d: %s\n",
			dnet_dump_id_str(cmd->id.id), err, strerror(-err));
	}

	return err;
}

static int blob_file_info(struct eblob_backend_config *c, void *state, struct dnet_cmd *cmd)
{
	struct eblob_backend *b = c->eblob;
	struct eblob_key key;
	struct eblob_write_control wc;
	struct dnet_ext_list elist;
	static const size_t ehdr_size = sizeof(struct dnet_ext_list_hdr);
	uint64_t offset, size;
	int fd, err;

	dnet_ext_list_init(&elist);

	memcpy(key.id, cmd->id.id, EBLOB_ID_SIZE);
	err = eblob_read_return(b, &key, EBLOB_READ_NOCSUM, &wc);
	if (err < 0) {
		dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-file-info: info-read: %d: %s.\n",
				dnet_dump_id(&cmd->id), err, strerror(-err));
		goto err_out_exit;
	}

	offset = wc.data_offset;
	size = wc.total_data_size;
	fd = wc.data_fd;

	if ((wc.flags & BLOB_DISK_CTL_EXTHDR) != 0) {
		struct dnet_ext_list_hdr ehdr;

		/* Sanity */
		if (size < ehdr_size) {
			err = -ERANGE;
			goto err_out_exit;
		}

		err = dnet_ext_hdr_read(&ehdr, fd, offset);
		if (err != 0)
			goto err_out_exit;
		dnet_ext_hdr_to_list(&ehdr, &elist);

		/* Take into an account extended header's len */
		size -= ehdr_size;
		offset += ehdr_size;
	}

	if (size == 0) {
		err = -ENOENT;
		dnet_backend_log(DNET_LOG_INFO, "%s: EBLOB: blob-file-info: info-read: ZERO-SIZE-FILE.\n",
				dnet_dump_id(&cmd->id));
		goto err_out_exit;
	}

	err = dnet_send_file_info_ts(state, cmd, fd, offset, size, &elist.timestamp);

err_out_exit:
	dnet_ext_list_destroy(&elist);
	return err;
}

static int eblob_backend_checksum(struct dnet_node *n, void *priv, struct dnet_id *id, void *csum, int *csize) {
	struct eblob_backend_config *c = priv;
	struct eblob_backend *b = c->eblob;
	struct eblob_write_control wc;
	struct eblob_key key;
	static const size_t ehdr_size = sizeof(struct dnet_ext_list_hdr);
	int err;

	memcpy(key.id, id->id, EBLOB_ID_SIZE);
	err = eblob_read_return(b, &key, EBLOB_READ_NOCSUM, &wc);
	if (err < 0) {
		dnet_backend_log(DNET_LOG_ERROR, "%s: EBLOB: blob-checksum: read: %d: %s.\n",
							dnet_dump_id_str(id->id), err, strerror(-err));
		goto err_out_exit;
	}
	err = 0;

	if (wc.flags & BLOB_DISK_CTL_EXTHDR) {
		/* Sanity */
		if (wc.total_data_size < ehdr_size) {
			err = -EINVAL;
			goto err_out_exit;
		}
		wc.data_offset += ehdr_size;
		wc.total_data_size -= ehdr_size;
	}

	if (wc.total_data_size == 0)
		memset(csum, 0, *csize);
	else
		err = dnet_checksum_fd(n, wc.data_fd, wc.data_offset,
				wc.total_data_size, csum, *csize);

err_out_exit:
	return err;
}

static int blob_start_defrag(struct eblob_backend_config *c, struct dnet_cmd *cmd, void *data)
{
	struct dnet_defrag_ctl *ctl = data;
	int err;

	if (cmd->size != sizeof(struct dnet_defrag_ctl)) {
		err = -EPROTO;
		dnet_backend_log(DNET_LOG_ERROR, "DEFRAG: invalid defragmetation request: cmd-size: %llu, must-be: %zu\n",
				(unsigned long long)cmd->size, sizeof(struct dnet_defrag_ctl));
		goto err_out_exit;
	}

	dnet_convert_defrag_ctl(ctl);

	if (ctl->flags & DNET_DEFRAG_FLAGS_STATUS) {
		ctl->status = eblob_defrag_status(c->eblob);
	} else {
		ctl->status = eblob_start_defrag(c->eblob);
	}

	dnet_backend_log(DNET_LOG_INFO, "DEFRAG: defragmetation request: flags: 0x%llx, status: %d\n",
			(unsigned long long)ctl->flags, ctl->status);

	err = ctl->status;

err_out_exit:
	return err;
}

static int eblob_backend_command_handler(void *state, void *priv, struct dnet_cmd *cmd, void *data)
{
	int err;
	struct eblob_backend_config *c = priv;
	char *path, *p;

	switch (cmd->cmd) {
		case DNET_CMD_LOOKUP:
			err = blob_file_info(c, state, cmd);
			break;
		case DNET_CMD_WRITE:
			err = blob_write(c, state, cmd, data);
			break;
		case DNET_CMD_READ:
			err = blob_read(c, state, cmd, data, 1);
			break;
		case DNET_CMD_READ_RANGE:
		case DNET_CMD_DEL_RANGE:
			err = blob_read_range(c, state, cmd, data);
			break;
		case DNET_CMD_STAT:
			path = strdup(c->data.file);
			if (!path) {
				err = -ENOMEM;
				break;
			}

			p = strrchr(path, '/');
			if (p) {
				*p = '\0';
			} else {
				free(path);
				path = NULL;
			}

			err = backend_stat(state, path, cmd);
			free(path);
			break;
		case DNET_CMD_DEL:
			err = blob_del(c, cmd);
			break;
		case DNET_CMD_DEFRAG:
			err = blob_start_defrag(c, cmd, data);
			break;
		default:
			err = -ENOTSUP;
			break;
	}

	return err;
}

static int dnet_blob_set_sync(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct eblob_backend_config *c = b->data;

	c->data.sync = atoi(value);
	return 0;
}

static int dnet_blob_set_data(struct dnet_config_backend *b, char *key __unused, char *file)
{
	struct eblob_backend_config *c = b->data;
	int err;

	err = backend_storage_size(b, file);
	if (err) {
		char root[strlen(file)+1], *ptr;

		snprintf(root, sizeof(root), "%s", file);
		ptr = strrchr(root, '/');
		if (ptr) {
			*ptr = '\0';
			err = backend_storage_size(b, root);
		}

		if (err)
			return err;
	}

	free(c->data.file);
	c->data.file = strdup(file);
	if (!c->data.file)
		return -ENOMEM;

	return 0;
}

static int dnet_blob_set_blob_size(struct dnet_config_backend *b, char *key, char *value)
{
	struct eblob_backend_config *c = b->data;
	uint64_t val = strtoul(value, NULL, 0);

	if (strchr(value, 'T'))
		val *= 1024*1024*1024*1024ULL;
	else if (strchr(value, 'G'))
		val *= 1024*1024*1024ULL;
	else if (strchr(value, 'M'))
		val *= 1024*1024;
	else if (strchr(value, 'K'))
		val *= 1024;

	if (!strcmp(key, "blob_size"))
		c->data.blob_size = val;
	else if (!strcmp(key, "blob_size_limit"))
		c->data.blob_size_limit = val;

	return 0;
}

static int dnet_blob_set_index_block_size(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct eblob_backend_config *c = b->data;

	c->data.index_block_size = strtoul(value, NULL, 0);
	return 0;
}

static int dnet_blob_set_index_block_bloom_length(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct eblob_backend_config *c = b->data;

	c->data.index_block_bloom_length = strtoul(value, NULL, 0);
	return 0;
}

static int dnet_blob_set_records_in_blob(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct eblob_backend_config *c = b->data;
	uint64_t val = strtoul(value, NULL, 0);

	c->data.records_in_blob = val;
	return 0;
}

static int dnet_blob_set_defrag_timeout(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct eblob_backend_config *c = b->data;

	c->data.defrag_timeout = strtoul(value, NULL, 0);
	return 0;
}

static int dnet_blob_set_defrag_time(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct eblob_backend_config *c = b->data;

	c->data.defrag_time = strtoul(value, NULL, 0);
	return 0;
}

static int dnet_blob_set_defrag_splay(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct eblob_backend_config *c = b->data;

	c->data.defrag_splay = strtoul(value, NULL, 0);
	return 0;
}

static int dnet_blob_set_defrag_percentage(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct eblob_backend_config *c = b->data;

	c->data.defrag_percentage = strtoul(value, NULL, 0);
	return 0;
}

static int dnet_blob_set_blob_flags(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct eblob_backend_config *c = b->data;

	c->data.blob_flags = strtoul(value, NULL, 0);
	return 0;
}

int eblob_backend_storage_stat(void *priv, struct dnet_stat *st)
{
	int err;
	struct eblob_backend_config *r = priv;

	memset(st, 0, sizeof(struct dnet_stat));

	err = backend_stat_low_level(r->data.file, st);
	if (err) {
		char root[strlen(r->data.file)+1], *ptr;

		snprintf(root, sizeof(root), "%s", r->data.file);
		ptr = strrchr(root, '/');
		if (ptr) {
			*ptr = '\0';
			err = backend_stat_low_level(root, st);
		}

		if (err)
			return err;
	}

	st->node_files = eblob_total_elements(r->eblob);
	st->node_files_removed = eblob_stat_get_summary(r->eblob, EBLOB_LST_RECORDS_REMOVED);

	return 0;
}

static void eblob_backend_cleanup(void *priv)
{
	struct eblob_backend_config *c = priv;

	eblob_cleanup(c->eblob);

	pthread_mutex_destroy(&c->last_read_lock);
	free(c->data.file);
}

static int dnet_eblob_iterator(struct dnet_iterator_ctl *ictl)
{
	struct eblob_backend_config *c = ictl->iterate_private;
	return blob_iterate(c, ictl);
}

static int dnet_blob_config_init(struct dnet_config_backend *b, struct dnet_config *cfg)
{
	struct eblob_backend_config *c = b->data;
	struct dnet_stat st;
	int err = 0;

	if (!c->data.file) {
		dnet_backend_log(DNET_LOG_ERROR, "blob: no data file present. Exiting.\n");
		err = -EINVAL;
		goto err_out_exit;
	}

	c->data.log = (struct eblob_log *)b->log;

	err = pthread_mutex_init(&c->last_read_lock, NULL);
	if (err) {
		err = -err;
		dnet_backend_log(DNET_LOG_ERROR, "blob: could not create last-read lock: %d.\n", err);
		goto err_out_exit;
	}

	c->eblob = eblob_init(&c->data);
	if (!c->eblob) {
		err = -EINVAL;
		goto err_out_last_read_lock_destroy;
	}

	memset(&st, 0, sizeof(struct dnet_stat));
	err = eblob_backend_storage_stat(c, &st);
	if (err)
		goto err_out_last_read_lock_destroy;

	c->vm_total = st.vm_total * st.vm_total * 1024 * 1024;

	cfg->cb = &b->cb;
	cfg->storage_size = b->storage_size;
	cfg->storage_free = b->storage_free;
	b->cb.storage_stat = eblob_backend_storage_stat;

	b->cb.command_private = c;
	b->cb.command_handler = eblob_backend_command_handler;
	b->cb.backend_cleanup = eblob_backend_cleanup;
	b->cb.checksum = eblob_backend_checksum;

	b->cb.iterator = dnet_eblob_iterator;

	return 0;

err_out_last_read_lock_destroy:
	pthread_mutex_destroy(&c->last_read_lock);
err_out_exit:
	return err;
}

static void dnet_blob_config_cleanup(struct dnet_config_backend *b)
{
	struct eblob_backend_config *c = b->data;

	eblob_backend_cleanup(c);
}

static struct dnet_config_entry dnet_cfg_entries_blobsystem[] = {
	{"sync", dnet_blob_set_sync},
	{"data", dnet_blob_set_data},
	{"blob_flags", dnet_blob_set_blob_flags},
	{"blob_size", dnet_blob_set_blob_size},
	{"records_in_blob", dnet_blob_set_records_in_blob},
	{"defrag_timeout", dnet_blob_set_defrag_timeout},
	{"defrag_time", dnet_blob_set_defrag_time},
	{"defrag_splay", dnet_blob_set_defrag_splay},
	{"defrag_percentage", dnet_blob_set_defrag_percentage},
	{"blob_size_limit", dnet_blob_set_blob_size},
	{"index_block_size", dnet_blob_set_index_block_size},
	{"index_block_bloom_length", dnet_blob_set_index_block_bloom_length},
};

static struct dnet_config_backend dnet_eblob_backend = {
	.name			= "blob",
	.ent			= dnet_cfg_entries_blobsystem,
	.num			= ARRAY_SIZE(dnet_cfg_entries_blobsystem),
	.size			= sizeof(struct eblob_backend_config),
	.init			= dnet_blob_config_init,
	.cleanup		= dnet_blob_config_cleanup,
};

int dnet_eblob_backend_init(void)
{
	return dnet_backend_register(&dnet_eblob_backend);
}

void dnet_eblob_backend_exit(void)
{
	/* cleanup routing will be called explicitly through backend->cleanup() callback */
}
