/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifndef __MB_INPUT_H__
#define __MB_INPUT_H__

#include <stdint.h>
#include <pthread.h>
#include "../linkedlist.h"
#include "../dispatch.h"
#include "input-tcp.h"
#include "input-socket.h"

#ifdef ENABLE_DIRECTFB
#	include "input-directfb.h"
#endif
#ifdef ENABLE_LIBINPUT
#	include "input-libinput.h"
#endif
#ifdef ENABLE_BLUETOOTH
#	include "../bluetooth.h"
#	include "input-bluetooth.h"
#endif
#ifdef ENABLE_WEBREMOTE
#	include "input-web.h"
#endif


#define MBI_RECIPIENT_ANY	(-1)


enum avbox_input_event
{
	MBI_EVENT_NONE,
	MBI_EVENT_PLAY,
	MBI_EVENT_PAUSE,
	MBI_EVENT_STOP,
	MBI_EVENT_MENU,
	MBI_EVENT_BACK,
	MBI_EVENT_ENTER,
	MBI_EVENT_NEXT,
	MBI_EVENT_PREV,
	MBI_EVENT_ARROW_UP,
	MBI_EVENT_ARROW_DOWN,
	MBI_EVENT_ARROW_LEFT,
	MBI_EVENT_ARROW_RIGHT,
	MBI_EVENT_CLEAR,
	MBI_EVENT_INFO,
	MBI_EVENT_VOLUME_UP,
	MBI_EVENT_VOLUME_DOWN,
	MBI_EVENT_KBD_A,
	MBI_EVENT_KBD_B,
	MBI_EVENT_KBD_C,
	MBI_EVENT_KBD_D,
	MBI_EVENT_KBD_E,
	MBI_EVENT_KBD_F,
	MBI_EVENT_KBD_G,
	MBI_EVENT_KBD_H,
	MBI_EVENT_KBD_I,
	MBI_EVENT_KBD_J,
	MBI_EVENT_KBD_K,
	MBI_EVENT_KBD_L,
	MBI_EVENT_KBD_M,
	MBI_EVENT_KBD_N,
	MBI_EVENT_KBD_O,
	MBI_EVENT_KBD_P,
	MBI_EVENT_KBD_Q,
	MBI_EVENT_KBD_R,
	MBI_EVENT_KBD_S,
	MBI_EVENT_KBD_T,
	MBI_EVENT_KBD_U,
	MBI_EVENT_KBD_V,
	MBI_EVENT_KBD_W,
	MBI_EVENT_KBD_X,
	MBI_EVENT_KBD_Y,
	MBI_EVENT_KBD_Z,
	MBI_EVENT_KBD_SPACE,
	MBI_EVENT_TIMER,
	MBI_EVENT_VOLUME_CHANGED,
	MBI_EVENT_PLAYER_NOTIFICATION,
	MBI_EVENT_URL,
	MBI_EVENT_DOWNLOAD,
	MBI_EVENT_CONTEXT,
	MBI_EVENT_TRACK,
	MBI_EVENT_TRACK_LONG,
	MBI_EVENT_EXIT,
	MBI_EVENT_QUIT,
};


/* Message passing structure */
struct avbox_input_message
{
	enum avbox_input_event msg;
	uint8_t *payload;
};


void
avbox_input_sendevent(enum avbox_input_event e, void * const payload);


/**
 * Returns a file descriptor to a pipe where
 * all input events will be sent until the file descriptor is closed,
 * in which case the prior descriptor is closed or until mbi_grab_input()
 * is called again
 */
int
avbox_input_grab(struct avbox_object *obj);


void
avbox_input_release(struct avbox_object *obj);


void
avbox_input_eventfree(struct avbox_input_message *msg);


int
avbox_input_init(int argc, char **argv);


void
avbox_input_shutdown(void);

#endif

