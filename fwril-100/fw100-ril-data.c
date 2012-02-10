/** 
 * \file fw100-ril-data.c 
 * \brief ril packet data processing
 *
 */

#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <alloca.h>
#include <arpa/inet.h>
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

// build options
#define BUILD_DEBUG_1	0

/* For interface polling */
extern int ifc_init(void);
extern int ifc_get_info(const char *name, unsigned int *addr, unsigned int *mask, unsigned int *flags);
extern int ifc_get_default_route(const char *ifname);

/**
 * \brief check the ppp link status
 *
 * \param: ifname - name if interface "ppp0"
 * \param: max_retry - retry limit, 1 second retry cycle
 * \param: addr - returned ip address binary
 * \param: mask - returned netmask binary
 * \param: flags - returned flags binary
 *
 * \return: 
 * 0 : link up
 * -1: link not up
 */
static int poll_ppp_linkstate(const char *ifname, int max_retry,
    unsigned int *addr, unsigned int *mask, unsigned int *gway, unsigned int *flags)
{
    int i;
    int ret;
    int tmp;

    if (ifc_init())
    {
        LOGE("%s: ifc_init error\n", __FUNCTION__);
        return -1;
    }

    for(i = 0; i < max_retry; i++)
    {
        *flags = 0;
        if (ifc_get_info(ifname, addr, mask, flags))
        {
            LOGW("%s: ifc_get_info retry count=%d\n", __FUNCTION__, i);
            continue;
        }
        if(*flags & 0x1)
        {
            ret = 0;  // OK
            break;
        }
        sleep(1);
    }

    if (i >= max_retry) 
    {
        ret = -1;  // not up
	return ret;
    } 

    // now get default gateway.  returns zero on undefined
    tmp = ifc_get_default_route(ifname);
    *gway = tmp;

    return ret;
}

/**
 * \brief search process list by name and return PID if found
 *
 * \param: void
 *
 * \return: 
 * -1: process name not found
 * >0 : PID
 */
static int findPidByName(char* pidName)
{
    int pid = -1;
    DIR *dir;
    struct dirent *next;
    const int READ_BUF_SIZE = 512;

    dir = opendir("/proc");
    if (!dir) {
        LOGE("%s cannot open /proc\n", __FUNCTION__);
        return -1;
    }
    LOGD("findPidByName: finding %s", pidName);

    while ((next = readdir(dir)) != NULL) {
        FILE *status;
        char filename[READ_BUF_SIZE];
        char buffer[READ_BUF_SIZE];
        char name[READ_BUF_SIZE];

        /* Must skip ".." since that is outside /proc */
        if (strcmp(next->d_name, "..") == 0)
            continue;

        /* If it isn't a number, we don't want it */
        if (!isdigit(*next->d_name))
            continue;

        sprintf(filename, "/proc/%s/status", next->d_name);
        if (! (status = fopen(filename, "r")) ) {
            continue;
        }
        if (fgets(buffer, READ_BUF_SIZE-1, status) == NULL) {
            fclose(status);
            continue;
        }
        fclose(status);

        /* Buffer should contain a string like "Name:   binary_name" */
        sscanf(buffer, "%*s %s", name);
        if (strcmp(name, pidName) == 0) {
            pid = strtol(next->d_name, NULL, 0);
            LOGD("findPidByName: pppd pid is %d", pid);
            break;
        }
    }
    closedir(dir);

    return pid;
}

/** 
 * \brief find pppd PID and kill it
 *
 */
static void killPppd()
{
    pid_t pid;
    pid = findPidByName("pppd");
    if (pid > 0) {
        kill(pid, SIGTERM);
    }
}

/**
 * \brief requestDataCallList
 * handles RIL_REQUEST_DATA_CALL_LIST
 *
 * Queries the status of PDP contexts, returning for each
 * its CID, whether or not it is active, and its PDP type,
 * APN, and PDP adddress.
 * replaces RIL_REQUEST_PDP_CONTEXT_LIST
 *
 * "data" is NULL
 * "response" is an array of RIL_Data_Call_Response
 *
 * Valid errors:
 *  SUCCESS
 *  RADIO_NOT_AVAILABLE (radio resetting)
 *  GENERIC_FAILURE
 *
 * typedef struct {
 *   int             cid;         Context ID 
 *   int             active;      0=inactive, 1=active/physical link down, 2=active/physical link up 
 *   char *          type;        One of the PDP_type values in TS 27.007 section 10.1.1.
 *                                  For example, "IP", "IPV6", "IPV4V6", or "PPP". 
 *   char *          apn;
 *   char *          address;     The IPv4 or IPv6 address assigned to the call, e.g., "192.0.1.3"
 *                                  or "2001:db8::1". 
 * } RIL_Data_Call_Response;
 */
void requestDataCallList(void *data, size_t datalen, RIL_Token t)
{
    RIL_Data_Call_Response response;
    fw100SessionCtx_t *ctx = fw100GetSessionCtx();

    memset((void *) &response, 0, sizeof(response));
    if (ctx->inDataCall)
    {
        response.cid    = 1;
        response.active = 2;
        response.type   = "PPP";
        response.apn    = "internet";
        response.address = ctx->dataCallLocalIP;
    }
    else
    {
        response.cid    = 1;
        response.active = 0;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, (void *) &response, sizeof(response));

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * \brief packet data setup request
 * system/bin/pppd sets up the data call
 * requires /etc/ppp/peer scripts to be present
 * 
 * \brief SEE pppAautomatic.  called by RIL driver
 * automatically not dependent on Android stack 
 * RIL_REQUEST_SETUP_DATA_CALL
 *
 * "data" is a const char **
 * ((const char **)data)[0] indicates whether to setup connection on radio technology CDMA
 *                              or GSM/UMTS, 0-1. 0 - CDMA, 1-GSM/UMTS
 *
 * ((const char **)data)[1] is a RIL_DataProfile (support is optional)
 * ((const char **)data)[2] is the APN to connect to if radio technology is GSM/UMTS. This APN will
 *                          override the one in the profile. NULL indicates no APN overrride.
 * ((const char **)data)[3] is the username for APN, or NULL
 * ((const char **)data)[4] is the password for APN, or NULL
 * ((const char **)data)[5] is the PAP / CHAP auth type. Values:
 *                          0 => PAP and CHAP is never performed.
 *                          1 => PAP may be performed; CHAP is never performed.
 *                          2 => CHAP may be performed; PAP is never performed.
 *                          3 => PAP / CHAP may be performed - baseband dependent.
 * ((const char **)data)[6] is the PDP type to request if the radio technology is GSM/UMTS.
 *                          Must be one of the PDP_type values in TS 27.007 section 10.1.1.
 *                          For example, "IP", "IPV6", "IPV4V6", or "PPP".
 *
 * "response" is a char **
 * ((char **)response)[0] the Connection ID, CID, which is generated by RIL.
 * ((char **)response)[1] the network interface name.
 * ((char **)response)[2] a numeric IPv4 or IPv6 address that has been assigned to the interface.
 * ((char **)response)[3] a space-separated list of numeric IPv4 or IPv6 DNS addresses.
 *                        Ignored on Android platforms before 3.0 and instead two DNS IP addresses
 *                        are retrieved from system properties "net.$IN.dns1" and "net.$IN.dns2".
 * ((char **)response)[4] the numeric IPv4 or IPv6 address of the default gateway.
 *                        Ignored on Android platforms before 3.0 and instead an IP address
 *                        is retrieved from system property "net.$IN.gw".
 *
 * Notes:
 *   1) Numeric addresses must be in the Java InetAddress parsable represe
 */
void requestSetupDataCallEVDO(void *data, size_t datalen, RIL_Token t)
{
    int rc;
    char tmp[128];
    char dns[64];
    char gw[64];
    char *response[5];
    fw100SessionCtx_t *ctx = fw100GetSessionCtx();
    unsigned int addr;
    unsigned int gway;
    unsigned int mask;
    unsigned int flags;

    #if PLATFORM_X86
    sprintf(tmp,"/usr/sbin/pppd debug call %s", ctx->s_data_devname);
    #else
    sprintf(tmp,"/system/bin/pppd call %s", ctx->s_data_devname);
    #endif
    rc = system(tmp);
    if (rc < 0)  
    {
    	LOGD("%s:%d error %s %s", __FUNCTION__, __LINE__, strerror(errno), tmp);
        goto error;
    }
    LOGD("%s started %s", __FUNCTION__, tmp);
    
    // may take a long time to put up data call
    // allow 30 seconds retry
    rc = poll_ppp_linkstate("ppp0", 30, &addr, &mask, &gway, &flags);
    if (!rc) 
    {
        ctx->inDataCall = DATA_STATE_CONNECTED;
        
        memset(response, 0, sizeof(response));
        response[0] = "1";

        response[1] = "ppp0";

	inet_ntop(AF_INET, &addr, ctx->dataCallLocalIP, sizeof(ctx->dataCallLocalIP));
	inet_ntop(AF_INET, &gway, ctx->dataCallGateway, sizeof(ctx->dataCallGateway));

        response[2] = ctx->dataCallLocalIP;

	tmp[0]= 0;
        property_get("net.dns1", dns, "0");
	strncpy(tmp, dns, sizeof(tmp));
        property_get("net.dns2", dns, "0");
	strncat(tmp, " ", sizeof(tmp));
	strncat(tmp, dns, sizeof(tmp));
        response[3] = tmp;

	// this property doesn't work when set in /etc/ppp/ip-up-ppp
	// permission error
        // property_get("net.remote-ip", gw, "0");

        response[4] = ctx->dataCallGateway;

        LOGD("%s success cid=%s name=%s ip=%s dns=%s gw=%s", 
		__FUNCTION__, 
		response[0], 
		response[1], 
		response[2], 
		response[3], 
		response[4]);

        RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
        return;
    }

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * \brief tear down packet data call
 * kill pppd and ATH the modem
 *
 * FIXME - note below pppd is handled in UI
 * "data" is const char **
 * ((char**)data)[0] indicating CID
 * ((char**)data)[1] indicating Disconnect Reason
 *                   0 => No specific reason specified
 *                   1 => Radio shutdown requested
 *
 * "response" is NULL
 *
 */
void requestDeactivateDataCallEVDO(void *data, size_t datalen, RIL_Token t)
{
    char exitstatus[4] = {0};
    fw100SessionCtx_t *ctx = fw100GetSessionCtx();

    char *cid = ((char **)data)[0];
    if(cid == NULL)
    {
        LOGE("No target cid");
        /* goto error; */
    }

    at_send_command("ATH", NULL);
    sleep(2);

    killPppd();

    ctx->inDataCall = DATA_STATE_DISCONNECTED;
   
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    sleep(2);

    RIL_onUnsolicitedResponse ( RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED, 
      NULL, 0);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * \brief get data call fail cause
 * Requests the failure cause code for the most recently failed PDP
 * context or CDMA data connection active
 * replaces RIL_REQUEST_LAST_PDP_FAIL_CAUSE
 *
 * "data" is NULL
 *
 * "response" is a "int *"
 * ((int *)response)[0] is an integer cause code defined in TS 24.008
 *   section 6.1.3.1.3 or close approximation
 *
 * If the implementation does not have access to the exact cause codes,
 * then it should return one of the values listed in
 * RIL_LastDataCallActivateFailCause, as the UI layer needs to distinguish these
 * cases for error notification
 * and potential retries.
 *
 */
void requestDataCallFailCause(void *data, size_t datalen, RIL_Token t)
{
    int response = 0;

    // FIXME add detail test pppd exit status
    response = PDP_FAIL_ERROR_UNSPECIFIED;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));

    return;

}

/**
 * \brief ppp automatic.  called by RIL driver
 * automatically not dependent on Android stack 
 * RIL_REQUEST_SETUP_DATA_CALL
 * 
 * \return
 * 0 - pppd is running
 * -1  error
 *
 * init.rc experimental:
 * service ppp_start /system/bin/pppd call ttyUSB0
 *    user root
 *    group system radio cache inet misc vpn net_admin
 *    disabled
 */
int pppAutomatic()
{
    int rc;
    unsigned int addr;
    unsigned int mask;
    unsigned int flags;
    unsigned int gway;
    char tmp[128];
    RIL_Data_Call_Response response;
    fw100SessionCtx_t *ctx = fw100GetSessionCtx();

    memset((void *) &response, 0, sizeof(response));

    // check status of pppd
    // may take a long time to put up data call
    // allow 30 seconds retry
    rc = poll_ppp_linkstate("ppp0", 30, &addr, &mask, &gway, &flags);
    if (!rc) 
    {
        // link is up, changed state?
        if (ctx->inDataCall != DATA_STATE_CONNECTED)
        {
        	ctx->inDataCall = DATA_STATE_CONNECTED;
	        inet_ntop(AF_INET, &addr, ctx->dataCallLocalIP, sizeof(ctx->dataCallLocalIP));
                response.cid     = 1;
                response.active  = 2;
                response.type    = "IP";
                response.apn     = ctx->carrier;
                response.address = ctx->dataCallLocalIP;
                RIL_onUnsolicitedResponse (RIL_UNSOL_DATA_CALL_LIST_CHANGED, 
                  (void *) &response, sizeof(response));
        
                LOGD("%s ppp0 is UP ipaddr=%s", __FUNCTION__, ctx->dataCallLocalIP);
                // update ril status 
                rilWriteStatus(ctx, RIL_STATUS_FILEPATH);
        }

        return 0;
    }

    // link is down.  changed state?
    if (ctx->inDataCall == DATA_STATE_CONNECTED)
    {
        ctx->inDataCall = DATA_STATE_DISCONNECTED;
        memset(ctx->dataCallLocalIP, 0, sizeof(ctx->dataCallLocalIP));
        response.cid    = 1;
        response.active = 0;
                RIL_onUnsolicitedResponse (RIL_UNSOL_DATA_CALL_LIST_CHANGED, 
                  (void *) &response, sizeof(response));

        LOGD("%s ppp0 is DOWN", __FUNCTION__);
        // update ril status 
        rilWriteStatus(ctx, RIL_STATUS_FILEPATH);
    }

    #if 0
    // experimental.  seems to automatically restart on 
    // error which is perhaps not what we want.
    rc = property_set("ctl.start", "ppp_start");
    #endif

    #if PLATFORM_X86
    sprintf(tmp,"/usr/sbin/pppd debug call %s", ctx->s_data_devname);
    #else
    sprintf(tmp,"/system/bin/pppd call %s", ctx->s_data_devname);
    #endif
    rc = system(tmp);
    if (rc < 0)  
    {
    	LOGD("%s:%d error %s %s", __FUNCTION__, __LINE__, strerror(errno), tmp);
        goto error;
    }
    LOGD("%s started %s", __FUNCTION__, tmp);
    
    return 0;

error:
    return - 1;
    
}


