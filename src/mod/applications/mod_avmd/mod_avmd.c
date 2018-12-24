/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2010, Eric des Courtis <eric.des.courtis@benbria.com>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * Eric des Courtis <eric.des.courtis@benbria.com>
 * Copyright (C) Benbria. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Piotr Gregor <piotrek.gregor gmail.com>:
 * Eric des Courtis <eric.des.courtis@benbria.com>
 *
 * mod_avmd.c -- Advanced Voicemail Detection Module
 *
 * This module detects single frequency tones (used in voicemail to denote
 * the moment caller's voice is started to be recorded, aka. beep sounds,
 * beeps) using modified DESA-2 algorithm.
 */

#include <switch.h>
#include <g711.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef WIN32
#include <float.h>
#define ISNAN(x) (!!(_isnan(x)))
#else
#define ISNAN(x) (isnan(x))
#endif


#include "avmd_buffer.h"
#include "avmd_desa2_tweaked.h"
#include "avmd_sma_buf.h"
#include "avmd_options.h"

#ifdef AVMD_FAST_MATH
#include "avmd_fast_acosf.h"
#endif


/*! Calculate how many audio samples per ms based on the rate */
#define SAMPLES_PER_MS(r, m) ((r) / (1000/(m)))
/*! Minimum beep length */
#define BEEP_TIME (2)
/*! How often to evaluate the output of DESA-2 in ms */
#define SINE_TIME (2*0.125)
/*! How long in samples does DESA-2 results get evaluated */
#define SINE_LEN(r) SAMPLES_PER_MS((r), SINE_TIME)
/*! How long in samples is the minimum beep length */
#define BEEP_LEN(r) SAMPLES_PER_MS((r), BEEP_TIME)
/*! Number of points in DESA-2 sample */
#define P (5)
/*! Guesstimate frame length in ms */
#define FRAME_TIME (20)
/*! Length in samples of the frame (guesstimate) */
#define FRAME_LEN(r) SAMPLES_PER_MS((r), FRAME_TIME)
/*! Conversion to Hertz */
#define TO_HZ(r, f) (((r) * (f)) / (2.0 * M_PI))
/*! Minimum beep frequency in Hertz */
#define MIN_FREQUENCY (300.0)
/*! Minimum frequency as digital normalized frequency */
#define MIN_FREQUENCY_R(r) ((2.0 * M_PI * MIN_FREQUENCY) / (r))
/*! 
 * Maximum beep frequency in Hertz
 * Note: The maximum frequency the DESA-2 algorithm can uniquely
 * identify is 0.25 of the sampling rate. All the frequencies
 * below that level are detected unambiguously. This means 2kHz
 * for 8kHz audio. All the frequencies above 0.25 sampling rate
 * will be aliased to some frequency below that threshold.
 * This is not a problem here as we are interested in detection
 * of any constant amplitude and frequency sine wave instead
 * of detection of particular frequency.
 */
#define MAX_FREQUENCY (2500.0)
/*! Maximum frequency as digital normalized frequency */
#define MAX_FREQUENCY_R(r) ((2.0 * M_PI * MAX_FREQUENCY) / (r))
/* decrease this value to eliminate false positives */
#define VARIANCE_THRESHOLD (0.00025)

#ifdef AVMD_REQUIRE_CONTINUOUS_STREAK
    /* increase this value to eliminate false positives */
    #define SAMPLES_CONSECUTIVE_STREAK 15
#endif

/*! Syntax of the API call. */
#define AVMD_SYNTAX "<uuid> <start|stop>"

/*! Number of expected parameters in api call. */
#define AVMD_PARAMS 2

/*! FreeSWITCH CUSTOM event type. */
#define AVMD_EVENT_BEEP "avmd::beep"

#define AVMD_CHAR_BUF_LEN 20
#define AVMD_BUF_LINEAR_LEN 160


/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_avmd_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_avmd_load);
SWITCH_MODULE_DEFINITION(mod_avmd, mod_avmd_load, mod_avmd_shutdown, NULL);
SWITCH_STANDARD_API(avmd_api_main);
SWITCH_STANDARD_APP(avmd_start_function);

/*! Status of the beep detection */
typedef enum {
	BEEP_DETECTED,
	BEEP_NOTDETECTED
} avmd_beep_state_t;

/*! Data related to the current status of the beep */
typedef struct {
	avmd_beep_state_t beep_state;
	size_t last_beep;
} avmd_state_t;

/*! Type that holds session information pertinent to the avmd module. */
typedef struct {
	/*! Internal FreeSWITCH session. */
	switch_core_session_t *session;
	uint32_t rate;
	circ_buffer_t b;
	sma_buffer_t sma_b;
	sma_buffer_t sqa_b;
	size_t pos;
	double f;
	/* freq_table_t ft; */
	avmd_state_t state;
	switch_time_t start_time;
#ifdef AVMD_REQUIRE_CONTINUOUS_STREAK
    size_t samples_streak; /* number of DESA samples in single streak without reset
                              needed to validate SMA estimator */
#endif
    size_t sample_count;
} avmd_session_t;

static void avmd_process(avmd_session_t *session, switch_frame_t *frame);
static switch_bool_t avmd_callback(switch_media_bug_t * bug,
                                    void *user_data, switch_abc_type_t type);

/*! \brief The avmd session data initialization function.
 * @author Eric des Courtis
 * @param avmd_session A reference to a avmd session.
 * @param fs_session A reference to a FreeSWITCH session.
 */
static switch_status_t
init_avmd_session_data(avmd_session_t *avmd_session,
                                    switch_core_session_t *fs_session)
{
    size_t buf_sz;

	/*! This is a worst case sample rate estimate */
	avmd_session->rate = 48000;
	INIT_CIRC_BUFFER(&avmd_session->b,
            (size_t)BEEP_LEN(avmd_session->rate),
            (size_t)FRAME_LEN(avmd_session->rate),
            fs_session);
    if (avmd_session->b.buf == NULL) {
            return SWITCH_STATUS_MEMERR;
    }
	avmd_session->session = fs_session;
	avmd_session->pos = 0;
	avmd_session->f = 0.0;
	avmd_session->state.last_beep = 0;
	avmd_session->state.beep_state = BEEP_NOTDETECTED;
#ifdef AVMD_REQUIRE_CONTINUOUS_STREAK
    avmd_session->samples_streak = SAMPLES_CONSECUTIVE_STREAK;
#endif
    avmd_session->sample_count = 0;

    buf_sz = BEEP_LEN((uint32_t)avmd_session->rate) / (uint32_t)SINE_LEN(avmd_session->rate);
    if (buf_sz < 1) {
            return SWITCH_STATUS_MORE_DATA;
    }

    INIT_SMA_BUFFER(&avmd_session->sma_b, buf_sz, fs_session);
    if (avmd_session->sma_b.data == NULL) {
            return SWITCH_STATUS_FALSE;
    }
    memset(avmd_session->sma_b.data, 0, sizeof(BUFF_TYPE) * buf_sz);

    INIT_SMA_BUFFER(&avmd_session->sqa_b, buf_sz, fs_session);
    if (avmd_session->sqa_b.data == NULL) {
            return SWITCH_STATUS_FALSE;
    }
    memset(avmd_session->sqa_b.data, 0, sizeof(BUFF_TYPE) * buf_sz);
    return SWITCH_STATUS_SUCCESS;
}


/*! \brief The callback function that is called when new audio data becomes available.
 *
 * @author Eric des Courtis
 * @param bug A reference to the media bug.
 * @param user_data The session information for this call.
 * @param type The switch callback type.
 * @return The success or failure of the function.
 */
static switch_bool_t avmd_callback(switch_media_bug_t * bug,
                                        void *user_data, switch_abc_type_t type)
{
	avmd_session_t          *avmd_session;
#ifdef AVMD_OUTBOUND_CHANNEL
	switch_codec_t          *read_codec;
#endif
#ifdef AVMD_INBOUND_CHANNEL
    switch_codec_t          *write_codec;
#endif
	switch_frame_t          *frame;
    switch_core_session_t   *fs_session;


	avmd_session = (avmd_session_t *) user_data;
	if (avmd_session == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			    "No avmd session assigned!\n");
		return SWITCH_FALSE;
	}
	fs_session = avmd_session->session;
	if (fs_session == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			    "No FreeSWITCH session assigned!\n");
		return SWITCH_FALSE;
	}

	switch (type) {

	case SWITCH_ABC_TYPE_INIT:
#ifdef AVMD_OUTBOUND_CHANNEL
		read_codec = switch_core_session_get_read_codec(fs_session);
        if (read_codec == NULL) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session), SWITCH_LOG_WARNING,
			    "No read codec assigned, default session rate to 8000 samples/s\n");
		    avmd_session->rate = 8000;
        } else {
            if (read_codec->implementation == NULL) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session), SWITCH_LOG_WARNING,
			        "No read codec implementation assigned, default session rate to 8000 samples/s\n");
		        avmd_session->rate = 8000;
            } else {
                avmd_session->rate = read_codec->implementation->samples_per_second;
            }
        }
#endif
#ifdef AVMD_INBOUND_CHANNEL
		write_codec = switch_core_session_get_write_codec(fs_session);
        if (write_codec == NULL) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session), SWITCH_LOG_WARNING,
			    "No write codec assigned, default session rate to 8000 samples/s\n");
		    avmd_session->rate = 8000;
        } else {
            if (write_codec->implementation == NULL) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session), SWITCH_LOG_WARNING,
			        "No write codec implementation assigned, default session rate to 8000 samples/s\n");
		        avmd_session->rate = 8000;
            } else {
                avmd_session->rate = write_codec->implementation->samples_per_second;
            }
        }
#endif

		avmd_session->start_time = switch_micro_time_now();
		/* avmd_session->vmd_codec.channels = 
         *                  read_codec->implementation->number_of_channels; */
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session),SWITCH_LOG_INFO,
			    "Avmd session initialized, [%u] samples/s\n", avmd_session->rate);
		break;

	case SWITCH_ABC_TYPE_READ_REPLACE:
		frame = switch_core_media_bug_get_read_replace_frame(bug);
		avmd_process(avmd_session, frame);
		return SWITCH_TRUE;

	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		frame = switch_core_media_bug_get_write_replace_frame(bug);
		avmd_process(avmd_session, frame);
		return SWITCH_TRUE;

	default:
		break;
	}

	return SWITCH_TRUE;
}

/*! \brief FreeSWITCH module loading function.
 *
 * @author Eric des Courtis
 * @par    Modifications: Piotr Gregor
 * @return On success SWITCH_STATUS_SUCCES,
 *         on failure SWITCH_STATUS_TERM.
 */
SWITCH_MODULE_LOAD_FUNCTION(mod_avmd_load)
{
#ifdef AVMD_FAST_MATH
    char    err[150];
    int     ret;
#endif

	switch_application_interface_t *app_interface;
	switch_api_interface_t *api_interface;
	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	if (switch_event_reserve_subclass(AVMD_EVENT_BEEP) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                "Couldn't register subclass [%s]!\n", AVMD_EVENT_BEEP);
		return SWITCH_STATUS_TERM;
	}
	
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Advanced voicemail detection enabled\n");

#ifdef AVMD_FAST_MATH
    ret = init_fast_acosf();
    if (ret != 0) {
        strerror_r(errno, err, 150);
        switch (ret) {

            case -1:
	            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
		            "Can't access file [%s], error [%s]\n",
                    ACOS_TABLE_FILENAME, err);
                break;
            case -2:
	            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
		            "Error creating file [%s], error [%s]\n",
                    ACOS_TABLE_FILENAME, err);
                break;
            case -3:
	            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
		            "Access rights are OK but can't open file [%s], error [%s]\n",
                    ACOS_TABLE_FILENAME, err);
                break;
            case -4:
	            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
		            "Access rights are OK but can't mmap file [%s], error [%s]\n",
                    ACOS_TABLE_FILENAME, err);
                break;
            default:
	            switch_log_printf(SWITCH_CHANNEL_LOG,SWITCH_LOG_ERROR,
		            "Unknown error [%d] while initializing fast cos table [%s], "
                    "errno [%s]\n", ret, ACOS_TABLE_FILENAME, err);
                return SWITCH_STATUS_TERM;
        }
        return SWITCH_STATUS_TERM;
    } else
	switch_log_printf(
		SWITCH_CHANNEL_LOG,
		SWITCH_LOG_NOTICE,
		"Advanced voicemail detection: fast math enabled, arc cosine table "
        "is [%s]\n", ACOS_TABLE_FILENAME
		);
#endif

	SWITCH_ADD_APP(app_interface, "avmd","Beep detection",
            "Advanced detection of voicemail beeps", avmd_start_function,
            AVMD_SYNTAX, SAF_NONE);

	SWITCH_ADD_API(api_interface, "avmd", "Voicemail beep detection",
            avmd_api_main, AVMD_SYNTAX);

	switch_console_set_complete("add avmd ::console::list_uuid ::[start:stop");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/*! \brief FreeSWITCH application handler function.
 *  This handles calls made from applications such as LUA and the dialplan.
 *
 * @author Eric des Courtis
 * @return Success or failure of the function.
 */
SWITCH_STANDARD_APP(avmd_start_function)
{
	switch_media_bug_t  *bug;
	switch_status_t     status;
	switch_channel_t    *channel;
	avmd_session_t      *avmd_session;
    switch_media_bug_flag_t flags = 0;

	if (session == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			    "No FreeSWITCH session assigned!\n");
		return;
    }

	channel = switch_core_session_get_channel(session);

	/* Is this channel already using avmd ? */
	bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_avmd_");
	/* If it is using avmd */
	if (bug != NULL) {
		/* If we have a stop remove audio bug */
		if (strcasecmp(data, "stop") == 0) {
			switch_channel_set_private(channel, "_avmd_", NULL);
			switch_core_media_bug_remove(session, &bug);
			return;
		}
		/* We have already started */
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_WARNING, "Cannot run 2 at once on the same channel!\n");
		return;
	}

	avmd_session = (avmd_session_t *)switch_core_session_alloc(
                                            session, sizeof(avmd_session_t));

	status = init_avmd_session_data(avmd_session, session);
    if (status != SWITCH_STATUS_SUCCESS) {
        switch (status) {
            case SWITCH_STATUS_MEMERR:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR, "Failed to init avmd session."
                    " Buffer error!\n");
            break;
            case SWITCH_STATUS_MORE_DATA:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR, "Failed to init avmd session."
                    " SMA buffer size is 0!\n");
                break;
            case SWITCH_STATUS_FALSE:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR, "Failed to init avmd session."
                    " SMA buffers error\n");
                break;
            default:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR, "Failed to init avmd session."
                    " Unknown error\n");
                break;

        }
		return;
    }

#ifdef AVMD_INBOUND_CHANNEL
    flags |= SMBF_READ_REPLACE;
#endif
#ifdef AVMD_OUTBOUND_CHANNEL
    flags |= SMBF_WRITE_REPLACE;
#endif
    switch_assert(flags != 0);

	status = switch_core_media_bug_add(
		session,
		"avmd",
		NULL,
		avmd_callback,
		avmd_session,
		0,
		flags,
		&bug
		);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR, "Failure hooking to stream\n");

		return;
	}

	switch_channel_set_private(channel, "_avmd_", bug);
}

/*! \brief Called when the module shuts down.
 *
 * @author Eric des Courtis
 * @return The success or failure of the function.
 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_avmd_shutdown)
{
#ifdef AVMD_FAST_MATH
	int res;
#endif

	switch_event_free_subclass(AVMD_EVENT_BEEP);
	
#ifdef AVMD_FAST_MATH
	res = destroy_fast_acosf();
    if (res != 0) {
        switch (res) {
            case -1:
	            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
		            "Failed unmap arc cosine table\n");
                break;
            case -2:
	            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
		            "Failed closing arc cosine table\n");
                break;
            default:
            break;
        }
    }
#endif

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Advanced voicemail detection disabled\n");

	return SWITCH_STATUS_SUCCESS;
}

/*! \brief FreeSWITCH API handler function.
 *  This function handles API calls such as the ones
 *  from mod_event_socket and in some cases
 *  scripts such as LUA scripts.
 *
 *  @author Eric des Courtis
 *  @return The success or failure of the function.
 */
SWITCH_STANDARD_API(avmd_api_main)
{
	switch_media_bug_t  *bug;
	avmd_session_t      *avmd_session;
	switch_channel_t    *channel;
	int     argc;
	char    *argv[AVMD_PARAMS];
	char    *ccmd = NULL;
	char    *uuid, *uuid_dup;
	char    *command;
    switch_core_media_flag_t flags = 0;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_core_session_t   *fs_session = NULL;

	/* No command? Display usage */
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR, bad command!\n"
                "-USAGE: %s\n\n", AVMD_SYNTAX);
		goto end;
	}

	/* Duplicated contents of original string */
	ccmd = strdup(cmd);
	/* Separate the arguments */
	argc = switch_separate_string(ccmd, ' ', argv, AVMD_PARAMS);

	/* If we don't have the expected number of parameters
	* display usage */
	if (argc != AVMD_PARAMS) {
		stream->write_function(stream, "-ERR, avmd takes [%u] parameters!\n"
                "-USAGE: %s\n\n", AVMD_PARAMS, AVMD_SYNTAX);
		goto end;
	}

	uuid = argv[0];
	command = argv[1];

	/* using uuid locate a reference to the FreeSWITCH session */
	fs_session = switch_core_session_locate(uuid);

	/* If the session was not found exit */
	if (fs_session == NULL) {
		stream->write_function(stream, "-ERR, no FreeSWITCH session for uuid [%s]!"
                "\n-USAGE: %s\n\n", uuid, AVMD_SYNTAX);
		goto end;
	}

	/* Get current channel of the session to tag the session
	* This indicates that our module is present
    * At this moment this cannot return NULL, it will either
    * succeed or assert failed, but we make ourself secure anyway */
	channel = switch_core_session_get_channel(fs_session);
	if (channel == NULL) {
		stream->write_function(stream, "-ERR, no channel for FreeSWITCH session [%s]!"
                "\n Please report this to the developers\n\n", uuid);
		goto end;
	}

	/* Is this channel already set? */
	bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_avmd_");
	/* If yes */
	if (bug != NULL) {
		/* If we have a stop remove audio bug */
		if (strcasecmp(command, "stop") == 0) {
            uuid_dup = switch_core_strdup(switch_core_session_get_pool(fs_session), uuid);
			switch_channel_set_private(channel, "_avmd_", NULL);
			switch_core_media_bug_remove(fs_session, &bug);
			switch_safe_free(ccmd);
#ifdef AVMD_REPORT_STATUS
			stream->write_function(stream, "+OK\n [%s] [%s] stopped\n\n",
                    uuid_dup, switch_channel_get_name(channel));
		    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session), SWITCH_LOG_INFO,
			    "Avmd on channel [%s] stopped!\n", switch_channel_get_name(channel));
#endif
			goto end;
		}

#ifdef AVMD_REPORT_STATUS
		/* We have already started */
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session),
                SWITCH_LOG_ERROR, "Avmd already started!\n");
		stream->write_function(stream, "-ERR, avmd for FreeSWITCH session [%s]"
                "\n already started\n\n", uuid);
#endif
		goto end;
	}

#ifdef AVMD_OUTBOUND_CHANNEL
    if (SWITCH_CALL_DIRECTION_OUTBOUND != switch_channel_direction(channel)) {
		stream->write_function(stream, "-ERR, channel for FreeSWITCH session [%s]"
                "\n is not outbound\n\n", uuid);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session), SWITCH_LOG_WARNING,
			"Channel [%s] is not outbound!\n", switch_channel_get_name(channel));
    } else {
        flags |= SMBF_READ_REPLACE;
    }
#endif
#ifdef AVMD_INBOUND_CHANNEL
    if (SWITCH_CALL_DIRECTION_INBOUND != switch_channel_direction(channel)) {
		stream->write_function(stream, "-ERR, channel for FreeSWITCH session [%s]"
                "\n is not inbound\n\n", uuid);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session), SWITCH_LOG_WARNING,
			"Channel [%s] is not inbound!\n", switch_channel_get_name(channel));
    } else {
        flags |= SMBF_WRITE_REPLACE;
    }
#endif
    if(flags == 0) {
		stream->write_function(stream, "-ERR, can't set direction for channel [%s]\n"
               " for FreeSWITCH session [%s]. Please check avmd configuration\n\n",
               switch_channel_get_name(channel), uuid);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session), SWITCH_LOG_ERROR,
			"Can't set direction for channel [%s]\n", switch_channel_get_name(channel));
        goto end;
    }


#ifdef AVMD_OUTBOUND_CHANNEL
    if (switch_channel_test_flag(channel, CF_MEDIA_SET) == 0) {
		stream->write_function(stream, "-ERR, channel [%s] for FreeSWITCH session [%s]"
                "\n has no read codec assigned yet. Please try again.\n\n",
                switch_channel_get_name(channel), uuid);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session), SWITCH_LOG_ERROR,
			"Failed to start session. Channel [%s] has no codec assigned yet."
            " Please try again\n", switch_channel_get_name(channel));
        goto end;
    }
#endif

	/* If we don't see the expected start exit */
	if (strcasecmp(command, "start") != 0) {
		stream->write_function(stream, "-ERR, did you mean\n"
                " api avmd %s start ?\n-USAGE: %s\n\n", uuid, AVMD_SYNTAX);
		goto end;
	}

	/* Allocate memory attached to this FreeSWITCH session for
	* use in the callback routine and to store state information */
    avmd_session = (avmd_session_t *) switch_core_session_alloc(
                                            fs_session, sizeof(avmd_session_t));
    status = init_avmd_session_data(avmd_session, fs_session);
    if (status != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR, failed to initialize avmd session\n"
                " for FreeSWITCH session [%s]\n", uuid);
        switch (status) {
            case SWITCH_STATUS_MEMERR:
		        stream->write_function(stream, "-ERR, buffer error\n\n");
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session),
                    SWITCH_LOG_ERROR, "Failed to init avmd session."
                    " Buffer error!\n");
            break;
            case SWITCH_STATUS_MORE_DATA:
		        stream->write_function(stream, "-ERR, SMA buffer size is 0\n\n");
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session),
                    SWITCH_LOG_ERROR, "Failed to init avmd session."
                    " SMA buffer size is 0!\n");
                break;
            case SWITCH_STATUS_FALSE:
		        stream->write_function(stream, "-ERR, SMA buffer error\n\n");
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session),
                    SWITCH_LOG_ERROR, "Failed to init avmd session."
                    " SMA buffers error\n");
                break;
            default:
		        stream->write_function(stream, "-ERR, unknown error\n\n");
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session),
                    SWITCH_LOG_ERROR, "Failed to init avmd session."
                    " Unknown error\n");
                break;
        }
		goto end;
    }

	/* Add a media bug that allows me to intercept the
	* reading leg of the audio stream */
	status = switch_core_media_bug_add(
		fs_session,
		"avmd",
		NULL,
		avmd_callback,
		avmd_session,
		0,
		flags,
		&bug
		);

	/* If adding a media bug fails exit */
	if (status != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session),
			SWITCH_LOG_ERROR, "Failed to add media bug!\n");
		stream->write_function(stream,
                "-ERR, [%s] failed to add media bug!\n\n", uuid);
		goto end;
	}

	/* Set the vmd tag to detect an existing vmd media bug */
	switch_channel_set_private(channel, "_avmd_", bug);

	/* OK */
#ifdef AVMD_REPORT_STATUS
	stream->write_function(stream, "+OK\n [%s] [%s] started!\n\n",
            uuid, switch_channel_get_name(channel));
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fs_session), SWITCH_LOG_INFO,
            "Avmd on channel [%s] started!\n", switch_channel_get_name(channel));
    switch_assert(status == SWITCH_STATUS_SUCCESS);
#endif
end:

	if (fs_session) {
		switch_core_session_rwunlock(fs_session);
	}

	switch_safe_free(ccmd);

	return SWITCH_STATUS_SUCCESS;
}

/*! \brief Process one frame of data with avmd algorithm.
 * @author Eric des Courtis
 * @par Modifications: Piotr Gregor
 * @param session An avmd session.
 * @param frame An audio frame.
 */
static void avmd_process(avmd_session_t *session, switch_frame_t *frame)
{
    int res;
    switch_event_t      *event;
    switch_status_t     status;
    switch_event_t      *event_copy;
    switch_channel_t    *channel;

    circ_buffer_t       *b;
    size_t              pos;
    double              omega;
#ifdef AVMD_DEBUG
    double f;
#endif
    double      v;
    double      sma_digital_freq;
    uint32_t    sine_len_i;
    char        buf[AVMD_CHAR_BUF_LEN];
    int         sample_to_skip_n = AVMD_SAMLPE_TO_SKIP_N;
    size_t      sample_n = 0;

    b = &session->b;

    /* If beep has already been detected skip the CPU heavy stuff */
    if (session->state.beep_state == BEEP_DETECTED) return;

    /* Precompute values used heavily in the inner loop */
    sine_len_i = (uint32_t) SINE_LEN(session->rate);
    //sine_len = (double)sine_len_i;
    //beep_len_i = BEEP_LEN(session->rate);

    channel = switch_core_session_get_channel(session->session);

    /* Insert frame of 16 bit samples into buffer */
    INSERT_INT16_FRAME(b, (int16_t *)(frame->data), frame->samples);
    session->sample_count += frame->samples;

    /* INNER LOOP -- OPTIMIZATION TARGET */
    pos = session->pos;
    while (sample_n < (frame->samples - P)) {
	/*for (pos = session->pos; pos < (GET_CURRENT_POS(b) - P); pos++) { */
		if ((sample_n % sine_len_i) == 0) {
			/* Get a desa2 frequency estimate every sine len */
			omega = avmd_desa2_tweaked(b, pos + sample_n);

			if (omega < -0.999999 || omega > 0.999999) {
#ifdef AVMD_DEBUG
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session->session),
                        SWITCH_LOG_DEBUG, "<<< AVMD RESET >>>\n");
#endif
				v = 99999.0;
#ifdef AVMD_REQUIRE_CONTINUOUS_STREAK
				RESET_SMA_BUFFER(&session->sma_b);
				RESET_SMA_BUFFER(&session->sqa_b);
                session->samples_streak = SAMPLES_CONSECUTIVE_STREAK;
                sample_to_skip_n = AVMD_SAMLPE_TO_SKIP_N;
#endif
			} else {
                if (isnan(omega)) {
#ifdef AVMD_DEBUG
	                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session->session),
                            SWITCH_LOG_DEBUG, "<<< AVMD, SKIP NaN >>>\n");
#endif
                    sample_to_skip_n = AVMD_SAMLPE_TO_SKIP_N;
                    goto loop_continue;
                }
                if (session->sma_b.pos > 0 && 
                        (fabs(omega - session->sma_b.data[session->sma_b.pos - 1]) < 0.00000001)) {
#ifdef AVMD_DEBUG
	                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session->session), SWITCH_LOG_DEBUG,
                        "<<< AVMD, SKIP >>>\n");
#endif
                    goto loop_continue;
                }
#ifdef AVMD_DEBUG
	            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session->session),
                        SWITCH_LOG_DEBUG, "<<< AVMD omega [%f] >>>\n", omega);
#endif
                if (sample_to_skip_n > 0) {
                    sample_to_skip_n--;
                    goto loop_continue;
                }

                /* saturate */
                if (omega < -0.9999)
                    omega = -0.9999;
                if (omega > 0.9999)
                    omega = 0.9999;

                /* append */
				APPEND_SMA_VAL(&session->sma_b, omega);
				APPEND_SMA_VAL(&session->sqa_b, omega * omega);
#ifdef AVMD_REQUIRE_CONTINUOUS_STREAK
                if (session->samples_streak > 0)
                    --session->samples_streak;
#endif
				/* calculate variance (biased estimator) */
				v = session->sqa_b.sma - (session->sma_b.sma * session->sma_b.sma);
#ifdef AVMD_DEBUG
    #ifdef  AVMD_FAST_MATH
                f =  0.5 * (double) fast_acosf((float)omega);
                sma_digital_freq =  0.5 * (double) fast_acosf((float)session->sma_b.sma);
    #else
                f = 0.5 * acos(omega);
                sma_digital_freq =  0.5 * acos(session->sma_b.sma);
    #endif /* AVMD_FAST_MATH */
    #ifdef AVMD_REQUIRE_CONTINUOUS_STREAK
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session->session), SWITCH_LOG_DEBUG,
                    "<<< AVMD v[%.10f]\tomega[%f]\tf[%f] [%f]Hz\t\tsma[%f][%f]Hz\t\tsqa[%f]\t"
                    "streak[%zu] pos[%zu] sample_n[%zu] lpos[%zu] s[%zu]>>>\n",
                    v, omega, f, TO_HZ(session->rate, f), session->sma_b.sma,
                    TO_HZ(session->rate, sma_digital_freq), session->sqa_b.sma, session->samples_streak,
                    session->sma_b.pos, sample_n, session->sma_b.lpos, pos);
    #else
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session->session), SWITCH_LOG_DEBUG,
                    "<<< AVMD v[%.10f]\tomega[%f]\tf[%f] [%f]Hz\t\tsma[%f][%f]Hz\t\tsqa[%f]\tpos[%zu]"
                    " sample_n[%zu] lpos[%zu] s[%zu]>>>\n", v, omega, f,
                    TO_HZ(session->rate, f), session->sma_b.sma, TO_HZ(session->rate, sma_digital_freq),
                    session->sqa_b.sma, session->sma_b.pos, sample_n, session->sma_b.lpos, pos);
    #endif  /* AVMD_REQUIRE_CONTINUOUS_STREAK */
#endif  /* AVMD_DEBUG */
			}

            /* DECISION */
            /* If variance is less than threshold
             * and we have at least two estimates
             * then we have detection */
#ifdef AVMD_REQUIRE_CONTINUOUS_STREAK
			if (v < VARIANCE_THRESHOLD && (session->sma_b.lpos > 1) && (session->samples_streak == 0)) {
#else
			if (v < VARIANCE_THRESHOLD && (session->sma_b.lpos > 1)) {
#endif
    #ifdef  AVMD_FAST_MATH
                sma_digital_freq =  0.5 * (double) fast_acosf((float)session->sma_b.sma);
    #else
                sma_digital_freq =  0.5 * acos(session->sma_b.sma);
    #endif /* AVMD_FAST_MATH */
				switch_channel_set_variable_printf(channel, "avmd_total_time",
                        "[%d]", (int)(switch_micro_time_now() - session->start_time) / 1000);
				switch_channel_execute_on(channel, "execute_on_avmd_beep");

				/* Throw an event to FreeSWITCH */
				status = switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, AVMD_EVENT_BEEP);
				if (status != SWITCH_STATUS_SUCCESS) return;

				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Beep-Status", "stop");
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID",
                    switch_core_session_get_uuid(session->session));
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "avmd");
                res = snprintf(buf, AVMD_CHAR_BUF_LEN, "%f",
                            TO_HZ(session->rate, sma_digital_freq));
                if (res < 0 || res > AVMD_CHAR_BUF_LEN - 1) {
				    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session->session), SWITCH_LOG_ERROR,
                        "Frequency truncated [%s], [%d] attempted!\n", buf, res);
				    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "frequency", "ERROR (TRUNCATED)");
                }
                switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "frequency", buf);

                res = snprintf(buf, AVMD_CHAR_BUF_LEN, "%f", v);
                if (res < 0 || res > AVMD_CHAR_BUF_LEN - 1) {
				    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session->session), SWITCH_LOG_ERROR,
                        "Error, truncated [%s], [%d] attempeted!\n", buf, res);
				    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variance", "ERROR (TRUNCATED)");
                }
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variance", buf);

				if ((switch_event_dup(&event_copy, event)) != SWITCH_STATUS_SUCCESS) return;

				switch_core_session_queue_event(session->session, &event);
				switch_event_fire(&event_copy);

#ifdef AVMD_REPORT_STATUS
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session->session), SWITCH_LOG_INFO,
                        "<<< AVMD - Beep Detected: f = [%f], variance = [%f] >>>\n",
                        TO_HZ(session->rate, sma_digital_freq), v);
#endif
				switch_channel_set_variable(channel, "avmd_detect", "TRUE");
				RESET_SMA_BUFFER(&session->sma_b);
				RESET_SMA_BUFFER(&session->sqa_b);
				session->state.beep_state = BEEP_DETECTED;

				goto done;
            }
		}
loop_continue:
        ++sample_n;
	}

done:
	session->pos += sample_n;
    session->pos &= b->mask;

    return;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */

