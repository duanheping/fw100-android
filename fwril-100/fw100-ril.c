/* 
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <alloca.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <telephony/ril.h>

#include <atchannel.h>
#include <at_tok.h>
#include <misc.h>

#define LOG_TAG "RIL"
#include <utils/Log.h>

#include <fw100-ril.h>  
#include <rilinfo.h>

#define LOG_TAG "RIL"
#include <utils/Log.h>

#include <rilinfo.h>

#ifdef RILTEST_LOG
#include <logger.h>
#endif

#define MAX_AT_RESPONSE 0x1000

typedef enum {
    SIM_ABSENT = 0,
    SIM_NOT_READY = 1,
    SIM_READY = 2, /* SIM_READY means the radio state is RADIO_STATE_SIM_READY */
    SIM_PIN = 3,
    SIM_PUK = 4,
    SIM_NETWORK_PERSONALIZATION = 5
} SIM_Status; 

static void onRequest (int request, void *data, size_t datalen, RIL_Token t);
static RIL_RadioState currentState();
static int onSupports (int requestCode);
static void onCancel (RIL_Token t);
static const char *getVersion();
static void setRadioState(RIL_RadioState newState);
static SIM_Status getSIMStatus();
static int getCardStatus(RIL_CardStatus **pp_card_status);
static void freeCardStatus(RIL_CardStatus *p_card_status);
static void pollSIMState (void *param);

/*** Static Variables ***/
static const RIL_RadioFunctions myRILDriverCallbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion
};

#include <fw100-ril.h>

// friend
const struct RIL_Env *s_rilenv;
static fw100SessionCtx_t fw100Ctx;

/**
 * \brief file name isolator
 *  scan file path and return pointer to filename
 *
 * \param
 *  char *filepath
 *
 * \return
 *  pointer to filename
 *
 */
const char *trim_path(const char *filepath)
{

        int len = strlen(filepath);
        const char *p = &(filepath[len]);
        do
        {
                if (*p == '/') return p+1;
                if (*p == '\\') return p+1;
                --p;

        } while (--len);
        return p;
}

/** 
 * \brief do post-AT+CFUN=1 initialization 
 */
static void onRadioPowerOn()
{
    RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED, NULL, 0);
    //pollSIMState(NULL);
}

/** do post- SIM ready initialization */
static void onSIMReady()
{
    /*
     * Always send SMS messages directly to the TE
     *
     * mode = 1 // discard when link is reserved (link should never be
     *             reserved)
     * mt = 2   // most messages routed to TE
     * bm = 2   // new cell BM's routed to TE
     * ds = 1   // Status reports routed to TE
     * bfr = 1  // flush buffer
     */
    at_send_command("AT+CNMI=1,2,2,1,1", NULL);
}

static void requestRadioPower(void *data, size_t datalen, RIL_Token t)
{
    int err;
    int onOff;
    ATResponse *p_response = NULL;

    assert (datalen >= sizeof(int *));
    onOff = ((int *)data)[0];

    if (onOff == 0 && fw100Ctx.sState != RADIO_STATE_OFF) {
        err = at_send_command("AT+CPOF", &p_response);
       if (err < 0 || p_response->success == 0) goto error;
        setRadioState(RADIO_STATE_OFF);
    } else if (onOff > 0 && fw100Ctx.sState == RADIO_STATE_OFF) {
        err = at_send_command("AT+CPON", &p_response);
        if (err < 0|| p_response->success == 0) {
            // Some stacks return an error when there is no SIM,
            // but they really turn the RF portion on
            // So, if we get an error, let's check to see if it
            // turned on anyway

            if (isRadioOn() != 1) {
                goto error;
            }
        }
        setRadioState(RADIO_STATE_SIM_NOT_READY);
    }

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	
}

/**
 * \brief query GSM/UMTS network operator
 * 
 * FW100 seems stuck in manual mode 
 * or maybe just an artifact that there is only one mode?
 * at+cops=1
 * OK
 * at+cops?
 * +COPS:1
 * OK
 * at+cops=0
 * OK
 * at+cops?
 * +COPS:1
 *
 * 
 * enfora:
 * at+cops?
 * +COPS: 0,2,"310260"
 * at+cops=0
 * OK
 * +CGREG: 2
 * +CREG: 2
 * +CREG: 1, 0080, D23E
 * 
 *
 * "response" is int *
 * ((const int *)response)[0] is
 *     0 for automatic selection
 *     1 for manual selection
 *
 *
 */
static void requestQueryNetworkSelectionMode(
                void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    if (err < 0) {
        goto error;
    }

    // stateful override network select mode
    response = fw100Ctx.networkSelectMode;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    if (NULL != p_response) at_response_free(p_response);
    return;
error:
    if (NULL != p_response) at_response_free(p_response);
    LOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void sendCallStateChanged(void *param)
{
    LOGD ("%s:%d bypassed\n", __FUNCTION__, __LINE__);
    #if 0
    RIL_onUnsolicitedResponse (
        RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
        NULL, 0);
    #endif
}

/**
 * \brief not supported for data-only applications
 */
static void requestDial(void *data, size_t datalen, RIL_Token t)
{
    RIL_Dial *p_dial;
    char *cmd;
    const char *clir;
    int ret;

    p_dial = (RIL_Dial *)data;

    switch (p_dial->clir) {
        case 1: clir = "I"; break;  /*invocation*/
        case 2: clir = "i"; break;  /*suppression*/
        default:
        case 0: clir = ""; break;   /*subscription default*/
    }

    asprintf(&cmd, "ATD%s%s;", p_dial->address, clir);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestWriteSmsToSim(void *data, size_t datalen, RIL_Token t)
{
    RIL_SMS_WriteArgs *p_args;
    char *cmd;
    int length;
    int err;
    ATResponse *p_response = NULL;

    p_args = (RIL_SMS_WriteArgs *)data;

    length = strlen(p_args->pdu)/2;
    asprintf(&cmd, "AT+CMGW=%d,%d", length, p_args->status);

    err = at_send_command_sms(cmd, p_args->pdu, "+CMGW:", &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/**
 * \brief not supported for data-only applications
 */
static void requestHangup(void *data, size_t datalen, RIL_Token t)
{
    int *p_line;

    int ret;
    char *cmd;

    p_line = (int *)data;

    // 3GPP 22.030 6.5.5
    // "Releases a specific active call X"
    asprintf(&cmd, "AT+CHLD=1%d", p_line[0]);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

/**
 * \brief simple poll signal strength
 * 
 * response is defined by the following structure whose
 * binary fields comprise an array of 7 integers.
 * since this is only used in local UNIX domain, 
 * structure pack/pad and byte order are native
 * as generated by compiler.
 * 
 * GW:   0,1
 * CDMA: 2,3
 * EVDO  4,5,6
 *
 * typedef struct {
 * RIL_GW_SignalStrength   GW_SignalStrength;
 * RIL_CDMA_SignalStrength CDMA_SignalStrength;
 * RIL_EVDO_SignalStrength EVDO_SignalStrength;
 * } RIL_SignalStrength;
 * 
 * n.b. see fw100-ril-rqst.c requestSignalStrengthEVDO 
 * for EVDO aware version of this function.
 */
static void requestSignalStrength(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;
    int response[7];
    char *line;

    memset(response, 0, sizeof(response));

    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[0]));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[1]));
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));

    at_response_free(p_response);
    return;

error:
    LOGE("requestSignalStrength must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/** 
 * \brief GSM oriented basic version
 * 
 * fw100
 * at+CREG?
 * +CREG:1,4145,7,1
 *                ^
 * registration status 1 is fourth code in response
 *
 * enfora
 * at+CREG?
 * +CREG: 2,1,0080,D23E
 *          ^
 * registration status 1 is second code in response
 *
 * N.B. alternate see fw100-ril-rqst.c requestRegistrationStateEVDO()
 *
 * see ril.h for response format
 * essentially these requests do the same thing, but
 * RQST_REGISTRATION_STATE is superset of GPRS with 15 response fields
 * vs. 4 for GPRS
 */
static void requestRegistrationState(int request, void *data,
            size_t datalen, RIL_Token t)
{
    int err;
    int response[4];
    char *responseStr[15];
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count;
    int response_len;

    // for fw100 use CREG
    if (request == RIL_REQUEST_REGISTRATION_STATE) {
        cmd = "AT+CREG?";
        prefix = "+CREG:";
        response_len = 15;
    } else if (request == RIL_REQUEST_GPRS_REGISTRATION_STATE) {
        cmd = "AT+CREG?";
        prefix = "+CREG:";
        response_len = 4;
    } else {
        assert(0);
        goto error;
    }

    err = at_send_command_singleline(cmd, prefix, &p_response);

    if (err != 0) goto error;

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    /* Ok you have to be careful here
     * The solicited version of the CREG response is
     * +CREG: n, stat, [lac, cid]
     * and the unsolicited version is
     * +CREG: stat, [lac, cid]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both
     *
     * Also since the LAC and CID are only reported when registered,
     * we can have 1, 2, 3, or 4 arguments here
     *
     * finally, a +CGREG: answer may have a fifth value that corresponds
     * to the network type, as in;
     *
     *   +CGREG: n, stat [,lac, cid [,networkType]]
     */

    /* count number of commas */
    commas = 0;
    for (p = line ; *p != '\0' ;p++) {
        if (*p == ',') commas++;
    }

    switch (commas) {
        case 0: /* +CREG: <stat> */
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            response[1] = -1;
            response[2] = -1;
            break;

        case 1: /* +CREG: <n>, <stat> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            response[1] = -1;
            response[2] = -1;
            if (err < 0) goto error;
            break;

        case 2: /* +CREG: <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
            break;

        /* GSM +CREG: <n>, <stat>, <lac>, <cid> */
        /* for fw100 CREG map lac=sid, cid=nid */
        case 3: 
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[0]);
            if (err < 0) goto error;
            count = 4;
            break;

        default:
            goto error;
    }

    memset(responseStr, 0, sizeof(responseStr));
    asprintf(&responseStr[0], "%d", response[0]);
    asprintf(&responseStr[1], "%x", response[1]);
    asprintf(&responseStr[2], "%x", response[2]);

    // assign radio technology
    //asprintf(&responseStr[3], "%d", 2); // EDGE
    asprintf(&responseStr[3], "%d", 8); // EVDO Rev A

    //RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, response_len*sizeof(char*));
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, 4*sizeof(char*));
    at_response_free(p_response);

    return;
error:
    LOGE("requestRegistrationState must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSMSAcknowledge(void *data, size_t datalen, RIL_Token t)
{
    int ackSuccess;
    int err;

    ackSuccess = ((int *)data)[0];

    if (ackSuccess == 1) {
        err = at_send_command("AT+CNMA=1", NULL);
    } else if (ackSuccess == 0)  {
        err = at_send_command("AT+CNMA=2", NULL);
    } else {
        LOGE("unsupported arg to RIL_REQUEST_SMS_ACKNOWLEDGE\n");
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

static void  requestSIM_IO(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;
    int err;
    char *cmd = NULL;
    RIL_SIM_IO *p_args;
    char *line;

    memset(&sr, 0, sizeof(sr));

    p_args = (RIL_SIM_IO *)data;

    if (p_args->data == NULL) {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3);
    } else {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%s",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3, p_args->data);
    }

    err = at_send_command_singleline(cmd, "+CRSM:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr.sw1));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr.sw2));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(sr.simResponse));
        if (err < 0) goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    free(cmd);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);

}

static void  requestEnterSimPin(void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;;

    if ( datalen == sizeof(char*) ) {
        asprintf(&cmd, "AT+CPIN=%s", strings[0]);
    } else if ( datalen == 2*sizeof(char*) ) {
        asprintf(&cmd, "AT+CPIN=%s,%s", strings[0], strings[1]);
    } else
        goto error;

    err = at_send_command_singleline(cmd, "+CPIN:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
error:
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}


static void  requestSendUSSD(void *data, size_t datalen, RIL_Token t)
{
    const char *ussdRequest;

    ussdRequest = (char *)(data);


    RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);

// @@@ TODO

}

/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST
 *
 * Must be completed with a call to RIL_onRequestComplete()
 *
 * RIL_onRequestComplete() may be called from any thread, before or after
 * this function returns.
 *
 * Will always be called from the same thread, so returning here implies
 * that the radio is ready to process another command (whether or not
 * the previous command has completed).
 */
static void
onRequest (int request, void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response;
    int err;

    char tmp[164];
    sprintf(tmp, "state=%d token=%p req=%d %s\n", fw100Ctx.sState, t,
      request, requestToString(request));
    LOGD("onRequest: %s", tmp);

#if 0
    // filter request not supported in this profile
    if (!onSupports (request))
    {
        LOGW("request not supported");
	RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return;
    }
#endif

    /* 
     * Ignore most non-power requests when RADIO_STATE_OFF
     */
    if (fw100Ctx.sState == RADIO_STATE_OFF
        && !((request == RIL_REQUEST_RADIO_POWER)
            || (request == RIL_REQUEST_GET_SIM_STATUS)
            || (request == RIL_REQUEST_SCREEN_STATE))
    ) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    switch (request) {
        case RIL_REQUEST_GET_SIM_STATUS: {
            RIL_CardStatus *p_card_status;
            char *p_buffer;
            int buffer_size;

            int result = getCardStatus(&p_card_status);
            if (result == RIL_E_SUCCESS) {
                p_buffer = (char *)p_card_status;
                buffer_size = sizeof(*p_card_status);
            } else {
                p_buffer = NULL;
                buffer_size = 0;
            }
            RIL_onRequestComplete(t, result, p_buffer, buffer_size);
            freeCardStatus(p_card_status);
            break;
        }
        case RIL_REQUEST_GET_CURRENT_CALLS:
            requestGetCurrentCallsEVDO(data, datalen, t);
            break;

        case RIL_REQUEST_SCREEN_STATE:
            requestScreenState(data, datalen, t);
            break;

        case RIL_REQUEST_DIAL:
            requestDial(data, datalen, t);
            break;

        case RIL_REQUEST_HANGUP:
            requestHangup(data, datalen, t);
            break;

        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
            // 3GPP 22.030 6.5.5
            // "Releases all held calls or sets User Determined User Busy
            //  (UDUB) for a waiting call."
            at_send_command("AT+CHLD=0", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
            // 3GPP 22.030 6.5.5
            // "Releases all active calls (if any exist) and accepts
            //  the other (held or waiting) call."
            at_send_command("AT+CHLD=1", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
            // 3GPP 22.030 6.5.5
            // "Places all active calls (if any exist) on hold and accepts
            //  the other (held or waiting) call."
            at_send_command("AT+CHLD=2", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_ANSWER:
            at_send_command("ATA", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_CONFERENCE:
            // 3GPP 22.030 6.5.5
            // "Adds a held call to the conversation"
            at_send_command("AT+CHLD=3", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_UDUB:
            /* user determined user busy */
            /* sometimes used: ATH */
            at_send_command("ATH", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        case RIL_REQUEST_SEPARATE_CONNECTION:
            {
                char  cmd[12];
                int   party = ((int*)data)[0];

                // Make sure that party is in a valid range.
                // (Note: The Telephony middle layer imposes a range of 1 to 7.
                // It's sufficient for us to just make sure it's single digit.)
                if (party > 0 && party < 10) {
                    sprintf(cmd, "AT+CHLD=2%d", party);
                    at_send_command(cmd, NULL);
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            }
            break;

        // requestSignalStrength makes basic RSSI
        // requestSignalStrengthEVDO is EVDO aware version.
        case RIL_REQUEST_SIGNAL_STRENGTH:
            requestSignalStrength(data, datalen, t);
            break;

        case RIL_REQUEST_REGISTRATION_STATE:
            requestRegistrationStateEVDO(request, data, datalen, t);
            break;

        case RIL_REQUEST_GPRS_REGISTRATION_STATE:
            //requestRegistrationState(request, data, datalen, t);
            requestRegistrationStateEVDO(request, data, datalen, t);
            break;

        case RIL_REQUEST_OPERATOR:
            requestOperator(data, datalen, t);
            break;

        case RIL_REQUEST_RADIO_POWER:
            requestRadioPower(data, datalen, t);
            break;

        case RIL_REQUEST_DTMF: {
            char c = ((char *)data)[0];
            char *cmd;
            asprintf(&cmd, "AT+VTS=%c", (int)c);
            at_send_command(cmd, NULL);
            free(cmd);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }

        case RIL_REQUEST_SEND_SMS:
            requestSendSMSEVDO(data, datalen, t);
            break;

        case RIL_REQUEST_SETUP_DATA_CALL:
            requestSetupDataCallEVDO(data, datalen, t);
            break;

        case RIL_REQUEST_DEACTIVATE_DATA_CALL:
            requestDeactivateDataCallEVDO(data, datalen, t);
            break;

        case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
            requestDataCallFailCause(data, datalen, t);
            break;

        case RIL_REQUEST_SMS_ACKNOWLEDGE:
            requestSMSAcknowledge(data, datalen, t);
            break;

        case RIL_REQUEST_GET_IMSI:
            requestGetIMSI(data, datalen, t);
            break;

        case RIL_REQUEST_BASEBAND_VERSION:
            requestBasebandVersion(data, datalen, t);
            break;

        case RIL_REQUEST_GET_IMEI:
            requestGetIMEI(data, datalen, t);
            break;

        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
            requestSetPreferredNetworkType(data, datalen, t);
            break;

        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
            requestGetPreferredNetworkType(data, datalen, t);
            break;

        case RIL_REQUEST_SIM_IO:
            requestSIM_IO(data,datalen,t);
            break;

        case RIL_REQUEST_SEND_USSD:
            requestSendUSSD(data, datalen, t);
            break;

        case RIL_REQUEST_CANCEL_USSD:
            p_response = NULL;
            err = at_send_command_numeric("AT+CUSD=2", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            requestQueryNetworkSelectionMode(data, datalen, t);
            break;

        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
            at_send_command("AT+COPS=0", NULL);
            fw100Ctx.networkSelectMode = 0;	// search comment 'fw100 network mode'
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        //  FIXME the request contains the MCCMNC to select
        // "data" is const char * specifying MCCMNC of network to select (eg "310170")
        case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
            at_send_command("AT+COPS=1", NULL);
            fw100Ctx.networkSelectMode = 1;     
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
            requestQueryAvailableNetworks(data, datalen, t);
            break;

        case RIL_REQUEST_DATA_CALL_LIST:
            requestDataCallList(data, datalen, t);
            break;

        case RIL_REQUEST_OEM_HOOK_RAW:
            // echo back data
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;


        case RIL_REQUEST_OEM_HOOK_STRINGS: {
            int i;
            const char ** cur;

            LOGD("got OEM_HOOK_STRINGS: 0x%8p %lu", data, (long)datalen);


            for (i = (datalen / sizeof (char *)), cur = (const char **)data ;
                    i > 0 ; cur++, i --) {
                LOGD("> '%s'", *cur);
            }

            // echo back strings
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;
        }

        case RIL_REQUEST_WRITE_SMS_TO_SIM:
            requestWriteSmsToSim(data, datalen, t);
            break;

        case RIL_REQUEST_DELETE_SMS_ON_SIM: {
            char * cmd;
            p_response = NULL;
            asprintf(&cmd, "AT+CMGD=%d", ((int *)data)[0]);
            err = at_send_command(cmd, &p_response);
            free(cmd);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }

        case RIL_REQUEST_ENTER_SIM_PIN:
        case RIL_REQUEST_ENTER_SIM_PUK:
        case RIL_REQUEST_ENTER_SIM_PIN2:
        case RIL_REQUEST_ENTER_SIM_PUK2:
        case RIL_REQUEST_CHANGE_SIM_PIN:
        case RIL_REQUEST_CHANGE_SIM_PIN2:
            requestEnterSimPin(data, datalen, t);
            break;

        default:
            #if 0
            LOGD("%s request not supported\n", __FUNCTION__);
            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            #endif

            LOGD("%s request not supported, reported radio not available\n", __FUNCTION__);
            RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);

            break;
    }
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
static RIL_RadioState
currentState()
{
    return fw100Ctx.sState;
}
/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported"
 */

static int
onSupports (int requestCode)
{
    const ril_request_info_t *info = requestInfo(requestCode);
    if (NULL == info) return 0;
    return (info->profile <= RRI_MY_PROFILE) ? 1 : 0;
}

static void onCancel (RIL_Token t)
{
    //@@@todo

}

static const char * getVersion(void)
{
    return "Fusion Wireless RIL 1.0";
}

static void
setRadioState(RIL_RadioState newState)
{
    RIL_RadioState oldState;

    pthread_mutex_lock(&fw100Ctx.s_state_mutex);

    oldState = fw100Ctx.sState;

    if (fw100Ctx.s_closed > 0) {
        // If we're closed, the only reasonable state is
        // RADIO_STATE_UNAVAILABLE
        // This is here because things on the main thread
        // may attempt to change the radio state after the closed
        // event happened in another thread
        newState = RADIO_STATE_UNAVAILABLE;
    }

    if (fw100Ctx.sState != newState || fw100Ctx.s_closed > 0) {
        fw100Ctx.sState = newState;

        pthread_cond_broadcast (&fw100Ctx.s_state_cond);
    }

    pthread_mutex_unlock(&fw100Ctx.s_state_mutex);


    /* do these outside of the mutex */
    if (fw100Ctx.sState != oldState) {
        RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                    NULL, 0);

        /* FIXME onSimReady() and onRadioPowerOn() cannot be called
         * from the AT reader thread
         * Currently, this doesn't happen, but if that changes then these
         * will need to be dispatched on the request thread
         */
        if (fw100Ctx.sState == RADIO_STATE_SIM_READY) {
            onSIMReady();
        } else if (fw100Ctx.sState == RADIO_STATE_SIM_NOT_READY) {
            onRadioPowerOn();
        }
    }
}

/**
 * \brief check SIM ready
 * fw100 does not support SIM or RUIM
 * always returns ready
 */
static SIM_Status getSIMStatus()
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    // neither SIM nor RUIM are supported
    return SIM_READY;

    if (fw100Ctx.sState == RADIO_STATE_OFF || fw100Ctx.sState == RADIO_STATE_UNAVAILABLE) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start (&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp (cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp (cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp (cpinResult, "READY"))  {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = SIM_READY;

done:
    at_response_free(p_response);
    return ret;
}

/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatus(RIL_CardStatus **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 1;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus *p_card_status = malloc(sizeof(RIL_CardStatus));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->num_applications = num_apps;

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        // Only support one app, gsm
        p_card_status->num_applications = 1;
        p_card_status->gsm_umts_subscription_app_index = 0;

        // Get the correct app status
        p_card_status->applications[0] = app_status_array[sim_status];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus *p_card_status) {
    free(p_card_status);
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands)
 */

static void pollSIMState (void *param)
{
    ATResponse *p_response;
    int ret;

    if (fw100Ctx.sState != RADIO_STATE_SIM_NOT_READY) {
        // no longer valid to poll
        return;
    }

    switch(getSIMStatus()) {
        case SIM_ABSENT:
        case SIM_PIN:
        case SIM_PUK:
        case SIM_NETWORK_PERSONALIZATION:
        default:
            setRadioState(RADIO_STATE_SIM_LOCKED_OR_ABSENT);
        return;

        case SIM_NOT_READY:
            RIL_requestTimedCallback (pollSIMState, NULL, &fw100Ctx.TIMEVAL_SIMPOLL);
        return;

        case SIM_READY:
            setRadioState(RADIO_STATE_SIM_READY);
        return;
    }
}

/**
 * \brief AT commands to initialize modem
 */
static void atcommand_init()
{
    ATResponse *p_response = NULL;
    int err;

    /* note: we don't check errors here. Everything important will
       be handled in onATTimeout and onATReaderClosed */

    /*  atchannel is tolerant of echo but it must */
    /*  have verbose result codes */
    at_send_command("ATE0Q0V1", NULL);

    /*  No auto-answer */
    at_send_command("ATS0=0", NULL);

    /*  Extended errors */
    at_send_command("AT+CMEE=1", NULL);

    //  Network registration events 
    // enabled unsolicted status callback
    // fw100 no support for CREG=2 
    err = at_send_command("AT+CREG=1", &p_response);

    at_response_free(p_response);

    // FW100 commands
    /*Send URC +VMCCMNC when MCC/MNC changed in network */
    at_send_command("AT+VMCCMNC=0", NULL);
    // not a GSM AT command but not sure where this is documented
    // Report CSQ when RSSI changed
    at_send_command("AT+ARSI=1,4", NULL);

    /*  SMS PDU mode */
    at_send_command("AT+CMGF=0", NULL);

    /*  MEID */
    at_send_command("AT^MEID", NULL);

    /*  VMDN */
    at_send_command("AT+VMDN?", NULL);

    /*  HDRCSQ */
    at_send_command("AT^HDRCSQ", NULL);

    /*  SYSINFO */
    at_send_command("AT^SYSINFO", NULL);

    /*  CSNID */
    at_send_command("AT+CSNID?", NULL);

    if ((fw100Ctx.gpsFifoEnable) || (fw100Ctx.gpsTtyEnable))
    {
    	at_send_command("AT^GPSLOC=0",     NULL);
    	at_send_command("AT^GPSMODE=1",    NULL);
    	at_send_command("AT^GPSQOS=1,255", NULL);
	#if GPS_RATE_1PPS
    	at_send_command("AT^GPSLOC=1,1",  NULL);
	#endif
	#if GPS_RATE_10PPS
    	at_send_command("AT^GPSLOC=1,10",  NULL);
	#endif
	
    }
}

/**
 * \brief first callback.
 * initialize everything that can be configured while we're still 
 * in power off state AT+CFUN=0
 */
static void initializeCallback(void *param)
{
    ATResponse *p_response = NULL;
    int err;

    setRadioState (RADIO_STATE_OFF);

    at_handshake();

    atcommand_init();

    /* assume radio is off on error */
    if (isRadioOn() > 0) {
        setRadioState (RADIO_STATE_SIM_NOT_READY);
    }
}

static void waitForClose()
{
    pthread_mutex_lock(&fw100Ctx.s_state_mutex);

    while (fw100Ctx.s_closed == 0) {
        pthread_cond_wait(&fw100Ctx.s_state_cond, &fw100Ctx.s_state_mutex);
    }

    pthread_mutex_unlock(&fw100Ctx.s_state_mutex);
}

/**
 * \brief Called by atchannel when an unsolicited line appears
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here
 * 
 * \param s - modem AT string 
 */
static void onUnsolicited (const char *s, const char *sms_pdu)
{
    char *line = NULL;
    int err;


    /* Ignore unsolicited responses until we're initialized.
     * This is OK because the RIL library will poll for initial state
     */
    if (fw100Ctx.sState == RADIO_STATE_UNAVAILABLE) {
        return;
    }

    if (strStartsWith(s, "%CTZV:")) {
        /* TI specific -- NITZ time */
        char *response;

        line = strdup(s);
        at_tok_start(&line);

        err = at_tok_nextstr(&line, &response);

        if (err != 0) {
            LOGE("invalid NITZ line %s\n", s);
        } else {
            RIL_onUnsolicitedResponse (
                RIL_UNSOL_NITZ_TIME_RECEIVED,
                response, strlen(response));
        }
    } else if (strStartsWith(s,"+CRING:")
                || strStartsWith(s,"RING")
                || strStartsWith(s,"NO CARRIER")
                || strStartsWith(s,"+CCWA")
    ) {
        LOGD ("%s:%d bypassed CALL_STATE_CHANGED\n", __FUNCTION__, __LINE__);
        #if 0
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
            NULL, 0);
        #endif
    } else if (strStartsWith(s,"+CREG:")
                || strStartsWith(s,"+CGREG:")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
            NULL, 0);
    } else if (strStartsWith(s, "+CMT:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CDS:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
            sms_pdu, strlen(sms_pdu));
    }
    // vzw *22899 over the air activation 
    else if (strStartsWith(s, "^ORIG:")) {
    }
    // over the air activation message
    // save a copy of it
    else if (strStartsWith(s, "^OTACMSG:")) {
        fw100Ctx.otaMsgCnt++;
        strncpy(fw100Ctx.otaMsg, s, sizeof(fw100Ctx.otaMsg)); 
    }
    // over the air activation 
    else if (strStartsWith(s, "^CONN:")) {
    }
    // over the air activation 
    else if (strStartsWith(s, "^CEND:")) {
    }

    // GPS NMEA
    else if (strStartsWith(s, "$GP")) {
	if (fw100Ctx.gpsFifoEnable) rilWriteGPSFifo(&fw100Ctx, RIL_GPS_FIFOPATH, s);
	if (fw100Ctx.gpsTtyEnable)  rilWriteGPSTty(&fw100Ctx, s, 0);
    }
}

/* Called on command or reader thread */
static void onATReaderClosed()
{
    LOGI("AT channel closed\n");
    at_close();
    fw100Ctx.s_closed = 1;

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

/* Called on command thread */
static void onATTimeout()
{
    LOGI("AT channel timeout; closing\n");
    at_close();

    fw100Ctx.s_closed = 1;

    /* FIXME cause a radio reset here */

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

static void usage(char *s)
{
    char tmp[128];
    sprintf(tmp, "%s -d /dev/data_device -a /dev/atctrl_device\n", __FILE__);
    LOGD("%s %s\n", __FUNCTION__, tmp);
}

/**
 *  \brief sync modem AT port 
 *  AT/OK response 
 *  some modems autobaud and this will sync the
 *  AT port before processing AT general commands.
 *
 *  \param fd - serial port file descriptor
 *  \param devname - device node for diagnostic
 *
 *  \return 
 *  0 = MODEM ok
 *  -1 = MODEM bad 
 */
int sync_modem(int fd, const char *devname)
{
      int ret = -1;
      char buffer[255];  /* Input buffer */
      char *bufptr;      /* Current char in buffer */
      int  nbytes;       /* Number of bytes read */
      int  tries;        /* Number of tries so far */

      for (tries = 0; tries < 100; tries ++)
      {
        // send AT command followed by a CR 
	if (write(fd, "AT\r", 3) < 3)
	  continue;

        // read characters into our string buffer until we get 'OK' 
        // modem may echo AT\r\nOK\r\n or whatever
	bufptr = buffer;
        memset(buffer, 0, sizeof(buffer));  // nul termination
	while ((nbytes = read(fd, bufptr, buffer + sizeof(buffer) - bufptr - 1)) > 0)
	{
	  printf("%s: read %d: %s\n", __FUNCTION__, nbytes, buffer);
	  bufptr += nbytes;
	  if (strstr(buffer, "OK"))
          {
             ret = 0;
             goto done;
          }
        }
      }

     ret = 0;
done:
      if (!ret) 
                LOGD ("%s:%d %s is ready\n", __FUNCTION__, __LINE__, devname);
      else
                LOGE ("%s:%d %s not responding\n", __FUNCTION__, __LINE__, devname);
      return ret;
}

/**
 *  \brief init serial port properties for modem interface
 *  set up termios attributes
 *
 *  \param fd - serial port file descriptor
 *  \param devname - device node in case settings are device dependent
 *  \param options - per port configuration options
 * 
 *  \return 
 *  0 = port ok
 *  -1 = port bad 
 */
int configure_modem_fd(int fd, const char *devname, int options)
{
    int ret = -1;
    int rc;
    struct termios ios;
    struct termios verify;

    fcntl(fd , F_SETFL, 0);

    /* disable echo on serial ports */
    rc = tcgetattr( fd, &ios );

    /* set raw input, 1 second timeout */
    ios.c_cflag     |= (CLOCAL | CREAD);
    ios.c_lflag     &= ~(ICANON | ECHO | ECHOE | ISIG);
    ios.c_oflag     &= ~OPOST;
    ios.c_cc[VMIN]  = 1;
    ios.c_cc[VTIME] = 0;

    // baud rate
    rc = cfsetispeed(&ios, B115200);
    rc = cfsetospeed(&ios, B115200);
    // flush fd
    tcflush(fd, TCIFLUSH);
    // set attributes
    rc = tcsetattr( fd, TCSANOW, &ios );
    if (rc)
    {
        LOGE ("%s:%d tcsetattr error %d \n", __FUNCTION__, __LINE__, ret);
	goto done;
    }
    rc = tcgetattr( fd, &verify );


    ret = 0;
done: 
    return ret;
}

/**
 * \brief main loop opens and monitors AT serial port 
 * kicks off readerLoop thread if modem is opened OK
 * 
 * \param param - launch options
 */
static void *mainLoop(void *param)
{
    int fd;
    int ret;
    char tmp[128];

    AT_DUMP("== ", "entering mainLoop()", -1 );

    sprintf(tmp, "starting at:%s data:%s\n", fw100Ctx.s_atctrl_path, 
       fw100Ctx.s_data_path);
    LOGD("%s %s", __FUNCTION__, tmp);

    at_set_on_reader_closed(onATReaderClosed);
    at_set_on_timeout(onATTimeout);

    // one time create GPS ptty.  do this early to allow 
    // gps framework to open the port.  This makes a virtual tty
    // to output unsolicited GPS fix NMEA strings from modem.
    if (fw100Ctx.gpsTtyEnable) rilWriteGPSTty(&fw100Ctx, NULL, 1);

    for (;;) {
        fd = -1;
        while  (fd < 0) {
            if (fw100Ctx.s_port > 0) {
                fd = socket_loopback_client(fw100Ctx.s_port, SOCK_STREAM);
            } else if (fw100Ctx.s_device_socket) {
                if (!strcmp(fw100Ctx.s_atctrl_path, "/dev/socket/qemud")) {
                    /* Qemu-specific control socket */
                    fd = socket_local_client( "qemud",
                         ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM );
                    if (fd >= 0 ) {
                        char  answer[2];

                        if ( write(fd, "gsm", 3) != 3 ||
                             read(fd, answer, 2) != 2 ||
                             memcmp(answer, "OK", 2) != 0)
                        {
                            close(fd);
                            fd = -1;
                        }
                   }
                }
                else
                    fd = socket_local_client(fw100Ctx.s_atctrl_path,
                            ANDROID_SOCKET_NAMESPACE_FILESYSTEM, SOCK_STREAM );
            } else if (fw100Ctx.s_atctrl_path != NULL) {

                // open device
                fd = open (fw100Ctx.s_atctrl_path, (O_RDWR | O_NOCTTY ), 0666);
		if (errno == EAGAIN) goto check_fd;
		
                if ( fd >= 0 ) {
                    ret = configure_modem_fd(fd, fw100Ctx.s_atctrl_path, 0);
                    if (ret != 0) 
                    {
                      close(fd);
                      fd = -1;
                      goto check_fd;
                    }
                    ret = sync_modem(fd, fw100Ctx.s_atctrl_path);
                    if (ret != 0) 
                    {
                      close(fd);
                      fd = -1;
                      goto check_fd;
                    }
                }
            }

check_fd:
            if (fd < 0) {

                // n.b. this could happen if USB serial devices 
                // are not yet enumerated
                sprintf(tmp, "error open devname %s err=%d %s\n", 
                   fw100Ctx.s_atctrl_path, errno, strerror(errno));
                LOGD("%s %s", __FUNCTION__, tmp);

                // sleep and retry open devname
                sleep(5);
            }
        }
	
        // devices are open and ready
        fw100Ctx.mainIsStarted = 1;

        sprintf(tmp, "launching at_open on fd:%d %s\n", fd, 
           fw100Ctx.s_atctrl_path);
        LOGD("%s %s", __FUNCTION__, tmp);

        fw100Ctx.s_closed = 0;
	// start readerLoop thread
        ret = at_open(fd, onUnsolicited);

        if (ret < 0) {
            LOGE ("AT error %d on at_open\n", ret);
            return 0;
        }

        RIL_requestTimedCallback(initializeCallback, NULL, &fw100Ctx.TIMEVAL_0);

        // top sleep gives initializeCallback a chance to dispatch
	do 
	{ 
		sleep(10);

                // check activation status
                if (!fw100Ctx.moduleIsActivated) activateHelper(1);

                // check if session is closed
		if (fw100Ctx.s_closed) break;

		// modem state and services timer handles CREG, COPS, CSQ, PPP 
                // if screen state active
                if (fw100Ctx.screenState == 1)
                   fw100ModemTimer();

	} while (1);

        LOGD("%s Re-open after close\n", __FUNCTION__);
    }
}

/**
 *
 * \brief get session context handle
 * session context is a collection point for all
 * data referenced in this file and friend files
 * 
 *  \param void
 *  \return ctx - pointer to session context 
 */
fw100SessionCtx_t *fw100GetSessionCtx()
{
    return &fw100Ctx;
}

/**
 *
 * \brief initialize session context 
 * session context is a collection point for all
 * data referenced in this file and friend files
 * 
 *  \param ctx - pointer to session context 
 *  \return void
 * 
 * default settings are initialized first, 
 * then some settings are loaded at run time 
 * from ril control ini file
 *
 */
static void initSessionContext(fw100SessionCtx_t *ctx)
{
  int rc;

  ctx->sState = RADIO_STATE_OFF;

  rc = pthread_mutex_init(&ctx->s_state_mutex, NULL);
  rc = pthread_cond_init(&ctx->s_state_cond, NULL);
  
  ctx->s_port = -1;
  
  /* trigger change to this with s_state_cond */
  ctx->s_closed = 0;
  
  // fw100 network mode seems stuck in manual mode 
  // auto is 0, manual is 1
  ctx->networkSelectMode = 1;
   
  ctx->TIMEVAL_SIMPOLL.tv_sec = 1;
  ctx->TIMEVAL_SIMPOLL.tv_usec = 0;

  ctx->TIMEVAL_CALLSTATEPOLL.tv_sec = 0;
  ctx->TIMEVAL_CALLSTATEPOLL.tv_usec = 500000;

  ctx->TIMEVAL_0.tv_sec = 0;
  ctx->TIMEVAL_0.tv_usec = 0;

  ctx->screenState = SCREEN_IS_OFF;
  ctx->inDataCall = DATA_STATE_DISCONNECTED;
  ctx->dataCallIsAutomatic = 0;

  // turn on GPS NMEA output from module
  // this might be dynamically controlled on/off 
  #if GPS_USE_FIFO
  ctx->gpsFifoEnable = 1;
  #endif
  #if GPS_USE_TTY
  ctx->gpsTtyEnable = 1;
  #endif

  ctx->gpsFifoFD = -1;	
  ctx->gpsTtyFD = -1;

  // search pointer to devname in dev_path
  ctx->s_data_devname = trim_path(ctx->s_data_path);

  // activation related
  ctx->moduleAutoActivate = 1;
  ctx->moduleIsActivated = 0;
  ctx->moduleActivateRetry = MAX_AUTO_ACTIVATE_RETRY;

  // read run-time preferences
  rilReadControl(ctx, RIL_CONTROL_FILEPATH);

}

/**
 *
 * \brief entry point to start up RIL driver
 *  \param env  RIL driver session context
 *  \param argc argument count
 *  \param argv argument vector
 * 
 *  argument list:
 *  data device node: -d /dev/ttyUSB0
 *  AT control device node: -a /dev/ttyUSB2
 */
const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
    int ret;
    int opt;
    pthread_attr_t attr;

    // wipe session context
    memset(&fw100Ctx, 0, sizeof(fw100SessionCtx_t));

    s_rilenv = env;

    while ( -1 != (opt = getopt(argc, argv, "p:d:a:s:"))) {
        switch (opt) {
            case 'p':
                fw100Ctx.s_port = atoi(optarg);
                if (fw100Ctx.s_port == 0) {
                    usage(argv[0]);
                    return NULL;
                }
                LOGI("Opening loopback port %d\n", fw100Ctx.s_port);
                break;

            case 'a':
                fw100Ctx.s_atctrl_path = optarg;
                LOGI("Opening device %s\n", fw100Ctx.s_atctrl_path);
                break;

            case 'd':
                fw100Ctx.s_data_path = optarg;
                LOGI("Opening device %s\n", fw100Ctx.s_data_path);
                break;

            case 's':
                fw100Ctx.s_atctrl_path   = optarg;
                fw100Ctx.s_device_socket = 1;
                LOGI("Opening socket %s\n", fw100Ctx.s_atctrl_path);
                break;

            default:
                usage(argv[0]);
                return NULL;
        }
    }

    if (fw100Ctx.s_port < 0 && fw100Ctx.s_atctrl_path == NULL) {
        usage(argv[0]);
        return NULL;
    }

    // options are OK, initialize session context
    initSessionContext(&fw100Ctx);

    pthread_attr_init (&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&fw100Ctx.s_tid_mainloop, &attr, mainLoop, NULL);

    do 
    {
       // delay while mainLoop starts up and configures modem port
       sleep(1);
    } while (!fw100Ctx.mainIsStarted);

    return &myRILDriverCallbacks;
}

