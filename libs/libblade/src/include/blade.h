/*
 * Copyright (c) 2007-2014, Anthony Minessale II
 * All rights reserved.
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

#ifndef _BLADE_H_
#define _BLADE_H_

#include <ks.h>
#include <sodium.h>
#include <libconfig.h>
#include <civetweb.h>
#include <pcre.h>
#include "unqlite.h"
#include "blade_types.h"
#include "blade_stack.h"
#include "blade_identity.h"
#include "blade_transport.h"
#include "blade_rpc.h"
#include "blade_connection.h"
#include "blade_session.h"
#include "blade_protocol.h"
#include "blade_channel.h"
#include "blade_subscription.h"
#include "blade_tuple.h"
#include "blade_web.h"

#include "blade_transportmgr.h"
#include "blade_rpcmgr.h"
#include "blade_routemgr.h"
#include "blade_subscriptionmgr.h"
#include "blade_mastermgr.h"
#include "blade_connectionmgr.h"
#include "blade_sessionmgr.h"
#include "blade_restmgr.h"

#include "blade_transport_wss.h"

KS_BEGIN_EXTERN_C

// legacy for libconfig pre 1.5.0
#if (LIBCONFIG_VER_MAJOR <= 1) && (LIBCONFIG_VER_MINOR <= 4)
#define config_setting_lookup config_lookup_from
#endif

KS_DECLARE(ks_status_t) blade_init(void);
KS_DECLARE(ks_status_t) blade_shutdown(void);

KS_END_EXTERN_C

#endif

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
