/** 
 * \file fw100-ril-gps.c 
 * \brief GPS handling and psuedo-tty functions
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
 * \brief write GPS output to fifo
 * write to named pipe or file, non-blocking
 *
 * \param ctx - ril driver session context
 * \param file - name of file or pipe to write
 * \param gpsinfo - GPS NMEA fix ASCII string 
 *
 * \return 
 * 0 write to file OK
 * -1 error writing fifo
 * 
 * \brief n.b.  The fifo is opened for write in non-blocking mode. 
 * In this case, opening for read only will succeed even if noone has 
 * opened on the write side yet; opening for write only will fail with 
 * ENXIO (no such device or address) unless the other end has already 
 * been opened. 
 */
int rilWriteGPSFifo(fw100SessionCtx_t *ctx, const char *file, const char *gpsinfo)
{
    int rc = 0;
    int numw;
    char *result;

    // if closed mkfifo and open 
    // will warn if fifo exists 
    if (ctx->gpsFifoFD <= 0)
    {
	// try to make the fifo for first time use.
	// it persists across reboot
	rc = mkfifo(file, 0666);
	ctx->gpsFifoFD = open(file, O_WRONLY | O_NONBLOCK);
        LOGD("%s:%d mkfifo %s rc=%d open fd=%d %s", 
	__FUNCTION__, __LINE__, file, rc, ctx->gpsFifoFD, strerror(errno));
	if (ctx->gpsFifoFD <= 0) goto done;
    }

    numw = write(ctx->gpsFifoFD, gpsinfo, strlen(gpsinfo));
    write(ctx->gpsFifoFD, "\n", 1);
    if (numw < 0) 
    {
        LOGD("%s:%d error %s numw=%d %s", 
		__FUNCTION__, __LINE__, file, numw, strerror(errno));
    	close(ctx->gpsFifoFD);	// close on error to force reopen
    	ctx->gpsFifoFD = 0;	
	rc = -1;
    }

done:
    return rc;
}

/**
 * \brief write GPS output to pseudo-tty.
 * this function opens pseudo-terminal master
 * and enumerates the device name with system
 * property_set(...).  The property has a well-known
 * name defined at build time and can be discovered 
 * by tty slave.
 *
 * \param ctx - ril driver session context
 * \param gpsinfo - GPS NMEA fix ASCII string 
 * \param options - asserted for one-time create ptty
 * normally zero to write fix.
 *
 * \return 
 * 0 write to file OK
 * -1 error writing tty
 * 
 * \brief n.b. if the port open fails this will retry
 * each time a GPS fix is output.
 */
int rilWriteGPSTty(fw100SessionCtx_t *ctx, const char *gpsinfo, int options)
{
    int mfd;
    int rc = 0;
    int numw;
    char *result;
    char device[128];

    // one-time open
    if (ctx->gpsTtyFD <= 0)
    {
	mfd = getpt();
	if (mfd == -1) 
	{
		LOGD("%s cannot get ptmx %s", __FUNCTION__, strerror(errno));
		goto done;
	}
	rc = grantpt(mfd);
	if (rc  == -1) 
	{
		LOGD("%s cannot grant ptmx %s", __FUNCTION__, strerror(errno));
		goto done;
	}
	rc = unlockpt(mfd);
	if (rc == -1) 
	{
		LOGD("%s cannot unlock ptmx %s", __FUNCTION__, strerror(errno));
		goto done;
	}

	// change permission on the device so any program can read it
	rc = ptsname_r(mfd, device, sizeof(device));
	rc = chmod(device, 0666);
	if (rc)
	{
		LOGD("%s:%d error chmod devname %s err=%d %s\n", 
		__FUNCTION__, __LINE__, device, errno, strerror(errno));
	}

	rc = property_set(GPS_PROPERTY_NAME, device);
	ctx->gpsTtyFD = mfd;
        LOGD("%s:%d open ptmx rc=%d open fd=%d name=%s", 
	__FUNCTION__, __LINE__, rc, ctx->gpsTtyFD, device);

	// if option is asserted then one-time open is complete
	if (options) goto done;
    }

    numw = write(ctx->gpsTtyFD, gpsinfo, strlen(gpsinfo));
    write(ctx->gpsTtyFD, "\n", 1);
    if (numw < 0) 
    {
        LOGD("%s:%d error numw=%d %s", 
		__FUNCTION__, __LINE__, numw, strerror(errno));
    	close(ctx->gpsTtyFD);	// close on error to force reopen
    	ctx->gpsTtyFD = 0;	
	rc = -1;
    }

done:
    return rc;
}

