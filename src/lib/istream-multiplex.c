/* Copyright (c) 2017-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "istream-private.h"
#include "istream-multiplex.h"

/* all multiplex packets are [1 byte cid][4 byte length][data] */

struct multiplex_istream;

struct multiplex_ichannel {
	struct istream_private istream;
	struct multiplex_istream *mstream;
	uint8_t cid;
	/* Number of bytes already in the channel stream waiting to be read.
	   The bytes are located after the current stream->pos. */
	size_t pending_count;
	bool closed:1;
};

struct multiplex_istream {
	struct istream *parent;

	/* channel 0 is main channel */
	uint8_t cur_channel;
	/* Number of bytes still unread in this packet. */
	unsigned int packet_bytes_left;
	size_t bufsize;
	ARRAY(struct multiplex_ichannel *) channels;

	bool blocking:1;
};

static ssize_t i_stream_multiplex_ichannel_read(struct istream_private *stream);

static struct multiplex_ichannel *
get_channel(struct multiplex_istream *mstream, uint8_t cid)
{
	struct multiplex_ichannel *channel;
	i_assert(mstream != NULL);
	array_foreach_elem(&mstream->channels, channel) {
		if (channel != NULL && channel->cid == cid)
			return channel;
	}
	return NULL;
}

static void propagate_eof(struct multiplex_istream *mstream)
{
	struct multiplex_ichannel *channel;
	array_foreach_elem(&mstream->channels, channel) {
		if (channel == NULL)
			continue;

		channel->istream.istream.eof = TRUE;
		if (mstream->packet_bytes_left > 0 &&
		    channel->istream.istream.stream_errno == 0) {
			channel->istream.istream.stream_errno = EPIPE;
			io_stream_set_error(&channel->istream.iostream,
				"Unexpected EOF - %u bytes remaining in packet",
				mstream->packet_bytes_left);
		}
	}
}

static void propagate_error(struct multiplex_istream *mstream)
{
	struct multiplex_ichannel *channel;
	int stream_errno = mstream->parent->stream_errno;

	if (stream_errno != 0) {
		const char *error = i_stream_get_error(mstream->parent);
		array_foreach_elem(&mstream->channels, channel) {
			if (channel == NULL)
				continue;

			channel->istream.istream.eof = TRUE;
			channel->istream.istream.stream_errno = stream_errno;
			io_stream_set_error(&channel->istream.iostream,
					    "%s", error);
		}
	} else if (mstream->parent->eof)
		propagate_eof(mstream);
}

static ssize_t
i_stream_multiplex_add(struct multiplex_ichannel *req_channel,
		       const unsigned char *data, size_t wanted,
		       size_t *got)
{
	struct multiplex_ichannel *channel =
		get_channel(req_channel->mstream,
			    req_channel->mstream->cur_channel);
	size_t used, avail;

	/* is it open? */
	if (channel == NULL || channel->closed)
		return wanted;

	struct istream_private *stream = &channel->istream;
	stream->pos += channel->pending_count;
	bool alloc_ret = i_stream_try_alloc(stream, wanted, &avail);
	stream->pos -= channel->pending_count;
	if (!alloc_ret) {
		i_stream_set_input_pending(&stream->istream, TRUE);
		if (channel->cid != req_channel->cid)
			return 0;
		return -2;
	}

	used = I_MIN(wanted, avail);

	/* dump into buffer */
	if (channel->cid != req_channel->cid) {
		i_assert(stream->pos + channel->pending_count + used <= stream->buffer_size);
		memcpy(stream->w_buffer + stream->pos + channel->pending_count,
		       data, used);
		channel->pending_count += used;
		i_stream_set_input_pending(&stream->istream, TRUE);
	} else {
		i_assert(stream->pos + used <= stream->buffer_size);
		memcpy(stream->w_buffer + stream->pos, data, used);
		stream->pos += used;
		*got += used;
	}
	return used;
}

static ssize_t
i_stream_multiplex_read(struct multiplex_istream *mstream,
			struct multiplex_ichannel *req_channel)
{
	const unsigned char *data;
	size_t len = 0, wanted, got = 0;
	ssize_t ret;

	if (mstream->parent == NULL) {
		req_channel->istream.istream.eof = TRUE;
		return -1;
	}

	(void)i_stream_get_data(mstream->parent, &len);

	if (len == 0 && mstream->parent->closed) {
		req_channel->istream.istream.eof = TRUE;
		return -1;
	}

	if (((mstream->packet_bytes_left > 0 && len == 0) ||
	     (mstream->packet_bytes_left == 0 && len < 5)) &&
	    (ret = i_stream_read_memarea(mstream->parent)) <= 0) {
		propagate_error(mstream);
		return ret;
	}

	for(;;) {
		data = i_stream_get_data(mstream->parent, &len);
		if (len == 0) {
			if (got == 0 && mstream->blocking) {
				/* can't return 0 with blocking istreams,
				   so try again from the beginning. */
				return i_stream_multiplex_read(mstream, req_channel);
			}
			break;
		}

		if (mstream->packet_bytes_left > 0) {
			wanted = I_MIN(len, mstream->packet_bytes_left);
			ret = i_stream_multiplex_add(req_channel, data, wanted, &got);
			if (ret <= 0) {
				if (got > 0)
					break;
				return ret;
			}
			i_assert(ret > 0);
			mstream->packet_bytes_left -= ret;
			i_stream_skip(mstream->parent, ret);
			/* see if there is more to read */
			continue;
		}
		if (mstream->packet_bytes_left == 0) {
			/* need more data */
			if (len < 5) {
				ret = i_stream_multiplex_ichannel_read(&req_channel->istream);
				if (ret > 0)
					got += ret;
				break;
			}
			/* channel ID */
			mstream->cur_channel = data[0];
			/* data length */
			mstream->packet_bytes_left =
				be32_to_cpu_unaligned(data+1);
			i_stream_skip(mstream->parent, 5);
		}
	}

	propagate_error(mstream);

	return got;
}

static ssize_t i_stream_multiplex_ichannel_read(struct istream_private *stream)
{
	struct multiplex_ichannel *channel =
		container_of(stream, struct multiplex_ichannel, istream);
	/* if previous multiplex read dumped data for us
	   actually serve it here. */
	if (channel->pending_count > 0) {
		ssize_t ret = channel->pending_count;
		stream->pos += channel->pending_count;
		channel->pending_count = 0;
		return ret;
	}
	return i_stream_multiplex_read(channel->mstream, channel);
}

static void
i_stream_multiplex_ichannel_switch_ioloop_to(struct istream_private *stream,
					     struct ioloop *ioloop)
{
	struct multiplex_ichannel *channel =
		container_of(stream, struct multiplex_ichannel, istream);

	i_stream_switch_ioloop_to(channel->mstream->parent, ioloop);
}

static void
i_stream_multiplex_ichannel_close(struct iostream_private *stream, bool close_parent)
{
	struct multiplex_ichannel *arr_channel;
	struct multiplex_ichannel *channel =
		container_of(stream, struct multiplex_ichannel,
			     istream.iostream);
	channel->closed = TRUE;
	if (close_parent) {
		array_foreach_elem(&channel->mstream->channels, arr_channel)
			if (arr_channel != NULL && !arr_channel->closed)
				return;
		i_stream_close(channel->mstream->parent);
	}
}

static void i_stream_multiplex_try_destroy(struct multiplex_istream *mstream)
{
	struct multiplex_ichannel *channel;
	/* can't do anything until all channels are destroyed */
	array_foreach_elem(&mstream->channels, channel)
		if (channel != NULL)
			return;
	i_stream_unref(&mstream->parent);
	array_free(&mstream->channels);
	i_free(mstream);
}

static void i_stream_multiplex_ichannel_destroy(struct iostream_private *stream)
{
	struct multiplex_ichannel **channelp;
	struct multiplex_ichannel *channel =
		container_of(stream, struct multiplex_ichannel,
			     istream.iostream);
	i_stream_free_buffer(&channel->istream);
	array_foreach_modifiable(&channel->mstream->channels, channelp) {
		if (*channelp == channel) {
			*channelp = NULL;
			break;
		}
	}
	i_stream_multiplex_try_destroy(channel->mstream);
}

static struct istream *
i_stream_add_channel_real(struct multiplex_istream *mstream, uint8_t cid)
{
	struct multiplex_ichannel *channel = i_new(struct multiplex_ichannel, 1);
	channel->cid = cid;
	channel->mstream = mstream;
	channel->istream.read = i_stream_multiplex_ichannel_read;
	channel->istream.switch_ioloop_to = i_stream_multiplex_ichannel_switch_ioloop_to;
	channel->istream.iostream.close = i_stream_multiplex_ichannel_close;
	channel->istream.iostream.destroy = i_stream_multiplex_ichannel_destroy;
	channel->istream.max_buffer_size = mstream->bufsize;
	channel->istream.istream.blocking = mstream->blocking;
	if (cid == 0)
		channel->istream.fd = i_stream_get_fd(mstream->parent);
	else
		channel->istream.fd = -1;
	array_push_back(&channel->mstream->channels, &channel);

	return i_stream_create(&channel->istream, NULL, channel->istream.fd, 0);
}

struct istream *i_stream_multiplex_add_channel(struct istream *stream, uint8_t cid)
{
	struct multiplex_ichannel *chan =
		container_of(stream->real_stream,
			     struct multiplex_ichannel, istream);
	i_assert(get_channel(chan->mstream, cid) == NULL);

	return i_stream_add_channel_real(chan->mstream, cid);
}

struct istream *i_stream_create_multiplex(struct istream *parent, size_t bufsize)
{
	struct multiplex_istream *mstream;

	mstream = i_new(struct multiplex_istream, 1);
	mstream->parent = parent;
	mstream->bufsize = bufsize;
	mstream->blocking = parent->blocking;
	i_array_init(&mstream->channels, 8);
	i_stream_ref(parent);

	return i_stream_add_channel_real(mstream, 0);
}

uint8_t i_stream_multiplex_get_channel_id(struct istream *stream)
{
	struct multiplex_ichannel *channel =
		container_of(stream->real_stream,
			     struct multiplex_ichannel, istream);
	return channel->cid;
}
