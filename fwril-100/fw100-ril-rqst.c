/** 
 * \file fw100-ril-rqst.c 
 * \brief ril request handlers 
 *
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

// build option
#define BUILD_MCCMNC_WORKAROUND	0

// mobile network operators
static char *operator_info[][3] = 
{
              {"310000", "Verizon Wireless", "VZW"}, 
              {"310004", "Verizon Wireless", "VZW"}, 
              {"310005", "Verizon Wireless", "VZW"}, 
              {"310012", "Verizon Wireless", "VZW"}, 
              {NULL, NULL, NULL}   // must have this to terminate list
};

static int cdma_dbm = 0;
static int cdma_ecio = 0;
static int evdo_dbm = 0;
static int evdo_ecio = 750;
static int evdo_ratio = 8;

// PRIVATE
static int getNetworkInfo(char *response[]);
static int clccStateToRILState(int state, RIL_CallState *p_state);
static int callFromCLCCLine(char *line, RIL_Call *p_call);

/** 
 * \brief query radio power state
 *
 * \return 
 * 1 if on, 0 if off, and -1 on error 
 * 
 * note:  requestRadioPower uses CPON/CPOF
 * this VPON must be used to query not CFUN
 * 
 */
int isRadioOn()
{
    ATResponse *p_response = NULL;
    int err;
    int stacken;
    char *line;
    char display_en, ps_en;
    LOGD("isRadioOn");
    err = at_send_command_singleline("AT+VPON?", "+VPON:", &p_response);

    if (err < 0 || p_response->success == 0) {
        // assume radio is off
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &display_en);
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &ps_en);
    if (err < 0) goto error;

    at_response_free(p_response);

    // 111007 internal mode workaround
    stacken = 0;
    LOGD("%s isRadioOn ps_en=%d was isnow=%d", 
		__FUNCTION__, ps_en, stacken);

    return (int)stacken;

error:

    at_response_free(p_response);
    return -1;
}

/**
 * \brief simplified SW, HW version
 * response is free format text string for log reporting
 * 
 * \param data NULL
 * \param datalen zero
 * 
 */
void requestBasebandVersion(void *data, size_t datalen, RIL_Token t)
{
    int err = 0;
    char *pc = NULL;
    char *pmodel = NULL;
    char *psw = NULL;
    char *phw = NULL;
    char version[96];
    ATResponse *p_response = NULL;
    char* line = NULL;

    err = at_send_command_singleline("AT+CGMM", "+CGMM:", &p_response);
    if ((err != 0) || (p_response->success == 0)) 
    {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_nextstr(&line, &pc);
    if (err < 0) goto error;

    // skip +CGMM
    pmodel = pc+6;

    err = at_send_command_singleline("AT+GMR", "+GMR:", &p_response);
    if ((err != 0) || (p_response->success == 0)) 
    {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_nextstr(&line, &pc);
    if (err < 0) goto error;

    // skip +GMR:"
    psw = pc + 6;

    err = at_send_command_singleline("AT^HWVER", "^HWVER:", &p_response);
    if ((err != 0) || (p_response->success == 0)) 
    {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_nextstr(&line, &pc);
    if (err < 0) goto error;

    // skip ^HWVER:
    phw = pc + 7;

    snprintf(version, sizeof(version), "%s sw:%s hw:%s", pmodel, psw, phw);

    LOGD("response = %s", version);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, version, sizeof(char *));
    return;

error:
    LOGE("%s ERROR: requestBasebandVersion failed\n", __FUNCTION__);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

#if 0
/**
 * \brief simplified SW, HW version
 * response is free format text string for log reporting
 * 
 * \param data NULL
 * \param datalen zero
 * 
 */
void requestBasebandVersion(void *data, size_t datalen, RIL_Token t)
{
    int err = 0;
    char *pc = NULL;
    char *pmodel = NULL;
    char *psw = NULL;
    char *phw = NULL;
    char version[96];
    ATResponse *p_response = NULL;
    char* line = NULL;
    int retry;

    retry = 0;
getcgmm:
    err = at_send_command_singleline("AT+CGMM", "+CGMM:", &p_response);
    if ((err != 0) || (p_response->success == 0)) 
    {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_nextstr(&line, &pc);
    if (err < 0) goto error;

    pmodel = strstr(pc,"FW 27");
    if (NULL != pmodel) {
	// copy sw version as-is
    }
    else {
        if(retry == 5) {
	    pmodel = "N/A";
        } else {
            LOGW("%s:%d CGMM retry:%d\n", 
		__FUNCTION__, __LINE__, retry);
            retry++;
            goto getcgmm;
        }
    }

    retry = 0;
getgmr:
    err = at_send_command_singleline("AT+GMR", "+GMR:", &p_response);
    if ((err != 0) || (p_response->success == 0)) 
    {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_nextstr(&line, &pc);
    if (err < 0) goto error;

    if(strstr(pc,"+GMR:")) {
	psw = pc + 6;
    }    
    else {
        if(retry == 5) {
	    psw = "N/A";
        } else {
            LOGW("%s:%d GMR retry:%d\n", 
		__FUNCTION__, __LINE__, retry);
            retry++;
            goto getgmr;
        }
    }

    retry = 0;
gethwver:
    err = at_send_command_singleline("AT^HWVER", "^HWVER:", &p_response);
    if ((err != 0) || (p_response->success == 0)) 
    {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_nextstr(&line, &pc);
    if (err < 0) goto error;

    if(strstr(pc,"^HWVER:")) {
	phw = pc + 7;
    }    
    else {
        if(retry == 5) {
	    phw = "N/A";
        } else {
            LOGW("%s:%d HWVER retry:%d\n", 
		__FUNCTION__, __LINE__, retry);
            retry++;
            goto gethwver;
        }
    }

    snprintf(version, sizeof(version), "%s sw:%s hw:%s", pmodel, psw, phw);

    LOGD("response = %s", version);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, version, sizeof(char *));
    return;

error:
    LOGE("%s ERROR: requestBasebandVersion failed\n", __FUNCTION__);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}
#endif

/**
 * \brief get equipment ID 
 * 
 * \param data NULL
 * \param datalen zero
 * 
 */
void requestGetIMEI(void *data, size_t datalen, RIL_Token t)
{
    int ii;
    int err = 0;
    ATResponse *p_response = NULL;
    char *line = NULL;
    char *skip;
    char * responseStr = NULL;

    err = at_send_command_singleline("AT+VGMUID?","+VGMUID:",&p_response);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;
    for (ii=0; ii < 6; ii++) 
    {
    	err = at_tok_nextstr(&line, &skip);
    	if (err < 0) goto error;
    }
    err = at_tok_nextstr(&line, &responseStr);
    if (err < 0) goto error;

    // trim leading '0x'
    if ((*responseStr == '0') && (*(responseStr+1) == 'x')) responseStr += 2;
    // NOTE THIS IS REPORTED UNDER MY PHONE STATUS
    strToUpper(responseStr, strlen(responseStr));
    LOGD("%s responseStr is %s", __FUNCTION__, responseStr);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, sizeof(char *));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/**
 * \brief get mobile subscriber ID
 * in GSM like IMSI form MCC+MNC+MEID
 * 
 * \param data NULL
 * \param datalen zero
 * 
 */
void requestGetIMSI(void *data, size_t datalen, RIL_Token t)
{
    int ii;
    int err = 0;
    ATResponse *p_response = NULL;
    char *line = NULL;
    char *skip;
    char *meid = NULL;
    char *network[3]; 
    char responseStr[64];

    memset(&network, 0, sizeof(network));
    err = getNetworkInfo(network);
    if (err) goto error;

    err = at_send_command_singleline("AT^MEID","^MEID:",&p_response);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;
    err = at_tok_nextstr(&line, &meid);
    if (err < 0) goto error;

    // trim leading '0x'
    if ((*meid == '0') && (*(meid+1) == 'x')) meid += 2;
    strToUpper(meid, strlen(meid));

    // concatenate MCC, MNC, MEID
    memset(responseStr, 0, sizeof(responseStr));
    strncpy(responseStr, network[2], sizeof(responseStr));
    strncat(responseStr, meid, sizeof(responseStr));

    LOGD("%s responseStr = %s", __FUNCTION__, responseStr);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, sizeof(char *));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/**
 * \brief get device identity 
 * used in CDMA mode to get ESN and MEID
 * 
 * The request is always allowed and contains GSM and CDMA device identity;
 * it substitutes the deprecated requests RIL_REQUEST_GET_IMEI and
 * RIL_REQUEST_GET_IMEISV.
 *
 * If a NULL value is returned for any of the device id, it means that error
 * accessing the device.
 *
 * When CDMA subscription is changed the ESN/MEID may change.  The application
 * layer should re-issue the request to update the device identity in this case.
 *
 * "response" is const char **
 * ((const char **)response)[0] is IMEI if GSM subscription is available
 * ((const char **)response)[1] is IMEISV if GSM subscription is available
 * ((const char **)response)[2] is ESN if CDMA subscription is available
 * ((const char **)response)[3] is MEID if CDMA subscription is available
 *
 * Valid errors:
 *  SUCCESS
 *  RADIO_NOT_AVAILABLE
 *  GENERIC_FAILURE
 * 
 */
void requestDeviceIdentity(void *data, size_t datalen, RIL_Token t)
{
    int ii;
    int err = 0;
    ATResponse *p_gsn = NULL;
    ATResponse *p_meid = NULL;
    char *line = NULL;
    char *skip;
    char *gsn = NULL;
    char *meid = NULL;
    char *response[4]; 
    char responseStr[64];

    memset(&response, 0, sizeof(response));

    err = at_send_command_singleline("AT+GSN","+GSN:",&p_gsn);
    if (err != 0 || p_gsn->success == 0) {
        goto error;
    }

    line = p_gsn->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;
    err = at_tok_nextstr(&line, &gsn);
    if (err < 0) goto error;

    response[2] = gsn;

    err = at_send_command_singleline("AT^MEID","^MEID:",&p_meid);
    if (err != 0 || p_meid->success == 0) {
        goto error;
    }

    line = p_meid->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;
    err = at_tok_nextstr(&line, &meid);
    if (err < 0) goto error;

    // trim leading '0x'
    if ((*meid == '0') && (*(meid+1) == 'x')) meid += 2;
    strToUpper(meid, strlen(meid));
    response[3] = meid;

    LOGD("%s esn=%s meid=%s", __FUNCTION__, gsn, meid);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    if (NULL != p_gsn) at_response_free(p_gsn);
    if (NULL != p_meid) at_response_free(p_meid);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    if (NULL != p_gsn) at_response_free(p_gsn);
    if (NULL != p_meid) at_response_free(p_meid);
}

/**
 * \brief search network info, used by 
 * requestOperator()
 * requestQueryAvailableNetworks()
 * 
 * \param response char pointers array
 * [0] network long name
 * [1] network abbreviation
 * [2] MCCMNC

 * return 
 * 0 OK
 * -1 error network info could not be located
 */
static int getNetworkInfo(char *response[])
{
    int i;
    int err;
    int skip;
    int  display;
    char *mcctoken;
    char *mnctoken;
    char mccmnc[32];
    char *line = NULL;
    ATResponse *p_response = NULL;
    ATResponse *p_response2 = NULL;
    fw100SessionCtx_t *ctx = fw100GetSessionCtx();

    err = at_send_command_singleline("AT+VMCCMNC?","+VMCCMNC:",&p_response);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &skip);
    if (err < 0) goto error;
    err = at_tok_nextstr(&line, &mcctoken);
    if (err < 0) goto error;
    err = at_tok_nextstr(&line, &mnctoken);

    sprintf(mccmnc,"%s%s",mcctoken,mnctoken); // table search string

    #if BUILD_MCCMNC_WORKAROUND
    // workaround for MCCMNC returned 310,00
    if (!strcmp(mccmnc, "31000")) strncpy(mccmnc, "310000", sizeof(mccmnc));
    #endif

    response[2] = mccmnc;

    // simple linear search for mccmnc match
    i = 0;
    do 
    {
        if (0 == strcmp(mccmnc,operator_info[i][0])) {
             response[0] = operator_info[i][1];
             response[1] = operator_info[i][2];
             goto done;
        }
        i++;
    } while (operator_info[i][0] != NULL);

    // no match, try QCMIPGETP
    err = at_send_command_singleline("AT$QCMIPGETP=0","",&p_response2);
    if (err != 0 || p_response2->success == 0) {
        goto error;
    }

    line = p_response2->p_intermediates->line;

    if (strstr(line, "sprint")) 
    {
        response[0] = "Sprint PCS";
        response[1] = "SPCS";
        response[2] = "310120";
    }
    else if (strstr(line, "vzw")) 
    {
        response[0] = "Verizon Wireless";
        response[1] = "VZW";
        response[2] = "310004";
    }
    // not found
    else 
    {
    	LOGD("%s unknown operator %s", __FUNCTION__, line);
    	response[0] = "Unknown Operator";
    	response[1] = "UNK";
    	response[2] = "000000";
    }

done:
    strncpy(ctx->carrier, response[1], sizeof(ctx->carrier));  // save a copy 
    if (NULL != p_response)  at_response_free(p_response);
    if (NULL != p_response2) at_response_free(p_response2);

    // update ril status with carrier
    rilWriteStatus(ctx, RIL_STATUS_FILEPATH);

    return 0;

error:
    if (NULL != p_response)  at_response_free(p_response);
    if (NULL != p_response2) at_response_free(p_response2);
    return -1;
}

/**
 * \brief request operator
 * read MCC mobile country code
 * read MNC mobile network code
 * http://en.wikipedia.org/wiki/Mobile_Network_Code
 * 
 * \param data NULL
 * \param datalen zero
 * 
 * USA VZW:
 * at+vmccmnc?
 * +VMCCMNC:0,310,00
 * MCC 310 US
 * MNC unknown
 *
 * ONS Operator Name String
 * EONS Enhanced Operator Name String
 *
 * "response" is a "const char **"
 * ((const char **)response)[0] is long alpha ONS or EONS
 *                                  or NULL if unregistered
 *
 * ((const char **)response)[1] is short alpha ONS or EONS
 *                                  or NULL if unregistered
 * ((const char **)response)[2] is 5 or 6 digit numeric code (MCC + MNC)
 *                                  or NULL if unregistered
 */
void requestOperator(void *data, size_t datalen, RIL_Token t)
{
    int err;
    int  display;
    char *response[3];
    char display_optname[20];

    memset(&response, 0, sizeof(response));
    err = getNetworkInfo(response);
    if (err) goto error;

    // N.B. this property must be asserted for operator display 
    property_get("ro.telephony.display_optname",display_optname,"0");
    LOGD("requestOperator: display_optname = %s", display_optname);

    #if 0
    FIXME force display
    // check if display is suppressed
    display = atoi(display_optname);
    if (!display)
    {
       response[0] = NULL;
       response[1] = NULL;
       goto done;
    }
    #endif
 
done:
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    return;

error:
    LOGE("%s requestOperator must not return error when radio is on", __FUNCTION__);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    return;
}

/**
 * \brief query available networks
 * similar to requestOperator except the response adds a 
 * availability field
 *
 * ((const char **)response)[n+0] is long alpha ONS or EONS
 * ((const char **)response)[n+1] is short alpha ONS or EONS
 * ((const char **)response)[n+2] is 5 or 6 digit numeric code (MCC + MNC)
 * ((const char **)response)[n+3] is a string value of the status:
 *           "unknown"
 *           "available"
 *           "current"
 *           "forbidden"
 *
 */
void requestQueryAvailableNetworks(void *data, size_t datalen, RIL_Token t)
{
    int err;
    char *response[4];

    memset(&response, 0, sizeof(response));
    err = getNetworkInfo(response);
    if (err) goto error;
    response[3] = "available";
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    return;
}

/**
 * \brief handles registration state
 * 
 * AT> AT+CREG?
 * AT< +CREG:1,4145,7,1
 * 
 * AT> AT^SYSINFO
 * AT< ^SYSINFO:2,255,0,8,240
 *
 * AT> AT+CSNID?
 * AT< +CSNID:4145,7
 * 
 * AT> AT+VROM?
 * AT< +VROM:0,1
 * 
 * n.b. see notes in ril.h RIL_REQUEST_REGISTRAION_STATE
 * response field description including response[3]
 * radio_technology
 * 
 */ 
void requestRegistrationStateEVDO(int request, void *data,
     size_t datalen, RIL_Token t)
{
    int errline = 0;
    int err = 0;
    RIL_Registration_response response;
    int response_protocolrev = 0;
    int response_roaming_indicator = 0;
    ATResponse *p_response_status = NULL;
    ATResponse *p_response_status_delay = NULL;
    ATResponse *p_response_protocolrev = NULL;
    ATResponse *p_response_sidnid = NULL;
    ATResponse *p_response_roaming = NULL;
    char *line_status = NULL;
    char *line_protocolrev = NULL;
    char *line_sidnid = NULL;
    char *line_roaming = NULL;
    char *evdo_rev_a = REGISTRATION_EVDO_REV_A;
    char *cdma_1xrtt = REGISTRATION_CDMA_1XTT;
    char *network_unknown = REGISTRATION_NETWORK_UNKNOWN;
    char *default_roaming_indicator = DEFAULT_ROAMING_INDICATOR;
    char *default_val = REGISTRATION_DEFAULT_VALUE;
    char *default_prl_val = REGISTRATION_DEFAULT_PRL_VALUE;
    int skip = 0;

    // NULL out unused response fields
    memset(&response, 0, sizeof(response));

    // Get registration state
    // response
    // n: 
    //    0 disable unsolicited network registration result codes
    //    1 enable unsolicited network registration result codes
    // SID
    // NID
    // status:   
    //    0 not registered
    //    1 registered, home network
    //    2 not registered, searching for BS
    //    3 registration denied
    //    4 unknown
    // 
    err = at_send_command_singleline ("AT+CREG?", "+CREG:", &p_response_status);
    if((err != 0) || (p_response_status->success == 0))
    {
        LOGE("%s error in CREG", __FUNCTION__);
	errline = __LINE__; goto error;
    }
    line_status = p_response_status->p_intermediates->line;
    err = at_tok_start(&line_status);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextint(&line_status, &skip);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextint(&line_status, &skip);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextint(&line_status, &skip);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextstr(&line_status, &response.register_state);
    if (err < 0) {errline = __LINE__; goto error;}

    // Get protocol revision
    // AT> AT^SYSINFO
    // AT< ^SYSINFO:2,255,0,8,240
    //                    ^ ^
    //            roam ---| |---protocol
    //
    err = at_send_command_singleline ("AT^SYSINFO", "^SYSINFO:", &p_response_protocolrev);
    if((err != 0) || (p_response_protocolrev->success == 0))
    {
        LOGE("%s error in SYSINFO", __FUNCTION__);
	errline = __LINE__; goto error;
    }
    line_protocolrev = p_response_protocolrev->p_intermediates->line;
    err = at_tok_start(&line_protocolrev);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextint(&line_protocolrev, &skip);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextint(&line_protocolrev, &skip);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextint(&line_protocolrev, &response_roaming_indicator);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextint(&line_protocolrev, &response_protocolrev);
    if (err < 0) {errline = __LINE__; goto error;}

    if (response_protocolrev == 8 || response_protocolrev == 4) {
       response.radio_technology = evdo_rev_a;
    } else if (response_protocolrev == 2){
       response.radio_technology =  cdma_1xrtt;
    } else {
        response.radio_technology =  network_unknown;
    }

    // force network type non-cdma for GSM emulation
    // 1=GPRS, 2=EDGE, 3=UMTS
    if (request == RIL_REQUEST_REGISTRATION_STATE)
        response.radio_technology = "3";  
    else if (request == RIL_REQUEST_GPRS_REGISTRATION_STATE)
        response.radio_technology = "3";  

    // toggle the sense of roaming indicator
    if(response_roaming_indicator == 1)
        response.roaming_indicator = "0";
    else
        response.roaming_indicator = "1";
       
    // Get System ID and Network ID
    err = at_send_command_singleline ("AT+CSNID?", "+CSNID:", &p_response_sidnid);
    if((err != 0) || (p_response_sidnid->success == 0))
    {
        LOGE("%s error in CSNID", __FUNCTION__);
	errline = __LINE__; goto error;
    }
    line_sidnid = p_response_sidnid->p_intermediates->line;
    err = at_tok_start(&line_sidnid);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextstr(&line_sidnid, &response.system_id);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextstr(&line_sidnid, &response.network_id);
    if (err < 0) {errline = __LINE__; goto error;}
    
    // Get TSB-58 Roaming Indicator
    err = at_send_command_singleline ("AT+VROM?", "+VROM:", &p_response_roaming);
    if((err != 0) || (p_response_roaming->success == 0))
    {
        LOGE("%s error in VROM", __FUNCTION__);
	errline = __LINE__; goto error;
    }
    line_roaming = p_response_roaming->p_intermediates->line;
    err = at_tok_start(&line_roaming);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextint(&line_roaming, &skip);
    if (err < 0) {errline = __LINE__; goto error;}
    err = at_tok_nextstr(&line_roaming, &response.roaming_indicator);
    if (err < 0) {errline = __LINE__; goto error;}
    if(atoi(response.roaming_indicator) > 12)
    {
        response.roaming_indicator = DEFAULT_ROAMING_INDICATOR;
    }
 
    // don't touch GSM fields [1]LAC and [2]CID
    response.basestation_id = default_val;
    response.basestation_latitude = default_val;
    response.basestation_longitude = default_val;
    response.concurrent_service= default_val;
    response.prl_state = default_prl_val;
    response.deny_reason = default_val;
    response.prl_roaming_indicator = default_roaming_indicator;

    #if 0
    LOGD("%s register_state=%s", __FUNCTION__, response.register_state);
    LOGD("%s radio_technology=%s", __FUNCTION__, response.radio_technology);
    LOGD("%s roaming_indicator=%s", __FUNCTION__, response.roaming_indicator);
    #endif

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));

    at_response_free(p_response_status);
    at_response_free(p_response_protocolrev);
    at_response_free(p_response_sidnid);
    at_response_free(p_response_roaming);
    return;
    
error:
    LOGE("%s error at line %d while radio is on", __FUNCTION__, errline);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response_status);
    at_response_free(p_response_protocolrev);
    at_response_free(p_response_sidnid);
    at_response_free(p_response_roaming);
}

/**
 * \brief get network type preference
 */
void requestGetPreferredNetworkType(void *data, size_t datalen, RIL_Token t)
{
    int err = 0;
    ATResponse *p_response = NULL;
    char *line = NULL;
    int mode = 0;
    int response = 0;
    err = at_send_command_singleline ("AT^PREFMODE?", "PREFMODE:", &p_response);
    if ((err < 0) || (p_response->success == 0))
    {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
    {
        goto error;
    }
    err = at_tok_nextint(&line, &mode);
    if (err < 0)
    {
        goto error;
    }
    LOGD("mode = %d",mode);
    switch(mode){
        case 2:
            LOGD("CDMA mode");
            response = NETWORK_MODE_CDMA_NO_EVDO;
            break;
        case 4:
            LOGD("HDR mode");
            response = NETWORK_MODE_EVDO_NO_CDMA;
            break;
        case 8:
            LOGD("CDMA/HDR HYBRID mode");
            response = NETWORK_MODE_HYBRID;
            break;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/**
 * \brief set network type preference
 */
void requestSetPreferredNetworkType(void *data, size_t datalen, RIL_Token t)
{
    int err = 0;
    ATResponse *p_response = NULL;
    char *line = NULL;
    int mode = ((int*)data)[0];
    int value = 0;
    char *cmd;

    LOGD("%s mode=%d", __FUNCTION__, mode);

    switch(mode){
        case NETWORK_MODE_CDMA_NO_EVDO:
            value = 2;
            break;
        case NETWORK_MODE_EVDO_NO_CDMA:
            value = 4;
            break;
        case NETWORK_MODE_HYBRID:
            value = 8;
            break;
        default:
            goto error;
    }
    asprintf(&cmd, "AT^PREFMODE=%d", value);
    err = at_send_command(cmd, NULL);
    free(cmd);

    if (err != 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}


/**
 * \brief get signal strength, EVDO aware.
 * see notes in fw100-ril.c requestSignalStrength() for details
 * on the response structure.
 * 
 * signal strength is modem type dependent
 * typedef struct {
 *   RIL_GW_SignalStrength   GW_SignalStrength;
 *   RIL_CDMA_SignalStrength CDMA_SignalStrength;
 *   RIL_EVDO_SignalStrength EVDO_SignalStrength;
 * } RIL_SignalStrength;
 *
 */
void requestSignalStrengthEVDO(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    ATResponse *p_response_hdr = NULL;
    ATResponse *p_response_cdma_ecio = NULL;
    int err = 0;
    int err_netpar = 0;
    int skip = 0;
    int i = 9;
    int response[7] = {0};
    char *line = NULL;
    char *line_hdr = NULL;
    char *line_cdma_ecio = NULL;
    
    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_response);

    if ((err != 0) || (p_response->success == 0)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[2]));
    if (err < 0) goto error;

    err = at_send_command_singleline("AT^HDRCSQ", "^HDRCSQ:", &p_response_hdr);

    if ((err != 0) || (p_response_hdr->success == 0)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto error;
    }
    line_hdr = p_response_hdr->p_intermediates->line;
    err = at_tok_start(&line_hdr);
    if (err < 0) goto error;

    err = at_tok_nextint(&line_hdr, &(response[4]));
    if (err < 0) goto error;

    err_netpar = at_send_command_singleline ("AT+NETPAR=0", "+NETPAR:", &p_response_cdma_ecio);
    if((err_netpar != 0) || (p_response_cdma_ecio->success == 0)) {
        response[3] = CDMA_ECIO_DEFAULT;
    }
    else {
        line_cdma_ecio = p_response_cdma_ecio->p_intermediates->line;
        err = at_tok_start(&line_cdma_ecio);
        if (err < 0) goto error;
        while(i) {
            err = at_tok_nextint(&line_cdma_ecio, &skip);
            if (err < 0) goto error;
            i--;
        }
        err = at_tok_nextint(&line_cdma_ecio, &( response[3]));
        if (err < 0) goto error;
    }
    response[0] = SIGNAL_STRENGTH_DEFAULT;
    response[1] = SIGNAL_STRENGTH_DEFAULT;
    response[3] = CDMA_ECIO_DEFAULT;
    response[5] = EVDO_ECIO_DEFAULT;
    response[6] = EVDO_SNR_DEFAULT;
    //response[2] = 125-(response[2]*50)/31;        //for China Telecom requestment
    response[2] = 113 - response[2]*2;                    // 31 -->  -51dbm
    switch(response[4]) {
        case  0:
        case 20:
            response[4] = 105;
            break;
        case 40:
            response[4] = 90;
            break;
        case 60:
            response[4] = 75;
            break;
        case 80:
        case 99:
            response[4] = 60;
            break;
        default:
            response[4] = 105;
    }
	
    cdma_dbm = response[2];
    cdma_ecio = response[3];
    evdo_dbm = response[4];
    evdo_ecio = response[5];
    evdo_ratio = response[6];
	
    LOGD("before requestSignalStrength, evdo_dbm = %d, evdo_ecio = %d, evdo_ratio = %d",evdo_dbm,evdo_ecio,evdo_ratio);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));

    at_response_free(p_response);
    return;

error:
    LOGE("%s requestSignalStrength must never return an error when radio is on", __FUNCTION__);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/**
 * \brief request screen state
 * Indicates the current state of the screen.  When the screen is off, the
 * RIL should notify the baseband to suppress certain notifications (eg,
 * signal strength and changes in LAC/CID or BID/SID/NID/latitude/longitude)
 * in an effort to conserve power.  These notifications should resume when the
 * screen is on.
 *
 * "data" is int *
 * ((int *)data)[0] is == 1 for "Screen On"
 * ((int *)data)[0] is == 0 for "Screen Off"
 *
 * "response" is NULL
 *
 */
void requestScreenState(void *data, size_t datalen, RIL_Token t)
{
  int err, screenState;
  fw100SessionCtx_t *ctx = fw100GetSessionCtx();
 
  assert (datalen >= sizeof(int *));
  screenState = ((int*)data)[0];
  if(screenState == 1)
  {
      LOGD("%s on", __FUNCTION__);
      err = at_send_command("AT+ARSI=1,4", NULL);
      if (err < 0) goto error;
      err = at_send_command("AT+CREG=1", NULL);
      if (err < 0) goto error;
      err = at_send_command("AT+VMCCMNC=1", NULL);
      if (err < 0) goto error;
      err = at_send_command("AT+VSER=1", NULL);
      if (err < 0) goto error;
      // query network status again once screen is on
      RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED, NULL, 0); 
      ctx->screenState = SCREEN_IS_ON;
  }
  else if(screenState == 0)
  {
      LOGD("%s off", __FUNCTION__);
      err = at_send_command("AT+ARSI=0,4", NULL);
      if (err < 0) goto error;
      err = at_send_command("AT+CREG=0", NULL);
      if (err < 0) goto error;
      err = at_send_command("AT+VMCCMNC=0", NULL);
      if (err < 0) goto error;
      err = at_send_command("AT+VSER=0", NULL);
      if (err < 0) goto error;
      ctx->screenState = SCREEN_IS_OFF;
  }
  else
  {
      /* Not a defined value - error */
      goto error;
  }

  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

  return;

error:
  LOGE("%s ERROR: requestScreenState failed", __FUNCTION__);
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * \brief FW HCMGS text mode
 * simple 2 part payload
 * <da>
 * <message>
 *
 * "data" is const char **
 * ((const char **)data)[0] is SMSC address in GSM BCD format prefixed
 *      by a length byte (as expected by TS 27.005) or NULL for default SMSC
 * ((const char **)data)[1] is SMS in PDU format as an ASCII hex string
 *      less the SMSC address
 *      TP-Layer-Length is be "strlen(((const char **)data)[1])/2"
 *
 */
void requestSendSMSEVDO(void *data, size_t datalen, RIL_Token t)
{
    int err;
    int len;
    char tmp[128];
    int ctrlz = 0;
    int nulsmsc = 0;
    const char *smsc;
    char *pdu;
    int tpLayerLength;
    char *cmd1;
    RIL_SMS_Response response;
    ATResponse *p_response = NULL;

    // select text message format
    at_send_command("at+cmgf=1", NULL);

    smsc = ((const char **)data)[0];
    pdu = ((char **)data)[1];

    // check for PDU CTRL-Z terminator
    len = strlen(pdu);
    if (pdu[len-1] != 0x1a) 
    {
       pdu[len-1] = 0x1a;        // CTRL-Z
       ctrlz = 1;
    }

    tpLayerLength = strlen(pdu)/2;

    // "NULL for default SMSC"
    if (smsc == NULL) {
        smsc= "00";
        nulsmsc = 1;
    }

    sprintf(tmp, "smsc=%s pdu=%s ctrlz=%d nulsmsc=%d\n", smsc, pdu, ctrlz, nulsmsc);
    LOGD("%s started %s", __FUNCTION__, tmp);

    asprintf(&cmd1, "AT^HCMGS=%s", smsc);

    err = at_send_command_sms(cmd1, pdu, "^HCMGS:", &p_response);

    free(cmd1);
    if (err != 0 || p_response->success == 0)
    {
        goto error;
    }

    memset(&response, 0, sizeof(response));

    /* FIXME fill in messageRef and ackPDU */

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/**
 * 
 * \brief data only application so there should be no calls
 * Android phone stack seems to insist on using this RIL request
 * so 'unsupported' is not an option
 */
void requestGetCurrentCallsEVDO(void *data, size_t datalen, RIL_Token t)
{
    int err = 0;
    ATResponse *p_response = NULL;
    ATLine *p_cur = NULL;
    int countCalls = 0;
    int countValidCalls = 0;
    RIL_Call *p_calls = NULL;
    RIL_Call **pp_calls = NULL;
    int i = 0;
    int needRepoll = 0;

    err = at_send_command_multiline ("AT+CLCC", "+CLCC:", &p_response);

    if (err != 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* count the calls */
    for (countCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        countCalls++;
    }

    /* yes, there's an array of pointers and then an array of structures */

    pp_calls = (RIL_Call **)alloca(countCalls * sizeof(RIL_Call *));
    p_calls = (RIL_Call *)alloca(countCalls * sizeof(RIL_Call));
    memset (p_calls, 0, countCalls * sizeof(RIL_Call));
    for (countValidCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        err = callFromCLCCLine(p_cur->line, p_calls + countValidCalls);
        if (err != 0) {
            continue;
        }
        p_calls->index = countValidCalls+1;

        if (p_calls[countValidCalls].state != RIL_CALL_ACTIVE
            && p_calls[countValidCalls].state != RIL_CALL_HOLDING
        ) {
            needRepoll = 1;
        }

        countValidCalls++;
    }

    for(i = 0; i < countValidCalls ; i++) {
        pp_calls[i] = &(p_calls[i]);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_calls,
            countValidCalls * sizeof (RIL_Call *));

    at_response_free(p_response);

#if 0
#ifdef POLL_CALL_STATE
    if (countValidCalls) {  // We don't seem to get a "NO CARRIER" message from
                            // smd, so we're forced to poll until the call ends.
#else
    if (needRepoll) {
#endif
        RIL_requestTimedCallback (sendCallStateChanged, NULL, &TIMEVAL_CALLSTATEPOLL);
    }
#endif

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static int clccStateToRILState(int state, RIL_CallState *p_state)
{
    switch(state) {
        case 0: *p_state = RIL_CALL_ACTIVE;   return 0;
        case 1: *p_state = RIL_CALL_HOLDING;  return 0;
        case 2: *p_state = RIL_CALL_DIALING;  return 0;
        case 3: *p_state = RIL_CALL_ALERTING; return 0;
        case 4: *p_state = RIL_CALL_INCOMING; return 0;
        case 5: *p_state = RIL_CALL_WAITING;  return 0;
        default: return -1;
    }
}

/**
 * Note: directly modified line and has *p_call point directly into
 * modified line
 */
static int callFromCLCCLine(char *line, RIL_Call *p_call)
{
    int err;
    int state;
    int mode;
    err = at_tok_start(&line);
    if (err < 0)
    {
        goto error;
    }

    err = at_tok_nextint(&line, &(p_call->index));
    if (err < 0)
    {
        goto error;
    }
    err = at_tok_nextbool(&line, &(p_call->isMT));
    if (err < 0) 
    {
        goto error;
    }
    err = at_tok_nextint(&line, &state);
    if (err < 0) 
    {
        goto error;
    }
    err = clccStateToRILState(state, &(p_call->state));
    if (err < 0)
    {
        goto error;
    }
    err = at_tok_nextint(&line, &mode);
    if(err < 0)
    {
        goto error;
    }

    p_call->isVoice = (mode == 0);
    if(!p_call->isVoice)
    {
        LOGE("Data call line, filte");
        goto error;
    }

    err = at_tok_nextbool(&line, &(p_call->isMpty));
    if (err < 0)
    {
        goto error;
    }
    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(p_call->number));
        /* tolerate null here */
        if (err < 0) return 0;
        // Some lame implementations return strings
        // like "NOT AVAILABLE" in the CLCC line

        if (p_call->number != NULL
            && 0 == strspn(p_call->number, "+*#0123456789"))
        {
            if(strcmp(p_call->number,"Restricted") && strcmp(p_call->number,"NotAvailable") && strcmp(p_call->number,"UNKNOWN")) {
                LOGD("make number null");
                p_call->number = NULL;
            }
        }
        err = at_tok_nextint(&line, &p_call->toa);
        if (err < 0) goto error;
    }

    return 0;

error:
    LOGE("invalid CLCC line\n");
    return -1;
}



