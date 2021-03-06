/*
 * SpanDSP - a series of DSP components for telephony
 *
 * t38_terminal.c - T.38 termination, less the packet exchange part
 *
 * Written by Steve Underwood <steveu@coppice.org>
 *
 * Copyright (C) 2005, 2006, 2008 Steve Underwood
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id: t38_terminal.c,v 1.125 2009/05/02 04:43:48 steveu Exp $
 */

/*! \file */

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#if !defined(HAVE_TIFF_H)
#else

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#if defined(HAVE_TGMATH_H)
#include <tgmath.h>
#endif
#if defined(HAVE_MATH_H)
#include <math.h>
#endif
#include "floating_fudge.h"
#include <assert.h>
#include <tiffio.h>

#include "spandsp/telephony.h"
#include "spandsp/logging.h"
#include "spandsp/bit_operations.h"
#include "spandsp/queue.h"
#include "spandsp/power_meter.h"
#include "spandsp/complex.h"
#include "spandsp/tone_generate.h"
#include "spandsp/async.h"
#include "spandsp/hdlc.h"
#include "spandsp/fsk.h"
#include "spandsp/v29tx.h"
#include "spandsp/v29rx.h"
#include "spandsp/v27ter_tx.h"
#include "spandsp/v27ter_rx.h"
#include "spandsp/v17tx.h"
#include "spandsp/v17rx.h"
#include "spandsp/t4.h"
#include "spandsp/t30_fcf.h"
#include "spandsp/t35.h"
#include "spandsp/t30.h"
#include "spandsp/t30_api.h"
#include "spandsp/t30_logging.h"
#include "spandsp/t38_core.h"
#include "spandsp/t38_terminal.h"

#include "spandsp/private/logging.h"
#include "spandsp/private/t4.h"
#include "spandsp/private/t30.h"
#include "spandsp/private/t38_core.h"
#include "spandsp/private/t38_terminal.h"

/* Settings suitable for paced transmission over a UDP transport */
#define MS_PER_TX_CHUNK                         30

#define INDICATOR_TX_COUNT                      3
#define DATA_TX_COUNT                           1
#define DATA_END_TX_COUNT                       3

/* Settings suitable for unpaced transmission over a TCP transport */
#define MAX_OCTETS_PER_UNPACED_CHUNK            300

/* Backstop timeout if reception of packets stops in the middle of a burst */
#define MID_RX_TIMEOUT                          15000

enum
{
    T38_CHUNKING_MERGE_FCS_WITH_DATA    = 0x0001,
    T38_CHUNKING_WHOLE_FRAMES           = 0x0002,
    T38_CHUNKING_ALLOW_TEP_TIME         = 0x0004
};

enum
{
    T38_TIMED_STEP_NONE = 0,
    T38_TIMED_STEP_NON_ECM_MODEM = 0x10,
    T38_TIMED_STEP_NON_ECM_MODEM_2 = 0x11,
    T38_TIMED_STEP_NON_ECM_MODEM_3 = 0x12,
    T38_TIMED_STEP_NON_ECM_MODEM_4 = 0x13,
    T38_TIMED_STEP_NON_ECM_MODEM_5 = 0x14,
    T38_TIMED_STEP_HDLC_MODEM = 0x20,
    T38_TIMED_STEP_HDLC_MODEM_2 = 0x21,
    T38_TIMED_STEP_HDLC_MODEM_3 = 0x22,
    T38_TIMED_STEP_HDLC_MODEM_4 = 0x23,
    T38_TIMED_STEP_HDLC_MODEM_5 = 0x24,
    T38_TIMED_STEP_CED = 0x30,
    T38_TIMED_STEP_CED_2 = 0x31,
    T38_TIMED_STEP_CED_3 = 0x32,
    T38_TIMED_STEP_CNG = 0x40,
    T38_TIMED_STEP_CNG_2 = 0x41,
    T38_TIMED_STEP_PAUSE = 0x50
};

static __inline__ void front_end_status(t38_terminal_state_t *s, int status)
{
    t30_front_end_status(&s->t30, status);
}
/*- End of function --------------------------------------------------------*/

static __inline__ void hdlc_accept_frame(t38_terminal_state_t *s, const uint8_t *msg, int len, int ok)
{
    t30_hdlc_accept(&s->t30, msg, len, ok);
}
/*- End of function --------------------------------------------------------*/

static int extra_bits_in_stuffed_frame(const uint8_t buf[], int len)
{
    int bitstream;
    int ones;
    int stuffed;
    int i;
    int j;
    
    bitstream = 0;
    ones = 0;
    stuffed = 0;
    /* We should really append the CRC, and include the stuffed bits for that, to get
       the exact number of bits in the frame. */
    //len = crc_itu16_append(buf, len);
    for (i = 0;  i < len;  i++)
    {
        bitstream = buf[i];
        for (j = 0;  j < 8;  j++)
        {
            if ((bitstream & 1))
            {
                if (++ones >= 5)
                {
                    ones = 0;
                    stuffed++;
                }
            }
            else
            {
                ones = 0;
            }
            bitstream >>= 1;
        }
    }
    /* The total length of the frame is:
          the number of bits in the body
        + the number of additional bits in the body due to stuffing
        + the number of bits in the CRC
        + the number of additional bits in the CRC due to stuffing
        + 16 bits for the two terminating flag octets.
       Lets just allow 3 bits for the CRC, which is the worst case. It
       avoids calculating the real CRC, and the worst it can do is cause
       a flag octet's worth of additional output.
    */
    return stuffed + 16 + 3 + 16;
}
/*- End of function --------------------------------------------------------*/

static int process_rx_missing(t38_core_state_t *t, void *user_data, int rx_seq_no, int expected_seq_no)
{
    t38_terminal_state_t *s;
    
    s = (t38_terminal_state_t *) user_data;
    s->t38_fe.rx_data_missing = TRUE;
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int process_rx_indicator(t38_core_state_t *t, void *user_data, int indicator)
{
    t38_terminal_state_t *s;
    t38_terminal_front_end_state_t *fe;
    
    s = (t38_terminal_state_t *) user_data;
    fe = &s->t38_fe;

    if (t->current_rx_indicator == indicator)
    {
        /* This is probably due to the far end repeating itself, or slipping
           preamble messages in between HDLC frames. T.38/V.1.3 tells us to
           ignore it. Its harmless. */
        return 0;
    }
    /* In termination mode we don't care very much about indicators telling us training
       is starting. We only care about V.21 preamble starting, for timeout control, and
       the actual data. */
    switch (indicator)
    {
    case T38_IND_NO_SIGNAL:
        if (t->current_rx_indicator == T38_IND_V21_PREAMBLE
            &&
            (fe->current_rx_type == T30_MODEM_V21  ||  fe->current_rx_type == T30_MODEM_CNG))
        {
            hdlc_accept_frame(s, NULL, SIG_STATUS_CARRIER_DOWN, TRUE);
        }
        fe->timeout_rx_samples = 0;
        front_end_status(s, T30_FRONT_END_SIGNAL_ABSENT);
        break;
    case T38_IND_CNG:
        /* We are completely indifferent to the startup tones. They serve no purpose for us.
           We can't even assume that the existance of a tone means the far end is achieving
           proper communication. Some T.38 gateways will just send out a CED or CNG indicator
           without having seen anything from the far end FAX terminal.
           Just report them for completeness. */
        front_end_status(s, T30_FRONT_END_CNG_PRESENT);
        break;
    case T38_IND_CED:
        front_end_status(s, T30_FRONT_END_CED_PRESENT);
        break;
    case T38_IND_V21_PREAMBLE:
        /* Some T.38 implementations insert these preamble indicators between HDLC frames, so
           we need to be tolerant of that. */
        fe->timeout_rx_samples = fe->samples + ms_to_samples(MID_RX_TIMEOUT);
        front_end_status(s, T30_FRONT_END_SIGNAL_PRESENT);
        break;
    case T38_IND_V27TER_2400_TRAINING:
    case T38_IND_V27TER_4800_TRAINING:
    case T38_IND_V29_7200_TRAINING:
    case T38_IND_V29_9600_TRAINING:
    case T38_IND_V17_7200_SHORT_TRAINING:
    case T38_IND_V17_7200_LONG_TRAINING:
    case T38_IND_V17_9600_SHORT_TRAINING:
    case T38_IND_V17_9600_LONG_TRAINING:
    case T38_IND_V17_12000_SHORT_TRAINING:
    case T38_IND_V17_12000_LONG_TRAINING:
    case T38_IND_V17_14400_SHORT_TRAINING:
    case T38_IND_V17_14400_LONG_TRAINING:
    case T38_IND_V33_12000_TRAINING:
    case T38_IND_V33_14400_TRAINING:
        /* We really don't care what kind of modem is delivering the following image data.
           We only care that some kind of fast modem signal is coming next. */
        fe->timeout_rx_samples = fe->samples + ms_to_samples(MID_RX_TIMEOUT);
        front_end_status(s, T30_FRONT_END_SIGNAL_PRESENT);
        break;
    case T38_IND_V8_ANSAM:
    case T38_IND_V8_SIGNAL:
    case T38_IND_V34_CNTL_CHANNEL_1200:
    case T38_IND_V34_PRI_CHANNEL:
    case T38_IND_V34_CC_RETRAIN:
        /* V.34 support is a work in progress. */
        front_end_status(s, T30_FRONT_END_SIGNAL_PRESENT);
        break;
    default:
        front_end_status(s, T30_FRONT_END_SIGNAL_ABSENT);
        break;
    }
    fe->hdlc_rx.len = 0;
    fe->rx_data_missing = FALSE;
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int process_rx_data(t38_core_state_t *t, void *user_data, int data_type, int field_type, const uint8_t *buf, int len)
{
    t38_terminal_state_t *s;
    t38_terminal_front_end_state_t *fe;
#if defined(_MSC_VER)
    uint8_t *buf2 = (uint8_t *) _alloca(len);
#else
    uint8_t buf2[len];
#endif

    s = (t38_terminal_state_t *) user_data;
    fe = &s->t38_fe;
#if 0
    /* In termination mode we don't care very much what the data type is. */
    switch (data_type)
    {
    case T38_DATA_V21:
    case T38_DATA_V27TER_2400:
    case T38_DATA_V27TER_4800:
    case T38_DATA_V29_7200:
    case T38_DATA_V29_9600:
    case T38_DATA_V17_7200:
    case T38_DATA_V17_9600:
    case T38_DATA_V17_12000:
    case T38_DATA_V17_14400:
    case T38_DATA_V8:
    case T38_DATA_V34_PRI_RATE:
    case T38_DATA_V34_CC_1200:
    case T38_DATA_V34_PRI_CH:
    case T38_DATA_V33_12000:
    case T38_DATA_V33_14400:
    default:
        break;
    }
#endif
    switch (field_type)
    {
    case T38_FIELD_HDLC_DATA:
        if (fe->timeout_rx_samples == 0)
        {
            /* HDLC can just start without any signal indicator on some platforms, even when
               there is zero packet lost. Nasty, but true. Its a good idea to be tolerant of
               loss, though, so accepting a sudden start of HDLC data is the right thing to do. */
            fe->timeout_rx_samples = fe->samples + ms_to_samples(MID_RX_TIMEOUT);
            front_end_status(s, T30_FRONT_END_SIGNAL_PRESENT);
            /* All real HDLC messages in the FAX world start with 0xFF. If this one is not starting
               with 0xFF it would appear some octets must have been missed before this one. */
            if (len <= 0  ||  buf[0] != 0xFF)
                fe->rx_data_missing = TRUE;
        }
        if (len > 0  &&  fe->hdlc_rx.len + len <= T38_MAX_HDLC_LEN)
        {
            bit_reverse(fe->hdlc_rx.buf + fe->hdlc_rx.len, buf, len);
            fe->hdlc_rx.len += len;
        }
        fe->timeout_rx_samples = fe->samples + ms_to_samples(MID_RX_TIMEOUT);
        break;
    case T38_FIELD_HDLC_FCS_OK:
        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_WARNING, "There is data in a T38_FIELD_HDLC_FCS_OK!\n");
            /* The sender has incorrectly included data in this message. It is unclear what we should do
               with it, to maximise tolerance of buggy implementations. */
        }
        /* Some T.38 implementations send multiple T38_FIELD_HDLC_FCS_OK messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "Type %s - CRC OK (%s)\n", (fe->hdlc_rx.len >= 3)  ?  t30_frametype(fe->hdlc_rx.buf[2])  :  "???", (fe->rx_data_missing)  ?  "missing octets"  :  "clean");
            hdlc_accept_frame(s, fe->hdlc_rx.buf, fe->hdlc_rx.len, !fe->rx_data_missing);
        }
        fe->hdlc_rx.len = 0;
        fe->rx_data_missing = FALSE;
        fe->timeout_rx_samples = fe->samples + ms_to_samples(MID_RX_TIMEOUT);
        break;
    case T38_FIELD_HDLC_FCS_BAD:
        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_WARNING, "There is data in a T38_FIELD_HDLC_FCS_BAD!\n");
            /* The sender has incorrectly included data in this message. We can safely ignore it, as the
               bad FCS means we will throw away the whole message, anyway. */
        }
        /* Some T.38 implementations send multiple T38_FIELD_HDLC_FCS_BAD messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "Type %s - CRC bad (%s)\n", (fe->hdlc_rx.len >= 3)  ?  t30_frametype(fe->hdlc_rx.buf[2])  :  "???", (fe->rx_data_missing)  ?  "missing octets"  :  "clean");
            hdlc_accept_frame(s, fe->hdlc_rx.buf, fe->hdlc_rx.len, FALSE);
        }
        fe->hdlc_rx.len = 0;
        fe->rx_data_missing = FALSE;
        fe->timeout_rx_samples = fe->samples + ms_to_samples(MID_RX_TIMEOUT);
        break;
    case T38_FIELD_HDLC_FCS_OK_SIG_END:
        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_WARNING, "There is data in a T38_FIELD_HDLC_FCS_OK_SIG_END!\n");
            /* The sender has incorrectly included data in this message. It is unclear what we should do
               with it, to maximise tolerance of buggy implementations. */
        }
        /* Some T.38 implementations send multiple T38_FIELD_HDLC_FCS_OK_SIG_END messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "Type %s - CRC OK, sig end (%s)\n", (fe->hdlc_rx.len >= 3)  ?  t30_frametype(fe->hdlc_rx.buf[2])  :  "???", (fe->rx_data_missing)  ?  "missing octets"  :  "clean");
            hdlc_accept_frame(s, fe->hdlc_rx.buf, fe->hdlc_rx.len, !fe->rx_data_missing);
            hdlc_accept_frame(s, NULL, SIG_STATUS_CARRIER_DOWN, TRUE);
        }
        fe->hdlc_rx.len = 0;
        fe->rx_data_missing = FALSE;
        fe->timeout_rx_samples = 0;
        break;
    case T38_FIELD_HDLC_FCS_BAD_SIG_END:
        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_WARNING, "There is data in a T38_FIELD_HDLC_FCS_BAD_SIG_END!\n");
            /* The sender has incorrectly included data in this message. We can safely ignore it, as the
               bad FCS means we will throw away the whole message, anyway. */
        }
        /* Some T.38 implementations send multiple T38_FIELD_HDLC_FCS_BAD_SIG_END messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "Type %s - CRC bad, sig end (%s)\n", (fe->hdlc_rx.len >= 3)  ?  t30_frametype(fe->hdlc_rx.buf[2])  :  "???", (fe->rx_data_missing)  ?  "missing octets"  :  "clean");
            hdlc_accept_frame(s, fe->hdlc_rx.buf, fe->hdlc_rx.len, FALSE);
            hdlc_accept_frame(s, NULL, SIG_STATUS_CARRIER_DOWN, TRUE);
        }
        fe->hdlc_rx.len = 0;
        fe->rx_data_missing = FALSE;
        fe->timeout_rx_samples = 0;
        break;
    case T38_FIELD_HDLC_SIG_END:
        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_WARNING, "There is data in a T38_FIELD_HDLC_SIG_END!\n");
            /* The sender has incorrectly included data in this message, but there seems nothing meaningful
               it could be. There could not be an FCS good/bad report beyond this. */
        }
        /* Some T.38 implementations send multiple T38_FIELD_HDLC_SIG_END messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            /* WORKAROUND: At least some Mediatrix boxes have a bug, where they can send this message at the
                           end of non-ECM data. We need to tolerate this. We use the generic receive complete
                           indication, rather than the specific HDLC carrier down. */
            /* This message is expected under 2 circumstances. One is as an alternative to T38_FIELD_HDLC_FCS_OK_SIG_END - 
               i.e. they send T38_FIELD_HDLC_FCS_OK, and then T38_FIELD_HDLC_SIG_END when the carrier actually drops.
               The other is because the HDLC signal drops unexpectedly - i.e. not just after a final frame. */
            fe->hdlc_rx.len = 0;
            fe->rx_data_missing = FALSE;
            fe->timeout_rx_samples = 0;
            front_end_status(s, T30_FRONT_END_RECEIVE_COMPLETE);
        }
        break;
    case T38_FIELD_T4_NON_ECM_DATA:
        if (!fe->rx_signal_present)
        {
            t30_non_ecm_put_bit(&s->t30, SIG_STATUS_TRAINING_SUCCEEDED);
            fe->rx_signal_present = TRUE;
        }
        if (len > 0)
        {
            bit_reverse(buf2, buf, len);
            t30_non_ecm_put_chunk(&s->t30, buf2, len);
        }
        fe->timeout_rx_samples = fe->samples + ms_to_samples(MID_RX_TIMEOUT);
        break;
    case T38_FIELD_T4_NON_ECM_SIG_END:
        /* Some T.38 implementations send multiple T38_FIELD_T4_NON_ECM_SIG_END messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            if (len > 0)
            {
                if (!fe->rx_signal_present)
                {
                    t30_non_ecm_put_bit(&s->t30, SIG_STATUS_TRAINING_SUCCEEDED);
                    fe->rx_signal_present = TRUE;
                }
                bit_reverse(buf2, buf, len);
                t30_non_ecm_put_chunk(&s->t30, buf2, len);
            }
            /* WORKAROUND: At least some Mediatrix boxes have a bug, where they can send HDLC signal end where
                           they should send non-ECM signal end. It is possible they also do the opposite.
                           We need to tolerate this, so we use the generic receive complete
                           indication, rather than the specific non-ECM carrier down. */
            front_end_status(s, T30_FRONT_END_RECEIVE_COMPLETE);
        }
        fe->rx_signal_present = FALSE;
        fe->timeout_rx_samples = 0;
        break;
    case T38_FIELD_CM_MESSAGE:
        if (len >= 1)
            span_log(&s->logging, SPAN_LOG_FLOW, "CM profile %d - %s\n", buf[0] - '0', t38_cm_profile_to_str(buf[0]));
        else
            span_log(&s->logging, SPAN_LOG_FLOW, "Bad length for CM message - %d\n", len);
        break;
    case T38_FIELD_JM_MESSAGE:
        if (len >= 2)
            span_log(&s->logging, SPAN_LOG_FLOW, "JM - %s\n", t38_jm_to_str(buf, len));
        else
            span_log(&s->logging, SPAN_LOG_FLOW, "Bad length for JM message - %d\n", len);
        break;
    case T38_FIELD_CI_MESSAGE:
        if (len >= 1)
            span_log(&s->logging, SPAN_LOG_FLOW, "CI 0x%X\n", buf[0]);
        else
            span_log(&s->logging, SPAN_LOG_FLOW, "Bad length for CI message - %d\n", len);
        break;
    case T38_FIELD_V34RATE:
        if (len >= 3)
        {
            fe->t38.v34_rate = t38_v34rate_to_bps(buf, len);
            span_log(&s->logging, SPAN_LOG_FLOW, "V.34 rate %d bps\n", fe->t38.v34_rate);
        }
        else
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "Bad length for V34rate message - %d\n", len);
        }
        break;
    default:
        break;
    }
    return 0;
}
/*- End of function --------------------------------------------------------*/

static void send_hdlc(void *user_data, const uint8_t *msg, int len)
{
    t38_terminal_state_t *s;

    s = (t38_terminal_state_t *) user_data;
    if (len <= 0)
    {
        s->t38_fe.hdlc_tx.len = -1;
    }
    else
    {
        s->t38_fe.hdlc_tx.extra_bits = extra_bits_in_stuffed_frame(msg, len);
        bit_reverse(s->t38_fe.hdlc_tx.buf, msg, len);
        s->t38_fe.hdlc_tx.len = len;
        s->t38_fe.hdlc_tx.ptr = 0;
    }
}
/*- End of function --------------------------------------------------------*/

static __inline__ int bits_to_us(t38_terminal_state_t *s, int bits)
{
    if (s->t38_fe.ms_per_tx_chunk == 0  ||  s->t38_fe.tx_bit_rate == 0)
        return 0;
    return bits*1000000/s->t38_fe.tx_bit_rate;
}
/*- End of function --------------------------------------------------------*/

static void set_octets_per_data_packet(t38_terminal_state_t *s, int bit_rate)
{
    s->t38_fe.tx_bit_rate = bit_rate;
    if (s->t38_fe.ms_per_tx_chunk)
    {
        s->t38_fe.octets_per_data_packet = s->t38_fe.ms_per_tx_chunk*bit_rate/(8*1000);
        /* Make sure we have a positive number (i.e. we didn't truncate to zero). */
        if (s->t38_fe.octets_per_data_packet < 1)
            s->t38_fe.octets_per_data_packet = 1;
    }
    else
    {
        s->t38_fe.octets_per_data_packet = MAX_OCTETS_PER_UNPACED_CHUNK;
    }
}
/*- End of function --------------------------------------------------------*/

static int stream_non_ecm(t38_terminal_state_t *s)
{
    t38_terminal_front_end_state_t *fe;
    uint8_t buf[MAX_OCTETS_PER_UNPACED_CHUNK + 50];
    int delay;
    int len;

    fe = &s->t38_fe;
    for (delay = 0;  delay == 0;  )
    {
        switch (fe->timed_step)
        {
        case T38_TIMED_STEP_NON_ECM_MODEM:
            /* Create a 75ms silence */
            if (fe->t38.current_tx_indicator != T38_IND_NO_SIGNAL)
                delay = t38_core_send_indicator(&fe->t38, T38_IND_NO_SIGNAL, fe->t38.indicator_tx_count);
            else
                delay = 75000;
            fe->timed_step = T38_TIMED_STEP_NON_ECM_MODEM_2;
            fe->next_tx_samples = fe->samples;
            break;
        case T38_TIMED_STEP_NON_ECM_MODEM_2:
            /* Switch on a fast modem, and give the training time to complete */
            delay = t38_core_send_indicator(&fe->t38, fe->next_tx_indicator, fe->t38.indicator_tx_count);
            fe->timed_step = T38_TIMED_STEP_NON_ECM_MODEM_3;
            break;
        case T38_TIMED_STEP_NON_ECM_MODEM_3:
            /* Send a chunk of non-ECM image data */
            /* T.38 says it is OK to send the last of the non-ECM data in the signal end message.
               However, I think the early versions of T.38 said the signal end message should not
               contain data. Hopefully, following the current spec will not cause compatibility
               issues. */
            len = t30_non_ecm_get_chunk(&s->t30, buf, fe->octets_per_data_packet);
            if (len > 0)
                bit_reverse(buf, buf, len);
            if (len < fe->octets_per_data_packet)
            {
                /* That's the end of the image data. */
                if (s->t38_fe.ms_per_tx_chunk)
                {
                    /* Pad the end of the data with some zeros. If we just stop abruptly
                       at the end of the EOLs, some ATAs fail to clean up properly before
                       shutting down their transmit modem, and the last few rows of the image
                       are lost or corrupted. Simply delaying the no-signal message does not
                       help for all implentations. It is usually ignored, which is probably
                       the right thing to do after receiving a message saying the signal has
                       ended. */
                    memset(buf + len, 0, fe->octets_per_data_packet - len);
                    fe->non_ecm_trailer_bytes = 3*fe->octets_per_data_packet + len;
                    len = fe->octets_per_data_packet;
                    fe->timed_step = T38_TIMED_STEP_NON_ECM_MODEM_4;
                }
                else
                {
                    /* If we are sending quickly there seems no point in doing any padding */
                    t38_core_send_data(&fe->t38, fe->current_tx_data_type, T38_FIELD_T4_NON_ECM_SIG_END, buf, len, fe->t38.data_end_tx_count);
                    fe->timed_step = T38_TIMED_STEP_NON_ECM_MODEM_5;
                    delay = 0;
                }
            }
            t38_core_send_data(&fe->t38, fe->current_tx_data_type, T38_FIELD_T4_NON_ECM_DATA, buf, len, fe->t38.data_tx_count);
            delay = bits_to_us(s, 8*len);
            break;
        case T38_TIMED_STEP_NON_ECM_MODEM_4:
            /* Send padding */
            len = fe->octets_per_data_packet;
            fe->non_ecm_trailer_bytes -= fe->octets_per_data_packet;
            if (fe->non_ecm_trailer_bytes <= 0)
            {
                len += fe->non_ecm_trailer_bytes;
                memset(buf, 0, len);
                t38_core_send_data(&fe->t38, fe->current_tx_data_type, T38_FIELD_T4_NON_ECM_SIG_END, buf, len, fe->t38.data_end_tx_count);
                fe->timed_step = T38_TIMED_STEP_NON_ECM_MODEM_5;
                /* Allow a bit more time than the data will take to play out, to ensure the far ATA does not
                   cut things short. */
                delay = bits_to_us(s, 8*len);
                if (s->t38_fe.ms_per_tx_chunk)
                    delay += 60000;
                front_end_status(s, T30_FRONT_END_SEND_STEP_COMPLETE);
                break;
            }
            memset(buf, 0, len);
            t38_core_send_data(&fe->t38, fe->current_tx_data_type, T38_FIELD_T4_NON_ECM_DATA, buf, len, fe->t38.data_tx_count);
            delay = bits_to_us(s, 8*len);
            break;
        case T38_TIMED_STEP_NON_ECM_MODEM_5:
            /* This should not be needed, since the message above indicates the end of the signal, but it
               seems like it can improve compatibility with quirky implementations. */
            delay = t38_core_send_indicator(&fe->t38, T38_IND_NO_SIGNAL, fe->t38.indicator_tx_count);
            fe->timed_step = T38_TIMED_STEP_NONE;
            return delay;
        }
    }
    return delay;
}
/*- End of function --------------------------------------------------------*/

static int stream_hdlc(t38_terminal_state_t *s)
{
    t38_terminal_front_end_state_t *fe;
    uint8_t buf[MAX_OCTETS_PER_UNPACED_CHUNK + 50];
    t38_data_field_t data_fields[2];
    int previous;
    int delay;
    int i;

    fe = &s->t38_fe;
    for (delay = 0;  delay == 0;  )
    {
        switch (fe->timed_step)
        {
        case T38_TIMED_STEP_HDLC_MODEM:
            /* Create a 75ms silence */
            if (fe->t38.current_tx_indicator != T38_IND_NO_SIGNAL)
                delay = t38_core_send_indicator(&fe->t38, T38_IND_NO_SIGNAL, fe->t38.indicator_tx_count);
            else
                delay = 75000;
            fe->timed_step = T38_TIMED_STEP_HDLC_MODEM_2;
            fe->next_tx_samples = fe->samples;
            break;
        case T38_TIMED_STEP_HDLC_MODEM_2:
            /* Send HDLC preambling */
            delay = t38_core_send_indicator(&fe->t38, fe->next_tx_indicator, fe->t38.indicator_tx_count);
            delay += t38_core_send_flags_delay(&fe->t38, fe->next_tx_indicator);
            fe->timed_step = T38_TIMED_STEP_HDLC_MODEM_3;
            break;
        case T38_TIMED_STEP_HDLC_MODEM_3:
            /* Send a chunk of HDLC data */
            i = fe->hdlc_tx.len - fe->hdlc_tx.ptr;
            if (fe->octets_per_data_packet >= i)
            {
                /* The last part of an HDLC frame */
                if (fe->chunking_modes & T38_CHUNKING_MERGE_FCS_WITH_DATA)
                {
                    /* Copy the data, as we might be about to refill the buffer it is in */
                    memcpy(buf, &fe->hdlc_tx.buf[fe->hdlc_tx.ptr], i);
                    data_fields[0].field_type = T38_FIELD_HDLC_DATA;
                    data_fields[0].field = buf;
                    data_fields[0].field_len = i;

                    /* Now see about the next HDLC frame. This will tell us whether to send FCS_OK or FCS_OK_SIG_END */
                    previous = fe->current_tx_data_type;
                    fe->hdlc_tx.ptr = 0;
                    fe->hdlc_tx.len = 0;
                    front_end_status(s, T30_FRONT_END_SEND_STEP_COMPLETE);
                    /* The above step should have got the next HDLC step ready - either another frame, or an instruction to stop transmission. */
                    if (fe->hdlc_tx.len < 0)
                    {
                        data_fields[1].field_type = T38_FIELD_HDLC_FCS_OK_SIG_END;
                        data_fields[1].field = NULL;
                        data_fields[1].field_len = 0;
                        t38_core_send_data_multi_field(&fe->t38, fe->current_tx_data_type, data_fields, 2, fe->t38.data_tx_count);
                        fe->timed_step = T38_TIMED_STEP_HDLC_MODEM_5;
                        /* We add a bit of extra time here, as with some implementations
                           the carrier falling too abruptly causes data loss. */
                        delay = bits_to_us(s, i*8 + fe->hdlc_tx.extra_bits);
                        if (s->t38_fe.ms_per_tx_chunk)
                            delay += 100000;
                        front_end_status(s, T30_FRONT_END_SEND_STEP_COMPLETE);
                    }
                    else
                    {
                        data_fields[1].field_type = T38_FIELD_HDLC_FCS_OK;
                        data_fields[1].field = NULL;
                        data_fields[1].field_len = 0;
                        t38_core_send_data_multi_field(&fe->t38, fe->current_tx_data_type, data_fields, 2, fe->t38.data_tx_count);
                        fe->timed_step = T38_TIMED_STEP_HDLC_MODEM_3;
                        delay = bits_to_us(s, i*8 + fe->hdlc_tx.extra_bits);
                    }
                }
                else
                {
                    t38_core_send_data(&fe->t38, fe->current_tx_data_type, T38_FIELD_HDLC_DATA, &fe->hdlc_tx.buf[fe->hdlc_tx.ptr], i, fe->t38.data_tx_count);
                    fe->timed_step = T38_TIMED_STEP_HDLC_MODEM_4;
                    delay = bits_to_us(s, i*8);
                }
                break;
            }
            t38_core_send_data(&fe->t38, fe->current_tx_data_type, T38_FIELD_HDLC_DATA, &fe->hdlc_tx.buf[fe->hdlc_tx.ptr], fe->octets_per_data_packet, fe->t38.data_tx_count);
            fe->hdlc_tx.ptr += fe->octets_per_data_packet;
            delay = bits_to_us(s, fe->octets_per_data_packet*8);
            break;
        case T38_TIMED_STEP_HDLC_MODEM_4:
            /* End of HDLC frame */
            previous = fe->current_tx_data_type;
            fe->hdlc_tx.ptr = 0;
            fe->hdlc_tx.len = 0;
            front_end_status(s, T30_FRONT_END_SEND_STEP_COMPLETE);
            /* The above step should have got the next HDLC step ready - either another frame, or an instruction to stop transmission. */
            if (fe->hdlc_tx.len < 0)
            {
                /* End of transmission */
                t38_core_send_data(&fe->t38, previous, T38_FIELD_HDLC_FCS_OK_SIG_END, NULL, 0, fe->t38.data_end_tx_count);
                fe->timed_step = T38_TIMED_STEP_HDLC_MODEM_5;
                /* We add a bit of extra time here, as with some implementations
                   the carrier falling too abruptly causes data loss. */
                delay = bits_to_us(s, fe->hdlc_tx.extra_bits);
                if (s->t38_fe.ms_per_tx_chunk)
                    delay += 100000;
                front_end_status(s, T30_FRONT_END_SEND_STEP_COMPLETE);
                break;
            }
            if (fe->hdlc_tx.len == 0)
            {
                /* Now, how did we get here? We have finished a frame, but have no new frame to
                   send, and no end of transmission condition. */
                span_log(&s->logging, SPAN_LOG_FLOW, "No new frame or end transmission condition.\n");
            }
            /* Finish the current frame off, and prepare for the next one. */
            t38_core_send_data(&fe->t38, previous, T38_FIELD_HDLC_FCS_OK, NULL, 0, fe->t38.data_tx_count);
            fe->timed_step = T38_TIMED_STEP_HDLC_MODEM_3;
            /* We should now wait enough time for everything to clear through an analogue modem at the far end. */
            delay = bits_to_us(s, fe->hdlc_tx.extra_bits);
            break;
        case T38_TIMED_STEP_HDLC_MODEM_5:
            /* Note that some boxes do not like us sending a T38_FIELD_HDLC_SIG_END at this point.
               A T38_IND_NO_SIGNAL should always be OK. */
            delay = t38_core_send_indicator(&fe->t38, T38_IND_NO_SIGNAL, fe->t38.indicator_tx_count);
            fe->timed_step = T38_TIMED_STEP_NONE;
            return delay;
        }
    }
    return delay;
}
/*- End of function --------------------------------------------------------*/

static int stream_ced(t38_terminal_state_t *s)
{
    t38_terminal_front_end_state_t *fe;
    int delay;

    fe = &s->t38_fe;
    for (delay = 0;  delay == 0;  )
    {
        switch (fe->timed_step)
        {
        case T38_TIMED_STEP_CED:
            /* It seems common practice to start with a no signal indicator, though
               this is not a specified requirement. Since we should be sending 200ms
               of silence, starting the delay with a no signal indication makes sense.
               We do need a 200ms delay, as that is a specification requirement. */
            fe->timed_step = T38_TIMED_STEP_CED_2;
            delay = t38_core_send_indicator(&fe->t38, T38_IND_NO_SIGNAL, fe->t38.indicator_tx_count);
            delay = 200000;
            fe->next_tx_samples = fe->samples;
            break;
        case T38_TIMED_STEP_CED_2:
            /* Initial 200ms delay over. Send the CED indicator */
            fe->timed_step = T38_TIMED_STEP_CED_3;
            delay = t38_core_send_indicator(&fe->t38, T38_IND_CED, fe->t38.indicator_tx_count);
            fe->current_tx_data_type = T38_DATA_NONE;
            break;
        case T38_TIMED_STEP_CED_3:
            /* End of CED */
            fe->timed_step = T38_TIMED_STEP_NONE;
            front_end_status(s, T30_FRONT_END_SEND_STEP_COMPLETE);
            return 0;
        }
    }
    return delay;
}
/*- End of function --------------------------------------------------------*/

static int stream_cng(t38_terminal_state_t *s)
{
    t38_terminal_front_end_state_t *fe;
    int delay;

    fe = &s->t38_fe;
    for (delay = 0;  delay == 0;  )
    {
        switch (fe->timed_step)
        {
        case T38_TIMED_STEP_CNG:
            /* It seems common practice to start with a no signal indicator, though
               this is not a specified requirement of the T.38 spec. Since we should
               be sending 200ms of silence, according to T.30, starting that delay with
               a no signal indication makes sense. */
            fe->timed_step = T38_TIMED_STEP_CNG_2;
            delay = t38_core_send_indicator(&fe->t38, T38_IND_NO_SIGNAL, fe->t38.indicator_tx_count);
            delay = 200000;
            fe->next_tx_samples = fe->samples;
            break;
        case T38_TIMED_STEP_CNG_2:
            /* Initial short delay over. Send the CNG indicator. CNG persists until something
               coming the other way interrupts it, or a long timeout controlled by the T.30 engine
               expires. */
            fe->timed_step = T38_TIMED_STEP_NONE;
            delay = t38_core_send_indicator(&fe->t38, T38_IND_CNG, fe->t38.indicator_tx_count);
            fe->current_tx_data_type = T38_DATA_NONE;
            return delay;
        }
    }
    return delay;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(int) t38_terminal_send_timeout(t38_terminal_state_t *s, int samples)
{
    t38_terminal_front_end_state_t *fe;
    int delay;

    fe = &s->t38_fe;
    if (fe->current_rx_type == T30_MODEM_DONE  ||  fe->current_tx_type == T30_MODEM_DONE)
        return TRUE;

    fe->samples += samples;
    t30_timer_update(&s->t30, samples);
    if (fe->timeout_rx_samples  &&  fe->samples > fe->timeout_rx_samples)
    {
        span_log(&s->logging, SPAN_LOG_FLOW, "Timeout mid-receive\n");
        fe->timeout_rx_samples = 0;
        front_end_status(s, T30_FRONT_END_RECEIVE_COMPLETE);
    }
    if (fe->timed_step == T38_TIMED_STEP_NONE)
        return FALSE;
    /* Wait until the right time comes along, unless we are working in "no delays" mode, while talking to an
       IAF terminal. */
    if (fe->ms_per_tx_chunk  &&  fe->samples < fe->next_tx_samples)
        return FALSE;
    /* Its time to send something */
    delay = 0;
    switch (fe->timed_step & 0xFFF0)
    {
    case T38_TIMED_STEP_NON_ECM_MODEM:
        delay = stream_non_ecm(s);
        break;
    case T38_TIMED_STEP_HDLC_MODEM:
        delay = stream_hdlc(s);
        break;
    case T38_TIMED_STEP_CED:
        delay = stream_ced(s);
        break;
    case T38_TIMED_STEP_CNG:
        delay = stream_cng(s);
        break;
    case T38_TIMED_STEP_PAUSE:
        /* End of timed pause */
        fe->timed_step = T38_TIMED_STEP_NONE;
        front_end_status(s, T30_FRONT_END_SEND_STEP_COMPLETE);
        break;
    }
    fe->next_tx_samples += us_to_samples(delay);
    return FALSE;
}
/*- End of function --------------------------------------------------------*/

static void set_rx_type(void *user_data, int type, int bit_rate, int short_train, int use_hdlc)
{
    t38_terminal_state_t *s;

    s = (t38_terminal_state_t *) user_data;
    span_log(&s->logging, SPAN_LOG_FLOW, "Set rx type %d\n", type);
    s->t38_fe.current_rx_type = type;
}
/*- End of function --------------------------------------------------------*/

static void start_tx(t38_terminal_front_end_state_t *fe, int use_hdlc)
{
    /* The actual transmission process depends on whether we are sending at a paced manner,
       for interaction with a traditional FAX machine, or streaming as fast as we can, normally
       over a TCP connection to a machine directly connected to the internet. */
    if (fe->ms_per_tx_chunk)
    {
        /* Start the paced packet transmission process. */
        fe->timed_step = (use_hdlc)  ?  T38_TIMED_STEP_HDLC_MODEM  :  T38_TIMED_STEP_NON_ECM_MODEM;
        if (fe->next_tx_samples < fe->samples)
            fe->next_tx_samples = fe->samples;
    }
    else
    {
        /* Start the fast streaming transmission process. */
    }
}
/*- End of function --------------------------------------------------------*/

static void set_tx_type(void *user_data, int type, int bit_rate, int short_train, int use_hdlc)
{
    t38_terminal_state_t *s;
    t38_terminal_front_end_state_t *fe;

    s = (t38_terminal_state_t *) user_data;
    fe = &s->t38_fe;
    span_log(&s->logging, SPAN_LOG_FLOW, "Set tx type %d\n", type);
    if (fe->current_tx_type == type)
        return;

    set_octets_per_data_packet(s, bit_rate);
    switch (type)
    {
    case T30_MODEM_NONE:
        /* If a "no signal" indicator is waiting to be played out, don't disturb it. */
        if (fe->timed_step != T38_TIMED_STEP_NON_ECM_MODEM_5  &&  fe->timed_step != T38_TIMED_STEP_HDLC_MODEM_5)
            fe->timed_step = T38_TIMED_STEP_NONE;
        fe->current_tx_data_type = T38_DATA_NONE;
        break;
    case T30_MODEM_PAUSE:
        fe->next_tx_samples = fe->samples + ms_to_samples(short_train);
        fe->timed_step = T38_TIMED_STEP_PAUSE;
        fe->current_tx_data_type = T38_DATA_NONE;
        break;
    case T30_MODEM_CED:
        fe->next_tx_samples = fe->samples;
        fe->timed_step = T38_TIMED_STEP_CED;
        fe->current_tx_data_type = T38_DATA_NONE;
        break;
    case T30_MODEM_CNG:
        fe->next_tx_samples = fe->samples;
        fe->timed_step = T38_TIMED_STEP_CNG;
        fe->current_tx_data_type = T38_DATA_NONE;
        break;
    case T30_MODEM_V21:
        fe->next_tx_indicator = T38_IND_V21_PREAMBLE;
        fe->current_tx_data_type = T38_DATA_V21;
        start_tx(fe, use_hdlc);
        break;
    case T30_MODEM_V27TER:
        switch (bit_rate)
        {
        case 2400:
            fe->next_tx_indicator = T38_IND_V27TER_2400_TRAINING;
            fe->current_tx_data_type = T38_DATA_V27TER_2400;
            break;
        case 4800:
            fe->next_tx_indicator = T38_IND_V27TER_4800_TRAINING;
            fe->current_tx_data_type = T38_DATA_V27TER_4800;
            break;
        }
        start_tx(fe, use_hdlc);
        break;
    case T30_MODEM_V29:
        switch (bit_rate)
        {
        case 7200:
            fe->next_tx_indicator = T38_IND_V29_7200_TRAINING;
            fe->current_tx_data_type = T38_DATA_V29_7200;
            break;
        case 9600:
            fe->next_tx_indicator = T38_IND_V29_9600_TRAINING;
            fe->current_tx_data_type = T38_DATA_V29_9600;
            break;
        }
        start_tx(fe, use_hdlc);
        break;
    case T30_MODEM_V17:
        switch (bit_rate)
        {
        case 7200:
            fe->next_tx_indicator = (short_train)  ?  T38_IND_V17_7200_SHORT_TRAINING  :  T38_IND_V17_7200_LONG_TRAINING;
            fe->current_tx_data_type = T38_DATA_V17_7200;
            break;
        case 9600:
            fe->next_tx_indicator = (short_train)  ?  T38_IND_V17_9600_SHORT_TRAINING  :  T38_IND_V17_9600_LONG_TRAINING;
            fe->current_tx_data_type = T38_DATA_V17_9600;
            break;
        case 12000:
            fe->next_tx_indicator = (short_train)  ?  T38_IND_V17_12000_SHORT_TRAINING  :  T38_IND_V17_12000_LONG_TRAINING;
            fe->current_tx_data_type = T38_DATA_V17_12000;
            break;
        case 14400:
            fe->next_tx_indicator = (short_train)  ?  T38_IND_V17_14400_SHORT_TRAINING  :  T38_IND_V17_14400_LONG_TRAINING;
            fe->current_tx_data_type = T38_DATA_V17_14400;
            break;
        }
        start_tx(fe, use_hdlc);
        break;
    case T30_MODEM_DONE:
        span_log(&s->logging, SPAN_LOG_FLOW, "FAX exchange complete\n");
        fe->timed_step = T38_TIMED_STEP_NONE;
        fe->current_tx_data_type = T38_DATA_NONE;
        break;
    }
    fe->current_tx_type = type;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(void) t38_terminal_set_config(t38_terminal_state_t *s, int without_pacing)
{
    if (without_pacing)
    {
        /* Continuous streaming mode, as used for TPKT over TCP transport */
        /* Inhibit indicator packets */
        s->t38_fe.t38.indicator_tx_count = 0;
        s->t38_fe.t38.data_tx_count = 1;
        s->t38_fe.t38.data_end_tx_count = 1;
        s->t38_fe.ms_per_tx_chunk = 0;
    }
    else
    {
        /* Paced streaming mode, as used for UDP transports */
        s->t38_fe.t38.indicator_tx_count = INDICATOR_TX_COUNT;
        s->t38_fe.t38.data_tx_count = DATA_TX_COUNT;
        s->t38_fe.t38.data_end_tx_count = DATA_END_TX_COUNT;
        s->t38_fe.ms_per_tx_chunk = MS_PER_TX_CHUNK;
    }
    set_octets_per_data_packet(s, 300);
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(void) t38_terminal_set_tep_mode(t38_terminal_state_t *s, int use_tep)
{
    if (use_tep)
        s->t38_fe.chunking_modes |= T38_CHUNKING_ALLOW_TEP_TIME;
    else
        s->t38_fe.chunking_modes &= ~T38_CHUNKING_ALLOW_TEP_TIME;
    t38_set_tep_handling(&s->t38_fe.t38, use_tep);
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(void) t38_terminal_set_fill_bit_removal(t38_terminal_state_t *s, int remove)
{
    if (remove)
        s->t38_fe.iaf |= T30_IAF_MODE_NO_FILL_BITS;
    else
        s->t38_fe.iaf &= ~T30_IAF_MODE_NO_FILL_BITS;
    t30_set_iaf_mode(&s->t30, s->t38_fe.iaf);
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(t30_state_t *) t38_terminal_get_t30_state(t38_terminal_state_t *s)
{
    return &s->t30;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(t38_core_state_t *) t38_terminal_get_t38_core_state(t38_terminal_state_t *s)
{
    return &s->t38_fe.t38;
}
/*- End of function --------------------------------------------------------*/

static int t38_terminal_t38_fe_init(t38_terminal_state_t *t,
                                    t38_tx_packet_handler_t *tx_packet_handler,
                                    void *tx_packet_user_data)
{
    t38_terminal_front_end_state_t *s;
    
    s = &t->t38_fe;
    t38_core_init(&s->t38,
                  process_rx_indicator,
                  process_rx_data,
                  process_rx_missing,
                  (void *) t,
                  tx_packet_handler,
                  tx_packet_user_data);
    s->t38.fastest_image_data_rate = 14400;

    s->rx_signal_present = FALSE;
    s->timed_step = T38_TIMED_STEP_NONE;
    //s->iaf = T30_IAF_MODE_T37 | T30_IAF_MODE_T38;
    s->iaf = T30_IAF_MODE_T38;

    s->current_tx_data_type = T38_DATA_NONE;
    s->next_tx_samples = 0;
    s->chunking_modes = T38_CHUNKING_ALLOW_TEP_TIME;

    s->hdlc_tx.ptr = 0;

    return 0;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(logging_state_t *) t38_terminal_get_logging_state(t38_terminal_state_t *s)
{
    return &s->logging;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(t38_terminal_state_t *) t38_terminal_init(t38_terminal_state_t *s,
                                                       int calling_party,
                                                       t38_tx_packet_handler_t *tx_packet_handler,
                                                       void *tx_packet_user_data)
{
    if (tx_packet_handler == NULL)
        return NULL;

    if (s == NULL)
    {
        if ((s = (t38_terminal_state_t *) malloc(sizeof(*s))) == NULL)
            return NULL;
    }
    memset(s, 0, sizeof(*s));
    span_log_init(&s->logging, SPAN_LOG_NONE, NULL);
    span_log_set_protocol(&s->logging, "T.38T");

    t38_terminal_t38_fe_init(s, tx_packet_handler, tx_packet_user_data);

    t38_terminal_set_config(s, FALSE);

    t30_init(&s->t30,
             calling_party,
             set_rx_type,
             (void *) s,
             set_tx_type,
             (void *) s,
             send_hdlc,
             (void *) s);
    t30_set_iaf_mode(&s->t30, s->t38_fe.iaf);
    t30_set_supported_modems(&s->t30,
                             T30_SUPPORT_V27TER | T30_SUPPORT_V29 | T30_SUPPORT_V17 | T30_SUPPORT_IAF);
    t30_restart(&s->t30);
    return s;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(int) t38_terminal_release(t38_terminal_state_t *s)
{
    t30_release(&s->t30);
    return 0;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(int) t38_terminal_free(t38_terminal_state_t *s)
{
    t38_terminal_release(s);
    free(s);
    return 0;
}
/*- End of function --------------------------------------------------------*/

#endif // HAVE_TIFF_H
/*- End of file ------------------------------------------------------------*/
