/** 
 * \file fw100-ril-utils.c 
 * \brief module activator and utility functions
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

/**
 * \brief in-place upper case string.  
 * enforces a limit on max length of string
 *
 * \param p - pointer to string to upcase
 * \param max - max string length
 *
 * \return
 * pointer to upcase string
 *
 */
char *strToUpper(char *p, int max)
{
    int len = 0;
    char *ret = p;

    // upcase string
    len = 0;
    while (*p)
    {
        *p = toupper(*p);
        p++;
        // sanity check limit
        if (++len > max) break;
    }
    return ret;
}

/**
 * \brief activation helper
 *
 * \param options - assert to enable automatic activation
 *
 * \return 
 * 0 OK module is activated
 * -1 helper error
 * -2 module is not activated
 * -3 activation try but module could not be activated
 * 
 * \brief n.b. module MDN is checked. If not valid then 
 * module has not been activated. 
 * If option is asserted, this will make OTASP call for 
 * Verizon provisioned module.
 */
int activateHelper(int options)
{
    int err;
    int ret = -1;
    int  display;
    int  retry = 0;
    char *result;
    char *line;
    fw100SessionCtx_t *ctx = fw100GetSessionCtx();
    ATResponse *meid  = NULL;
    ATResponse *mdn  = NULL;
    ATResponse *carrier = NULL;
    ATResponse *otasp   = NULL;
    ATResponse *verify = NULL;

    // save meid
    err = at_send_command_singleline("AT^MEID","^MEID:",&meid);
    if (err != 0 || meid->success == 0) {
        goto done;
    }
    line = meid->p_intermediates->line;
    strncpy(ctx->meid, (line+6), sizeof(ctx->meid));
    strToUpper(ctx->meid, strlen(ctx->meid));

    // get mdn
    err = at_send_command_singleline("AT+VMDN?","+VMDN:",&mdn);
    if (err != 0 || mdn->success == 0) {
        goto done;
    }

    line = mdn->p_intermediates->line;
    if ((*(line+6) == '0') && (*(line+7) == '0') && (*(line+8) == '0'))
    {
	ret = -2;
    }
    else
    {
        // module has valid MDN
        ctx->moduleIsActivated = 1;
        strncpy(ctx->mdn, (line+6), sizeof(ctx->mdn));	// save mdn
        ret = 0;
        goto done;
    }
 
    err = at_send_command_singleline("AT$QCMIPGETP=0","",&carrier);
    if (err != 0 || carrier->success == 0) {
        goto done;
    }

    line = carrier->p_intermediates->line;

    // sprint is auto provisioned does not support OTASP
    if (strstr(line, "sprint")) 
    {
        goto done;
    }

    // OTASP call
    else if (strstr(line, "vzw")) 
    {
        if (!ctx->moduleAutoActivate)
        {
            goto done;
        }
        
        // limit max auto-activate retries
        if (ctx->moduleActivateRetry) ctx->moduleActivateRetry--;
        if (!ctx->moduleActivateRetry)
        {
            ctx->moduleIsActivated = 2; // failed
            goto done;
        }

        ctx->otaMsgCnt = 0;
        err = at_send_command_singleline("AT+CDV=*22899","OK",&otasp);
        // ignore err -6 AT_ERROR_INVALID_RESPONSE and otasp NULL

        // wait on OTA call complete
        do 
        {
           sleep(1);
        } while ((ctx->otaMsgCnt < 5) && (retry++ < 30));

        ret = -2;
        if (strstr(ctx->otaMsg, "MSG:8")) 
        { 
            ctx->moduleIsActivated = 1;
            // confirm activated MDN
            err = at_send_command_singleline("AT+VMDN?","+VMDN:",&verify);
            if (err != 0 || verify->success == 0) {
                goto done;
            }
            line = verify->p_intermediates->line;
            strncpy(ctx->mdn, (line+6), sizeof(ctx->mdn));	// save mdn
            ret = 0;	// success
        }
        goto done;
    }

    // unknown carrier
    else
    {
        goto done;
    }

done:
    if (NULL != meid)    at_response_free(meid);
    if (NULL != mdn)     at_response_free(mdn);
    if (NULL != carrier) at_response_free(carrier);
    if (NULL != otasp)   at_response_free(otasp);
    if (NULL != verify)  at_response_free(verify);

    result = (ctx->moduleIsActivated) ? "Module Is Activated" : "Module Is NOT Activated";
    LOGD("%s auto=%d retry=%d active=%d %s", __FUNCTION__, 
         ctx->moduleAutoActivate, ctx->moduleActivateRetry, ctx->moduleIsActivated, result);

    // update ril status 
    rilWriteStatus(ctx, RIL_STATUS_FILEPATH);

    return ret;
}

/**
 * \brief read ril control 
 *
 * \param ctx - ril driver session context
 * \param file - name of file to read
 *
 * \return 
 * 0 read file OK
 * -1 error reading file
 * 
 */
int rilReadControl(fw100SessionCtx_t *ctx, const char *file)
{
    FILE *f;     
    char *p;
    char *result;
    int option1;
    int option2;
    int ret = -1;
    char buf[128];

    f = fopen(file, "rb");
    if (NULL == f) goto done;

    // read preferences 
    do 
    {
         p = fgets(buf, sizeof(buf), f);
         if (NULL == p) continue;

         //  simple yes no parsing
         if (strstr(buf, "DataCallIsAutomatic"))
         {
            // make it case insensitive
            option1 = (strstr(buf, "Yes")) ? 1 : 0;
            option2 = (strstr(buf, "yes")) ? 1 : 0;
            ctx->dataCallIsAutomatic = option1 + option2;
         }

         //  simple yes no parsing
         if (strstr(buf, "AutoActivate"))
         {
            // make it case insensitive
            option1 = (strstr(buf, "Yes")) ? 1 : 0;
            option2 = (strstr(buf, "yes")) ? 1 : 0;
            ctx->moduleAutoActivate = option1 + option2;
         }

    } while (NULL != p);

    fclose(f);
    ret = 0;

done:
    result = ret ? "Fail" : "OK";
    LOGD("%s %s result=%d %s", __FUNCTION__, file, ret, result);

    return ret;
}

/**
 * \brief write ril status 
 *
 * \param ctx - ril driver session context
 * \param file - name of file to write
 *
 * \return 
 * 0 write to file OK
 * -1 error writing file
 * 
 */
int rilWriteStatus(fw100SessionCtx_t *ctx, const char *file)
{
    int rc;
    FILE *f;     
    char *p;
    char *state;
    char *result;
    int ret = -1;
    char tmp[128];

    f = fopen(file, "wb");
    if (NULL == f) goto done;

    fprintf(f, "MEID=%s\n",    ctx->meid);
    fprintf(f, "MDN=%s\n",     ctx->mdn);
    fprintf(f, "Carrier=%s\n", ctx->carrier);

    if (ctx->moduleIsActivated == 0) state = "No";
    else if (ctx->moduleIsActivated == 1) state = "Yes";
    else state = "Fail";
    fprintf(f, "ModuleIsActivated=%s\n", state);

    state = (ctx->dataCallIsAutomatic) ? "Yes" : "No";
    fprintf(f, "DataCallIsAutomatic=%s\n", state);

    if (ctx->inDataCall)
    {
        fprintf(f, "InDataCall=Yes\n");
        fprintf(f, "LocalIP=%s\n", ctx->dataCallLocalIP);
    }
    else
    {  
        fprintf(f, "InDataCall=No\n");
    }

    fclose(f);
    ret = 0;

    // open up read permissions android fopen restricts 
    sprintf(tmp, "chmod 666 %s", file);
    rc = system(tmp); 
    if (rc < 0) 
    {
        LOGD("%s:%d error %s %s", __FUNCTION__, __LINE__, strerror(errno), tmp);
    }

done:
    result = ret ? "Fail" : "OK";
    LOGD("%s %s result=%d %s", __FUNCTION__, file, ret, result);

    return ret;
}

