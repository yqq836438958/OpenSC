/*
 * card.c: General SmartCard functions
 *
 * Copyright (C) 2001  Juha Yrj�l� <juha.yrjola@iki.fi>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sc-internal.h"
#include "sc-log.h"
#include "sc-asn1.h"
#include <assert.h>
#include <stdlib.h>

int sc_check_sw(struct sc_card *card, int sw1, int sw2)
{
	assert(card->ops->check_sw != NULL);
	return card->ops->check_sw(card, sw1, sw2);
}

static int sc_check_apdu(struct sc_context *ctx, const struct sc_apdu *apdu)
{
	if (apdu->le > 256) {
		error(ctx, "Value of Le too big (maximum 256 bytes)\n");
		SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
	}
	if (apdu->lc > 256) {
		error(ctx, "Value of Lc too big (maximum 256 bytes)\n");
		SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
	}
	switch (apdu->cse) {
	case SC_APDU_CASE_1:
		if (apdu->datalen > 0) {
			error(ctx, "Case 1 APDU with data supplied\n");
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		}
		break;
	case SC_APDU_CASE_2_SHORT:
		if (apdu->datalen > 0) {
			error(ctx, "Case 2 APDU with data supplied\n");
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		}
		if (apdu->le == 0) {
			error(ctx, "Case 2 APDU with no response expected\n");
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		}
		if (apdu->resplen < apdu->le) {
			error(ctx, "Response buffer size < Le\n");
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		}
		break;
	case SC_APDU_CASE_3_SHORT:
		if (apdu->datalen == 0 || apdu->data == NULL) {
			error(ctx, "Case 3 APDU with no data supplied\n");
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		}
		break;
	case SC_APDU_CASE_4_SHORT:
		if (apdu->datalen == 0 || apdu->data == NULL) {
			error(ctx, "Case 3 APDU with no data supplied\n");
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		}
		if (apdu->le == 0) {
			error(ctx, "Case 4 APDU with no response expected\n");
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		}
		if (apdu->resplen < apdu->le) {
			error(ctx, "Le > response buffer size\n");
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		}
		break;
	case SC_APDU_CASE_2_EXT:
	case SC_APDU_CASE_3_EXT:
	case SC_APDU_CASE_4_EXT:
		SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
	}
	return 0;
}

static int sc_transceive_t0(struct sc_card *card, struct sc_apdu *apdu)
{
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	size_t sendsize, recvsize;
	u8 *data = sbuf;
	size_t data_bytes = apdu->lc;
	int r;

	if (card->reader->ops->transmit == NULL)
		return SC_ERROR_NOT_SUPPORTED;
	assert(card->reader->ops->transmit != NULL);
	if (data_bytes == 0)
		data_bytes = 256;
	*data++ = apdu->cla;
	*data++ = apdu->ins;
	*data++ = apdu->p1;
	*data++ = apdu->p2;
	switch (apdu->cse) {
	case SC_APDU_CASE_1:
		break;
	case SC_APDU_CASE_2_SHORT:
		*data++ = (u8) apdu->le;
		break;
	case SC_APDU_CASE_2_EXT:
		*data++ = (u8) 0;
		*data++ = (u8) (apdu->le >> 8);
		*data++ = (u8) (apdu->le & 0xFF);
		break;
	case SC_APDU_CASE_3_SHORT:
		*data++ = (u8) apdu->lc;
		if (apdu->datalen != data_bytes)
			return SC_ERROR_INVALID_ARGUMENTS;
		memcpy(data, apdu->data, data_bytes);
		data += data_bytes;
		break;
	case SC_APDU_CASE_4_SHORT:
		*data++ = (u8) apdu->lc;
		if (apdu->datalen != data_bytes)
			return SC_ERROR_INVALID_ARGUMENTS;
		memcpy(data, apdu->data, data_bytes);
		data += data_bytes;
		if (apdu->le == 256)
			*data++ = 0x00;
		else
			*data++ = (u8) apdu->le;
		break;
	}
	sendsize = data - sbuf;
	recvsize = apdu->resplen + 2;	/* space for the SW's */
	if (card->ctx->debug >= 5) {
		char buf[2048];
		
		sc_hex_dump(card->ctx, sbuf, sendsize, buf, sizeof(buf));
		debug(card->ctx, "Sending %d bytes (resp. %d bytes):\n%s",
			sendsize, recvsize, buf);
	}
	r = card->reader->ops->transmit(card->reader, card->slot, sbuf,
					sendsize, rbuf, &recvsize);
	memset(sbuf, 0, sendsize);
	SC_TEST_RET(card->ctx, r, "Unable to transmit");

	assert(recvsize >= 2);
	apdu->sw1 = (unsigned int) rbuf[recvsize-2];
	apdu->sw2 = (unsigned int) rbuf[recvsize-1];
	recvsize -= 2;
	if (recvsize > apdu->resplen)
		recvsize = apdu->resplen;
	else
		apdu->resplen = recvsize;
	if (recvsize > 0)
		memcpy(apdu->resp, rbuf, recvsize);

	return 0;
}

int sc_transmit_apdu(struct sc_card *card, struct sc_apdu *apdu)
{
	int r;
	size_t orig_resplen;
	
	assert(card != NULL && apdu != NULL);
	SC_FUNC_CALLED(card->ctx, 4);
	orig_resplen = apdu->resplen;
	r = sc_check_apdu(card->ctx, apdu);
	SC_TEST_RET(card->ctx, r, "APDU sanity check failed");
	r = sc_lock(card);
	SC_TEST_RET(card->ctx, r, "sc_lock() failed");
	r = sc_transceive_t0(card, apdu);
	if (r != 0) {
		sc_unlock(card);
		SC_TEST_RET(card->ctx, r, "transceive_t0() failed");
	}
	if (card->ctx->debug >= 5) {
		char buf[2048];

		buf[0] = '\0';
		if (apdu->resplen > 0) {
			sc_hex_dump(card->ctx, apdu->resp, apdu->resplen,
				    buf, sizeof(buf));
		}
		debug(card->ctx, "Received %d bytes (SW1=%02X SW2=%02X)\n%s",
		      apdu->resplen, apdu->sw1, apdu->sw2, buf);
	}
	if (apdu->sw1 == 0x6C && apdu->resplen == 0) {
		apdu->resplen = orig_resplen;
		apdu->le = apdu->sw2;
		r = sc_transceive_t0(card, apdu);
		if (r != 0) {
			sc_unlock(card);
			SC_TEST_RET(card->ctx, r, "transceive_t0() failed");
		}
	}
	if (apdu->sw1 == 0x61 && apdu->resplen == 0) {
		struct sc_apdu rspapdu;
		u8 rsp[SC_MAX_APDU_BUFFER_SIZE];

		if (orig_resplen == 0) {
			apdu->sw1 = 0x90;	/* FIXME: should we do this? */
			apdu->sw2 = 0;
			sc_unlock(card);
			return 0;
		}
		
		sc_format_apdu(card, &rspapdu, SC_APDU_CASE_2_SHORT,
			       0xC0, 0, 0);
		rspapdu.le = (size_t) apdu->sw2;
		rspapdu.resp = rsp;
		rspapdu.resplen = (size_t) apdu->sw2;
		r = sc_transceive_t0(card, &rspapdu);
		if (r != 0) {
			error(card->ctx, "error while getting response: %s\n",
			      sc_strerror(r));
			sc_unlock(card);
			return r;
		}
		if (card->ctx->debug >= 5) {
			char buf[2048];
			buf[0] = 0;
			if (rspapdu.resplen) {
				sc_hex_dump(card->ctx, rspapdu.resp,
					    rspapdu.resplen,
					    buf, sizeof(buf));
			}
			debug(card->ctx, "Response %d bytes (SW1=%02X SW2=%02X)\n%s",
			      rspapdu.resplen, rspapdu.sw1, rspapdu.sw2, buf);
		}
		if (rspapdu.resplen) {
			size_t c = rspapdu.resplen;
			
			if (c > orig_resplen)
				c = orig_resplen;
			memcpy(apdu->resp, rspapdu.resp, c);
			apdu->resplen = c;
		}
		apdu->sw1 = rspapdu.sw1;
		apdu->sw2 = rspapdu.sw2;
	}
	sc_unlock(card);
	return 0;
}

void sc_format_apdu(struct sc_card *card, struct sc_apdu *apdu,
		    int cse, int ins, int p1, int p2)
{
	assert(card != NULL && apdu != NULL);
	memset(apdu, 0, sizeof(*apdu));
	apdu->cla = (u8) card->cla;
	apdu->cse = cse;
	apdu->ins = (u8) ins;
	apdu->p1 = (u8) p1;
	apdu->p2 = (u8) p2;

	return;
}

static struct sc_card * sc_card_new()
{
	struct sc_card *card;
	
	card = malloc(sizeof(struct sc_card));
	if (card == NULL)
		return NULL;
	memset(card, 0, sizeof(struct sc_card));
	card->ops = malloc(sizeof(struct sc_card_operations));
	if (card->ops == NULL) {
		free(card);
		return NULL;
	}
	card->app_count = -1;
	card->magic = SC_CARD_MAGIC;
	pthread_mutex_init(&card->mutex, NULL);

	return card;
}

static void sc_card_free(struct sc_card *card)
{
	int i;
	
	assert(sc_card_valid(card));
	for (i = 0; i < card->app_count; i++) {
		if (card->app[i]->label)
			free(card->app[i]->label);
		if (card->app[i]->ddo)
			free(card->app[i]->ddo);
		free(card->app[i]);
	}
	free(card->ops);
	if (card->algorithms != NULL)
		free(card->algorithms);
	pthread_mutex_destroy(&card->mutex);
	card->magic = 0;
	free(card);
}

int sc_connect_card(struct sc_reader *reader, int slot_id,
		    struct sc_card **card_out)
{
	struct sc_card *card;
	struct sc_context *ctx = reader->ctx;
	struct sc_slot_info *slot = _sc_get_slot_info(reader, slot_id);
	int i, r = 0, connected = 0;

	assert(card_out != NULL);
	SC_FUNC_CALLED(ctx, 1);
	if (reader->ops->connect == NULL)
		SC_FUNC_RETURN(ctx, 0, SC_ERROR_NOT_SUPPORTED);
	if (slot == NULL)
		SC_FUNC_RETURN(ctx, 0, SC_ERROR_SLOT_NOT_FOUND);

	card = sc_card_new();
	if (card == NULL)
		SC_FUNC_RETURN(ctx, 1, SC_ERROR_OUT_OF_MEMORY);
	r = reader->ops->connect(reader, slot);
	if (r)
		goto err;
	connected = 1;

	card->reader = reader;
	card->slot = slot;
	card->ctx = ctx;

	memcpy(card->atr, slot->atr, slot->atr_len);
	card->atr_len = slot->atr_len;

	if (ctx->forced_driver != NULL) {
		card->driver = ctx->forced_driver;
		memcpy(card->ops, card->driver->ops, sizeof(struct sc_card_operations));
		if (card->ops->init != NULL) {
			r = card->ops->init(card);
			if (r) {
				error(ctx, "driver '%s' init() failed: %s\n", card->driver->name,
				      sc_strerror(r));
				goto err;
			}
		}
	} else for (i = 0; ctx->card_drivers[i] != NULL; i++) {
		const struct sc_card_driver *drv = ctx->card_drivers[i];
		const struct sc_card_operations *ops = drv->ops;
		int r;
		
		if (ctx->debug >= 3)
			debug(ctx, "trying driver: %s\n", drv->name);
		if (ops == NULL || ops->match_card == NULL)
			continue;
		if (ops->match_card(card) != 1)
			continue;
		if (ctx->debug >= 3)
			debug(ctx, "matched: %s\n", drv->name);
		memcpy(card->ops, ops, sizeof(struct sc_card_operations));
		card->driver = drv;
		r = ops->init(card);
		if (r) {
			error(ctx, "driver '%s' init() failed: %s\n", drv->name,
			      sc_strerror(r));
			if (r == SC_ERROR_INVALID_CARD) {
				card->driver = NULL;
				continue;
			}
			goto err;
		}
		break;
	}
	if (card->driver == NULL) {
		error(ctx, "unable to find driver for inserted card\n");
		r = SC_ERROR_INVALID_CARD;
		goto err;
	}
	*card_out = card;

	SC_FUNC_RETURN(ctx, 1, 0);
err:
	if (card != NULL)
		sc_card_free(card);
	SC_FUNC_RETURN(ctx, 1, r);
}

int sc_disconnect_card(struct sc_card *card, int action)
{
	struct sc_context *ctx;
	assert(sc_card_valid(card));
	ctx = card->ctx;
	SC_FUNC_CALLED(ctx, 1);
	assert(card->lock_count == 0);
	if (card->ops->finish) {
		int r = card->ops->finish(card);
		if (r)
			error(card->ctx, "card driver finish() failed: %s\n",
			      sc_strerror(r)); 
	}
	if (card->reader->ops->disconnect) {
		int r = card->reader->ops->disconnect(card->reader, card->slot, action);
		if (r)
			error(card->ctx, "disconnect() failed: %s\n",
			      sc_strerror(r));
	}
	sc_card_free(card);
	SC_FUNC_RETURN(ctx, 1, 0);
}

int sc_lock(struct sc_card *card)
{
	int r = 0;
	
	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
	pthread_mutex_lock(&card->mutex);
	if (card->lock_count == 0) {
		if (card->reader->ops->lock != NULL)
			r = card->reader->ops->lock(card->reader, card->slot);
		if (r == 0)
			card->cache_valid = 1;
	}
	if (r == 0)
		card->lock_count++;
	pthread_mutex_unlock(&card->mutex);
	SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_unlock(struct sc_card *card)
{
	int r = 0;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
	pthread_mutex_lock(&card->mutex);
	card->lock_count--;
	assert(card->lock_count >= 0);
	if (card->lock_count == 0) {
		if (card->reader->ops->unlock != NULL)
			r = card->reader->ops->unlock(card->reader, card->slot);
		card->cache_valid = 0;
		memset(&card->cache, 0, sizeof(card->cache));
	}
	pthread_mutex_unlock(&card->mutex);
	SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_list_files(struct sc_card *card, u8 *buf, size_t buflen)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 1);
        if (card->ops->list_files == NULL)
		SC_FUNC_RETURN(card->ctx, 1, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->list_files(card, buf, buflen);
        SC_FUNC_RETURN(card->ctx, 1, r);
}

int sc_create_file(struct sc_card *card, struct sc_file *file)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 1);
        if (card->ops->create_file == NULL)
		SC_FUNC_RETURN(card->ctx, 1, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->create_file(card, file);
        SC_FUNC_RETURN(card->ctx, 1, r);
}

int sc_delete_file(struct sc_card *card, const struct sc_path *path)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 1);
        if (card->ops->delete_file == NULL)
		SC_FUNC_RETURN(card->ctx, 1, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->delete_file(card, path);
        SC_FUNC_RETURN(card->ctx, 1, r);
}

int sc_read_binary(struct sc_card *card, unsigned int idx,
		   unsigned char *buf, size_t count, unsigned long flags)
{
	int r;

	assert(card != NULL && card->ops != NULL && buf != NULL);
	if (card->ctx->debug >= 2)
		debug(card->ctx, "sc_read_binary: %d bytes at index %d\n", count, idx);
	if (card->ops->read_binary == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	if (count > SC_APDU_CHOP_SIZE && !(card->caps & SC_CARD_CAP_APDU_EXT)) {
		int bytes_read = 0;
		unsigned char *p = buf;

		r = sc_lock(card);
		SC_TEST_RET(card->ctx, r, "sc_lock() failed");
		while (count > 0) {
			int n = count > SC_APDU_CHOP_SIZE ? SC_APDU_CHOP_SIZE : count;
			r = sc_read_binary(card, idx, p, n, flags);
			if (r < 0) {
				sc_unlock(card);
				SC_TEST_RET(card->ctx, r, "sc_read_binary() failed");
			}
			p += r;
			idx += r;
			bytes_read += r;
			count -= r;
			if (r == 0) {
				sc_unlock(card);
				SC_FUNC_RETURN(card->ctx, 2, bytes_read);
			}
		}
		sc_unlock(card);
		SC_FUNC_RETURN(card->ctx, 2, bytes_read);
	}
	r = card->ops->read_binary(card, idx, buf, count, flags);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_write_binary(struct sc_card *card, unsigned int idx,
		    const u8 *buf, size_t count, unsigned long flags)
{
	int r;

	assert(card != NULL && card->ops != NULL && buf != NULL);
	if (card->ctx->debug >= 2)
		debug(card->ctx, "sc_write_binary: %d bytes at index %d\n", count, idx);
	if (card->ops->write_binary == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	if (count > SC_APDU_CHOP_SIZE && !(card->caps & SC_CARD_CAP_APDU_EXT)) {
		int bytes_written = 0;
		const u8 *p = buf;

		r = sc_lock(card);
		SC_TEST_RET(card->ctx, r, "sc_lock() failed");
		while (count > 0) {
			int n = count > SC_APDU_CHOP_SIZE ? SC_APDU_CHOP_SIZE : count;
			r = sc_write_binary(card, idx, p, n, flags);
			if (r < 0) {
				sc_unlock(card);
				SC_TEST_RET(card->ctx, r, "sc_read_binary() failed");
			}
			p += r;
			idx += r;
			bytes_written += r;
			count -= r;
			if (r == 0) {
				sc_unlock(card);
				SC_FUNC_RETURN(card->ctx, 2, bytes_written);
			}
		}
		sc_unlock(card);
		SC_FUNC_RETURN(card->ctx, 2, bytes_written);
	}
	r = card->ops->write_binary(card, idx, buf, count, flags);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_update_binary(struct sc_card *card, unsigned int idx,
		     const u8 *buf, size_t count, unsigned long flags)
{
	int r;

	assert(card != NULL && card->ops != NULL && buf != NULL);
	if (card->ctx->debug >= 2)
		debug(card->ctx, "sc_update_binary: %d bytes at index %d\n", count, idx);
	if (card->ops->update_binary == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	if (count > SC_APDU_CHOP_SIZE && !(card->caps & SC_CARD_CAP_APDU_EXT)) {
		int bytes_written = 0;
		const u8 *p = buf;

		r = sc_lock(card);
		SC_TEST_RET(card->ctx, r, "sc_lock() failed");
		while (count > 0) {
			int n = count > SC_APDU_CHOP_SIZE ? SC_APDU_CHOP_SIZE : count;
			r = sc_update_binary(card, idx, p, n, flags);
			if (r < 0) {
				sc_unlock(card);
				SC_TEST_RET(card->ctx, r, "sc_read_binary() failed");
			}
			p += r;
			idx += r;
			bytes_written += r;
			count -= r;
			if (r == 0) {
				sc_unlock(card);
				SC_FUNC_RETURN(card->ctx, 2, bytes_written);
			}
		}
		sc_unlock(card);
		SC_FUNC_RETURN(card->ctx, 2, bytes_written);
	}
	r = card->ops->update_binary(card, idx, buf, count, flags);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_select_file(struct sc_card *card,
		   const struct sc_path *in_path,
		   struct sc_file **file)
{
	int r;

	assert(card != NULL && in_path != NULL);
	if (in_path->len > SC_MAX_PATH_SIZE)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
	if (in_path->type == SC_PATH_TYPE_PATH) {
		/* Perform a sanity check */
		int i;
		if ((in_path->len & 1) != 0)
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
		for (i = 0; i < in_path->len/2; i++) {
			u8 p1 = in_path->value[2*i],
			   p2 = in_path->value[2*i+1];
			if ((p1 == 0x3F && p2 == 0x00) && i > 0)
				SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
		}
	}
	if (card->ctx->debug >= 2) {
		char line[128], *linep = line;

		linep += sprintf(linep, "called with type %d, path ", in_path->type);
		for (r = 0; r < in_path->len; r++) {
			sprintf(linep, "%02X", in_path->value[r]);
			linep += 2;
		}
		strcpy(linep, "\n");
		debug(card->ctx, line);
	}
        if (card->ops->select_file == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->select_file(card, in_path, file);
	/* Remember file path */
	if (r == 0 && file && *file)
		(*file)->path = *in_path;
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_get_challenge(struct sc_card *card, u8 *rnd, size_t len)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
        if (card->ops->get_challenge == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->get_challenge(card, rnd, len);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_read_record(struct sc_card *card, unsigned int rec_nr, u8 *buf,
		   size_t count, unsigned long flags)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
        if (card->ops->read_record == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->read_record(card, rec_nr, buf, count, flags);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_write_record(struct sc_card *card, unsigned int rec_nr, const u8 * buf,
		    size_t count, unsigned long flags)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
        if (card->ops->write_record == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->write_record(card, rec_nr, buf, count, flags);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_append_record(struct sc_card *card, const u8 * buf, size_t count,
		     unsigned long flags)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
        if (card->ops->append_record == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->append_record(card, buf, count, flags);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_update_record(struct sc_card *card, unsigned int rec_nr, const u8 * buf,
		     size_t count, unsigned long flags)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
        if (card->ops->update_record == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->update_record(card, rec_nr, buf, count, flags);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

inline int sc_card_valid(const struct sc_card *card) {
#ifndef NDEBUG
	assert(card != NULL);
#endif
	return card->magic == SC_CARD_MAGIC;
}

int
sc_card_ctl(struct sc_card *card, unsigned long cmd, void *args)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
        if (card->ops->card_ctl == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->card_ctl(card, cmd, args);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int _sc_card_add_algorithm(struct sc_card *card, const struct sc_algorithm_info *info)
{
	struct sc_algorithm_info *p;
	
	assert(sc_card_valid(card) && info != NULL);
	card->algorithms = realloc(card->algorithms, (card->algorithm_count + 1) * sizeof(*info));
	if (card->algorithms == NULL) {
		card->algorithm_count = 0;
		return SC_ERROR_OUT_OF_MEMORY;
	}
	p = card->algorithms + card->algorithm_count;
	card->algorithm_count++;
	*p = *info;
	return 0;
}

int _sc_card_add_rsa_alg(struct sc_card *card, unsigned int key_length,
			 unsigned long flags, unsigned long exponent)
{
	struct sc_algorithm_info info;

	memset(&info, 0, sizeof(info));
	info.algorithm = SC_ALGORITHM_RSA;
	info.key_length = key_length;
	info.flags = flags;
	info.u._rsa.exponent = exponent;
	
	return _sc_card_add_algorithm(card, &info);
}

struct sc_algorithm_info * _sc_card_find_rsa_alg(struct sc_card *card,
						 unsigned int key_length)
{
	int i;

	for (i = 0; i < card->algorithm_count; i++) {
		struct sc_algorithm_info *info = &card->algorithms[i];
		
		if (info->algorithm != SC_ALGORITHM_RSA)
			continue;
		if (info->key_length != key_length)
			continue;
		return info;
	}
	return NULL;
}

int _sc_match_atr(struct sc_card *card, struct sc_atr_table *table, int *id_out)
{
	const u8 *atr = card->atr;
	size_t atr_len = card->atr_len;
	int i = 0;
	
	for (i = 0; table[i].atr != NULL; i++) {
		if (table[i].atr_len != atr_len)
			continue;
		if (memcmp(table[i].atr, atr, atr_len) != 0)
			continue;
		if (id_out != NULL)
			*id_out = table[i].id;
		return i;
	}
	return -1;
}
