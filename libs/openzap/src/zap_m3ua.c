/*
 *  zap_m3ua.c
 *  openzap
 *
 *  Created by Shane Burrell on 4/3/08.
 *  Copyright 2008 Shane Burrell. All rights reserved.
 *
 * 
 * Copyright (c) 2007-2014, Anthony Minessale II, Nenad Corbic * 
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * * Neither the name of the original author; nor the names of any contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 * 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "openzap.h"
#include "m3ua_client.h"
#include "zap_m3ua.h"

#define MAX_REQ_ID MAX_PENDING_CALLS
typedef uint16_t m3ua_request_id_t;

typedef enum {
	BST_FREE,
	BST_WAITING,
	BST_READY,
	BST_FAIL
} m3ua_request_status_t;

typedef struct {
	m3ua_request_status_t status;
	m3uac_event_t event;
	zap_span_t *span;
	zap_channel_t *zchan;
} m3ua_request_t;


struct general_config {
	uint32_t region;
};
typedef struct general_config general_config_t;


struct m3ua_channel_profile {
	char name[80];
	int cust_span;
	unsigned char opc[3];
	unsigned char dpc[3];
	int local_ip[4];
	int local_port;
	int remote_ip[4];
	int remote_port;
	int m3ua_mode;
};
typedef struct m3ua_channel_profile m3ua_channel_profile_t;

static struct {
	zap_hash_t *profile_hash;
	general_config_t general_config;
} globals;

struct m3ua_span_data {
	uint32_t boardno;
	uint32_t flags;
};
typedef struct m3ua_span_data m3ua_span_data_t;

struct m3ua_chan_data {
	zap_buffer_t *digit_buffer;
	zap_mutex_t *digit_mutex;
	zap_size_t dtmf_len;
	uint32_t flags;
	uint32_t hdlc_bytes;
};
typedef struct m3ua_chan_data m3ua_chan_data_t;

static zap_mutex_t *request_mutex = NULL;
static zap_mutex_t *signal_mutex = NULL;

static uint8_t req_map[MAX_REQ_ID+1] = { 0 };

static void release_request_id(m3ua_request_id_t r)
{
	zap_mutex_lock(request_mutex);
	req_map[r] = 0;
	zap_mutex_unlock(request_mutex);
}

/*static m3ua_request_id_t next_request_id(void)
{
	m3ua_request_id_t r = 0;
	int ok = 0;
	
	while(!ok) {
		zap_mutex_lock(request_mutex);
		for (r = 1; r <= MAX_REQ_ID; r++) {
			if (!req_map[r]) {
				ok = 1;
				req_map[r] = 1;
				break;
			}
		}
		zap_mutex_unlock(request_mutex);
		if (!ok) {
			zap_sleep(5);
		}
	}
	return r;
}
*/

static __inline__ void state_advance(zap_channel_t *zchan)
{

	m3ua_data_t *m3ua_data = zchan->span->signal_data;
	m3uac_connection_t *mcon = &m3ua_data->mcon;
	zap_sigmsg_t sig;
	zap_status_t status;

	zap_log(ZAP_LOG_DEBUG, "%d:%d STATE [%s]\n", zchan->span_id, zchan->chan_id, zap_channel_state2str(zchan->state));
	
	memset(&sig, 0, sizeof(sig));
	sig.chan_id = zchan->chan_id;
	sig.span_id = zchan->span_id;
	sig.channel = zchan;

	switch (zchan->state) {
	case ZAP_CHANNEL_STATE_DOWN:
		{
			if (zchan->extra_id) {
				release_request_id((m3ua_request_id_t)zchan->extra_id);
				zchan->extra_id = 0;
			}
			zap_channel_done(zchan);			
		}
		break;
	case ZAP_CHANNEL_STATE_PROGRESS_MEDIA:
	case ZAP_CHANNEL_STATE_PROGRESS:
		{
			if (zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_PROGRESS_MEDIA;
				if ((status = m3ua_data->signal_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			} else {
				m3uac_exec_command(mcon,
								   zchan->physical_span_id-1,
								   zchan->physical_chan_id-1,								   
								   0,
								   SIGBOOST_EVENT_CALL_START_ACK,
								   0);
			}
		}
		break;
	case ZAP_CHANNEL_STATE_RING:
		{
			if (!zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_START;
				if ((status = m3ua_data->signal_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			}

		}
		break;
	case ZAP_CHANNEL_STATE_RESTART:
		{
			if (zchan->last_state != ZAP_CHANNEL_STATE_HANGUP && zchan->last_state != ZAP_CHANNEL_STATE_DOWN) {
				zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
			} else {
				zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DOWN);
			}
		}
		break;
	case ZAP_CHANNEL_STATE_UP:
		{
			if (zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_UP;
				if ((status = m3ua_data->signal_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			} else {
				if (!(zap_test_flag(zchan, ZAP_CHANNEL_PROGRESS) || zap_test_flag(zchan, ZAP_CHANNEL_MEDIA))) {
					m3uac_exec_command(mcon,
									   zchan->physical_span_id-1,
									   zchan->physical_chan_id-1,								   
									   0,
									   SIGBOOST_EVENT_CALL_START_ACK,
									   0);
				}
				
				m3uac_exec_command(mcon,
								   zchan->physical_span_id-1,
								   zchan->physical_chan_id-1,								   
								   0,
								   SIGBOOST_EVENT_CALL_ANSWERED,
								   0);
			}
		}
		break;
	case ZAP_CHANNEL_STATE_DIALING:
		{
		}
		break;
	case ZAP_CHANNEL_STATE_HANGUP_COMPLETE:
		{
			zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DOWN);
		}
		break;
	case ZAP_CHANNEL_STATE_HANGUP:
		{
			if (zap_test_flag(zchan, ZAP_CHANNEL_ANSWERED) || zap_test_flag(zchan, ZAP_CHANNEL_PROGRESS) || zap_test_flag(zchan, ZAP_CHANNEL_MEDIA)) {
				m3uac_exec_command(mcon,
								   zchan->physical_span_id-1,
								   zchan->physical_chan_id-1,
								   0,
								   SIGBOOST_EVENT_CALL_STOPPED,
								   zchan->caller_data.hangup_cause);
			} else {
				m3uac_exec_command(mcon,
								   zchan->physical_span_id-1,
								   zchan->physical_chan_id-1,								   
								   0,
								   SIGBOOST_EVENT_CALL_START_NACK,
								   zchan->caller_data.hangup_cause);
			}			
		}
		break;
	case ZAP_CHANNEL_STATE_CANCEL:
		{
			sig.event_id = ZAP_SIGEVENT_STOP;
			status = m3ua_data->signal_cb(&sig);
			zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DOWN);
			m3uac_exec_command(mcon,
							   zchan->physical_span_id-1,
							   zchan->physical_chan_id-1,
							   0,
							   SIGBOOST_EVENT_CALL_START_NACK_ACK,
							   0);
		}
		break;
	case ZAP_CHANNEL_STATE_TERMINATING:
		{
			sig.event_id = ZAP_SIGEVENT_STOP;
			status = m3ua_data->signal_cb(&sig);
			zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DOWN);
			m3uac_exec_command(mcon,
							   zchan->physical_span_id-1,
							   zchan->physical_chan_id-1,
							   0,
							   SIGBOOST_EVENT_CALL_STOPPED_ACK,
							   0);
		}
		break;
	default:
		break;
	}
}


static __inline__ void check_state(zap_span_t *span)
{
    if (zap_test_flag(span, ZAP_SPAN_STATE_CHANGE)) {
        uint32_t j;
        zap_clear_flag_locked(span, ZAP_SPAN_STATE_CHANGE);
        for(j = 1; j <= span->chan_count; j++) {
            if (zap_test_flag((&span->channels[j]), ZAP_CHANNEL_STATE_CHANGE)) {
                zap_clear_flag_locked((&span->channels[j]), ZAP_CHANNEL_STATE_CHANGE);
                state_advance(&span->channels[j]);
                zap_channel_complete_state(&span->channels[j]);
            }
        }
    }
}


static int parse_ss7_event(zap_span_t *span, m3uac_connection_t *mcon, m3uac_event_t *event)
{
	zap_mutex_lock(signal_mutex);
	
	if (!zap_running()) {
		zap_log(ZAP_LOG_WARNING, "System is shutting down.\n");
		goto end;
	}


	if (zap_test_flag(span, ZAP_SPAN_SUSPENDED) && 
		event->event_id != SIGBOOST_EVENT_SYSTEM_RESTART_ACK && event->event_id != SIGBOOST_EVENT_HEARTBEAT) {

		zap_log(ZAP_LOG_WARNING,
				"INVALID EVENT: %s:(%X) [w%dg%d] Rc=%i CSid=%i Seq=%i Cd=[%s] Ci=[%s]\n",
				m3uac_event_id_name(event->event_id),
				event->event_id,
				event->span+1,
				event->chan+1,
				event->release_cause,
				event->call_setup_id,
				event->fseqno,
				(event->called_number_digits_count ? (char *) event->called_number_digits : "N/A"),
				(event->calling_number_digits_count ? (char *) event->calling_number_digits : "N/A")
				);
		
		goto end;
	}


	zap_log(ZAP_LOG_DEBUG,
			"RX EVENT: %s:(%X) [w%dg%d] Rc=%i CSid=%i Seq=%i Cd=[%s] Ci=[%s]\n",
			   m3uac_event_id_name(event->event_id),
			   event->event_id,
			   event->span+1,
			   event->chan+1,
			   event->release_cause,
			   event->call_setup_id,
			   event->fseqno,
			   (event->called_number_digits_count ? (char *) event->called_number_digits : "N/A"),
			   (event->calling_number_digits_count ? (char *) event->calling_number_digits : "N/A")
			   );


	
    switch(event->event_id) {

    case SIGBOOST_EVENT_CALL_START:
		//handle_call_start(span, mcon, event);
		break;
    case SIGBOOST_EVENT_CALL_STOPPED:
		//handle_call_stop(span, mcon, event);
		break;
    case SIGBOOST_EVENT_CALL_START_ACK:
		//handle_call_start_ack(mcon, event);
		break;
    case SIGBOOST_EVENT_CALL_START_NACK:
		//handle_call_start_nack(span, mcon, event);
		break;
    case SIGBOOST_EVENT_CALL_ANSWERED:
		//handle_call_answer(span, mcon, event);
		break;
    case SIGBOOST_EVENT_HEARTBEAT:
		//handle_heartbeat(mcon, event);
		break;
    case SIGBOOST_EVENT_CALL_STOPPED_ACK:
    case SIGBOOST_EVENT_CALL_START_NACK_ACK:
		//handle_call_done(span, mcon, event);
		break;
    case SIGBOOST_EVENT_INSERT_CHECK_LOOP:
		//handle_call_loop_start(event);
		break;
    case SIGBOOST_EVENT_REMOVE_CHECK_LOOP:
		//handle_call_stop(event);
		break;
    case SIGBOOST_EVENT_SYSTEM_RESTART_ACK:
		//handle_restart_ack(mcon, span, event);
		break;
    case SIGBOOST_EVENT_AUTO_CALL_GAP_ABATE:
		//handle_gap_abate(event);
		break;
    default:
		zap_log(ZAP_LOG_WARNING, "No handler implemented for [%s]\n", m3uac_event_id_name(event->event_id));
		break;
    }

 end:

	zap_mutex_unlock(signal_mutex);

	return 0;
}

static ZIO_CONFIGURE_FUNCTION(m3ua_configure)
{
	m3ua_channel_profile_t *profile = NULL;

	int ok = 1;

	if (!(profile = (m3ua_channel_profile_t *) hashtable_search(globals.profile_hash, (char *)category))) {
		profile = malloc(sizeof(*profile));
		memset(profile, 0, sizeof(*profile));
		zap_set_string(profile->name, category);
		hashtable_insert(globals.profile_hash, (void *)profile->name, profile);
		zap_log(ZAP_LOG_INFO, "creating profile [%s]\n", category);
	}

//	zap_set_string(m3ua_data->mcon. cfg.local_ip, local_ip);
	if (!strcasecmp(var, "local_sctp_port")) {
		profile->local_port = 30000 ;
		profile->remote_port = 30000;
		profile->cust_span++;
	} 
	ok = 1;
	

	if (ok) {
		zap_log(ZAP_LOG_INFO, "setting param [%s]=[%s] for profile [%s]\n", var, val, category);
	} else {
		zap_log(ZAP_LOG_ERROR, "unknown param [%s]\n", var);
	}

	return ZAP_SUCCESS;
}

static ZIO_CONFIGURE_SPAN_FUNCTION(m3ua_configure_span)
{

	return ZAP_FAIL;
}

static ZIO_OPEN_FUNCTION(m3ua_open) 
{
	
	return ZAP_FAIL;
}

static ZIO_CLOSE_FUNCTION(m3ua_close)
{
	
	return ZAP_FAIL;
}

/*static ZIO_SET_INTERVAL_FUNCTION(m3ua_set_interval)
{
	
	return 0;
}*/

static ZIO_WAIT_FUNCTION(m3ua_wait)
{
	
	return ZAP_FAIL;
}

static ZIO_READ_FUNCTION(m3ua_read)
{
	
	return ZAP_FAIL;
}

static ZIO_WRITE_FUNCTION(m3ua_write)
{
	
	return ZAP_FAIL;
}

static ZIO_COMMAND_FUNCTION(m3ua_command)
{
	return ZAP_FAIL;
}

static ZIO_SPAN_POLL_EVENT_FUNCTION(m3ua_poll_event)
{
	return ZAP_FAIL;
}

static ZIO_SPAN_NEXT_EVENT_FUNCTION(m3ua_next_event)
{
	return ZAP_FAIL;
}


static ZIO_SPAN_DESTROY_FUNCTION(m3ua_span_destroy)
{
	m3ua_span_data_t *span_data = (m3ua_span_data_t *) span->mod_data;
	
	if (span_data) {
		free(span_data);
	}
	
	return ZAP_SUCCESS;
}
static ZIO_CHANNEL_DESTROY_FUNCTION(m3ua_channel_destroy)
{
	m3ua_chan_data_t *chan_data = (m3ua_chan_data_t *) zchan->mod_data;
	m3ua_span_data_t *span_data = (m3ua_span_data_t *) zchan->span->mod_data;
	
	if (!chan_data) {
		return ZAP_FAIL;
	}

	
		



	zap_mutex_destroy(&chan_data->digit_mutex);
	zap_buffer_destroy(&chan_data->digit_buffer);


	zap_safe_free(chan_data);
	
	if (span_data) {
		free(span_data);
	}
	
			
	return ZAP_SUCCESS;
}



static ZIO_GET_ALARMS_FUNCTION(m3ua_get_alarms)
{
	return ZAP_FAIL;
}

static zap_io_interface_t m3ua_interface;

zap_status_t m3ua_init(zap_io_interface_t **zint)
{
	assert(zint != NULL);
	memset(&m3ua_interface, 0, sizeof(m3ua_interface));

	m3ua_interface.name = "m3ua";
	m3ua_interface.configure =  m3ua_configure;
	m3ua_interface.configure_span =  m3ua_configure_span;
	m3ua_interface.open = m3ua_open;
	m3ua_interface.close = m3ua_close;
	m3ua_interface.wait = m3ua_wait;
	m3ua_interface.read = m3ua_read;
	m3ua_interface.write = m3ua_write;
	m3ua_interface.command = m3ua_command;
	m3ua_interface.poll_event = m3ua_poll_event;
	m3ua_interface.next_event = m3ua_next_event;
	m3ua_interface.channel_destroy = m3ua_channel_destroy;
	m3ua_interface.span_destroy = m3ua_span_destroy;
	m3ua_interface.get_alarms = m3ua_get_alarms;
	*zint = &m3ua_interface;

	return ZAP_FAIL;
}

zap_status_t m3ua_destroy(void)
{
	return ZAP_FAIL;
}


static void *m3ua_run(zap_thread_t *me, void *obj)
{
    zap_span_t *span = (zap_span_t *) obj;
    m3ua_data_t *m3ua_data = span->signal_data;
	m3uac_connection_t *mcon, *pcon;
	uint32_t ms = 10, too_long = 60000;
		

	m3ua_data->pcon = m3ua_data->mcon;

	if (m3uac_connection_open(&m3ua_data->mcon,
							  m3ua_data->mcon.cfg.local_ip,
							  m3ua_data->mcon.cfg.local_port,
							  m3ua_data->mcon.cfg.remote_ip,
							  m3ua_data->mcon.cfg.remote_port) < 0) {
		zap_log(ZAP_LOG_DEBUG, "Error: Opening MCON Socket [%d] %s\n", m3ua_data->mcon.socket, strerror(errno));
		goto end;
    }

	if (m3uac_connection_open(&m3ua_data->pcon,
							  m3ua_data->pcon.cfg.local_ip,
							  ++m3ua_data->pcon.cfg.local_port,
							  m3ua_data->pcon.cfg.remote_ip,
							  m3ua_data->pcon.cfg.remote_port) < 0) {
		zap_log(ZAP_LOG_DEBUG, "Error: Opening PCON Socket [%d] %s\n", m3ua_data->pcon.socket, strerror(errno));
		goto end;
    }
	
	mcon = &m3ua_data->mcon;
	pcon = &m3ua_data->pcon;

	top:

	//init_outgoing_array();		

	m3uac_exec_command(mcon,
					   0,
					   0,
					   -1,
					   SIGBOOST_EVENT_SYSTEM_RESTART,
					   0);
	
	while (zap_test_flag(m3ua_data, ZAP_M3UA_RUNNING)) {
		fd_set rfds, efds;
		struct timeval tv = { 0, ms * 1000 };
		int max, activity, i = 0;
		m3uac_event_t *event = NULL;
		
		if (!zap_running()) {
			m3uac_exec_command(mcon,
							   0,
							   0,
							   -1,
							   SIGBOOST_EVENT_SYSTEM_RESTART,
							   0);
			break;
		}

		FD_ZERO(&rfds);
		FD_ZERO(&efds);
		FD_SET(mcon->socket, &rfds);
		FD_SET(mcon->socket, &efds);
		FD_SET(pcon->socket, &rfds);
		FD_SET(pcon->socket, &efds);

		max = ((pcon->socket > mcon->socket) ? pcon->socket : mcon->socket) + 1;
		
		if ((activity = select(max, &rfds, NULL, &efds, &tv)) < 0) {
			goto error;
		}
		
		if (activity) {
			if (FD_ISSET(pcon->socket, &efds) || FD_ISSET(mcon->socket, &efds)) {
				goto error;
			}

			if (FD_ISSET(pcon->socket, &rfds)) {
				if ((event = m3uac_connection_readp(pcon, i))) {
					parse_ss7_event(span, mcon, event);
				} else goto top;
			}

			if (FD_ISSET(mcon->socket, &rfds)) {
				if ((event = m3uac_connection_read(mcon, i))) {
					parse_ss7_event(span, mcon, event);
				} else goto top;
			}
		}
		
		check_state(span);
		mcon->hb_elapsed += ms;
		
		if (mcon->hb_elapsed >= too_long && (mcon->up || !zap_test_flag(span, ZAP_SPAN_SUSPENDED))) {
			zap_set_state_all(span, ZAP_CHANNEL_STATE_RESTART);
			zap_set_flag_locked(span, ZAP_SPAN_SUSPENDED);
			mcon->up = 0;
			zap_log(ZAP_LOG_CRIT, "Lost Heartbeat!\n");
		}

	}

	goto end;

 error:
	zap_log(ZAP_LOG_CRIT, "Socket Error!\n");

 end:

	m3uac_connection_close(&m3ua_data->mcon);
	m3uac_connection_close(&m3ua_data->pcon);

	zap_clear_flag(m3ua_data, ZAP_M3UA_RUNNING);

	zap_log(ZAP_LOG_DEBUG, "M3UA thread ended.\n");
	return NULL;
}
zap_status_t m3ua_start(zap_span_t *span)
{
	m3ua_data_t *m3ua_data = span->signal_data;
	zap_set_flag(m3ua_data, ZAP_M3UA_RUNNING);
	return zap_thread_create_detached(m3ua_run, span);
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
*/
