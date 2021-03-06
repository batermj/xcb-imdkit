/*
 * (C) Copyright 2014 Weng Xuetian <wengxt@gmail.com>
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */

#include <stdlib.h>
#include <string.h>
#include "ximproto.h"
#include "message.h"
#include "common.h"
#include "parser.h"

uint8_t* _xcb_new_xim_message(uint8_t major_opcode,
                             uint8_t minor_opcode,
                             size_t length,
                             bool swap)
{
    uint8_t* message = calloc(length + XCB_IM_HEADER_SIZE, 1);
    if (message) {
        _xcb_write_xim_message_header(message, major_opcode, minor_opcode, length, swap);
    }

    return message;
}

void _xcb_write_xim_message_header(uint8_t* message,
                                  uint8_t major_opcode,
                                  uint8_t minor_opcode,
                                  size_t length,
                                  bool swap)
{
    uint16_t p_len = length / 4;
    message = uint8_t_write(&major_opcode, message, swap);
    message = uint8_t_write(&minor_opcode, message, swap);
    message = uint16_t_write(&p_len, message, swap);
}

// length is the body without header size in byte
bool _xcb_send_xim_message(xcb_connection_t* conn,
                           xcb_atom_t protocol_atom,
                           xcb_window_t window,
                           uint8_t* data, size_t length,
                           const char* name, size_t len)
{
    if (!data) {
        return false;
    }

    // add header size
    length += XCB_IM_HEADER_SIZE;

    xcb_client_message_event_t event;
    memset(&event, 0, sizeof(event));

    event.response_type = XCB_CLIENT_MESSAGE;
    event.sequence = 0;
    event.window = window;
    event.type = protocol_atom;

    if (length > XCB_XIM_CM_DATA_SIZE) {
        xcb_atom_t atom;

        xcb_intern_atom_cookie_t atom_cookie = xcb_intern_atom(conn, false, len, name);
        xcb_intern_atom_reply_t* atom_reply = xcb_intern_atom_reply(conn, atom_cookie, NULL);
        if (!atom_reply) {
            return false;
        }
        atom = atom_reply->atom;
        free(atom_reply);
        xcb_get_property_cookie_t get_property_cookie = xcb_get_property(conn,
                                                                         false,
                                                                         window,
                                                                         atom,
                                                                         XCB_ATOM_STRING,
                                                                         0L,
                                                                         10000L);

        xcb_get_property_reply_t* get_property_reply = xcb_get_property_reply(conn, get_property_cookie, NULL);
        if (!get_property_reply) {
            return false;
        }
        free(get_property_reply);
        xcb_void_cookie_t cookie = xcb_change_property_checked(conn,
                                            XCB_PROP_MODE_APPEND,
                                            window,
                                            atom,
                                            XCB_ATOM_STRING,
                                            8,
                                            length,
                                            data);
        xcb_generic_error_t* error = NULL;
        if ((error = xcb_request_check(conn, cookie)) != NULL) {
            DebugLog("Error code: %d", error->error_code);
            free(error);
        }
        event.format = 32;
        event.data.data32[0] = length;
        event.data.data32[1] = atom;
        for (size_t i = 2; i < ARRAY_SIZE(event.data.data32); i++)
            event.data.data32[i] = 0;
    } else {
        event.format = 8;

        memcpy(event.data.data8, data, length);
        /* Clear unused field with NULL */
        for (size_t i = length; i < XCB_XIM_CM_DATA_SIZE; i++)
            event.data.data8[i] = 0;
    }
    xcb_send_event(conn, false, window, XCB_EVENT_MASK_NO_EVENT, (const char*) &event);
    xcb_flush(conn);
    return true;
}

void _xcb_send_xim_error_message(xcb_connection_t* conn,
                                 xcb_atom_t protocol_atom,
                                 xcb_window_t window,
                                 bool swap)
{
    // use stack to avoid alloc fails
    uint8_t message[XCB_IM_HEADER_SIZE];
    _xcb_write_xim_message_header(message, XCB_XIM_ERROR, 0, 0, swap);
    _xcb_send_xim_message(conn, protocol_atom, window, message, 0, NULL, 0);
}


uint8_t* _xcb_read_xim_message(xcb_connection_t* conn,
                               xcb_window_t window,
                               xcb_client_message_event_t *ev,
                               xcb_im_packet_header_fr_t* hdr,
                               bool swap)
{
    uint8_t *p = NULL;

    if (ev->format == 8) {
        /* ClientMessage only */
        uint8_t* rec = ev->data.data8;
        size_t len  = sizeof(ev->data.data8);
        uint8_t_read(&hdr->major_opcode, &rec, &len, false);
        uint8_t_read(&hdr->minor_opcode, &rec, &len, false);
        uint16_t_read(&hdr->length, &rec, &len, false);

        // check message is well formed
        if (len >= hdr->length * 4) {
            p = malloc(hdr->length * 4);
        }
        if (p) {
            memcpy(p, rec, hdr->length * 4);
        }
    } else if (ev->format == 32) {
        /* ClientMessage and WindowProperty */
        size_t length = ev->data.data32[0];
        xcb_atom_t atom = ev->data.data32[1];

        xcb_get_property_cookie_t cookie = xcb_get_property(conn,
                                                           true,
                                                           window,
                                                           atom,
                                                           XCB_ATOM_ANY,
                                                           0L,
                                                           length);

        xcb_get_property_reply_t* reply = xcb_get_property_reply(conn, cookie, NULL);
        uint8_t* rec;

        do {
            if (!reply || reply->format == 0 || reply->length == 0) {
                free(reply);
                return (unsigned char *) NULL;
            }

            rec = xcb_get_property_value(reply);

            if (length != reply->value_len)
                length = reply->value_len;

            // make length into byte
            if (reply->format == 16)
                length *= 2;
            else if (reply->format == 32)
                length *= 4;

            uint8_t_read(&hdr->major_opcode, &rec, &length, swap);
            uint8_t_read(&hdr->minor_opcode, &rec, &length, swap);
            uint16_t_read(&hdr->length, &rec, &length, swap);

            // check message is well formed
            if (hdr->length * 4 <= length) {
                /* if hit, it might be an error */
                p = malloc(hdr->length * 4);
            }
        } while(0);

        if (p) {
            memcpy(p, rec, hdr->length * 4);
        }
        free(reply);
    }
    return p;
}
