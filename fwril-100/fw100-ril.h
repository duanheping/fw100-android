/** 
 * \file fw100-ril.h 
 * \brief fw100 ril driver header file
 *
 */

#ifndef _fw100_ril_h_included
#define _fw100_ril_h_included

extern const struct RIL_Env *s_rilenv;
#define RIL_onRequestComplete(t, e, response, responselen) \
  s_rilenv->OnRequestComplete(t,e, response, responselen)
#define RIL_onUnsolicitedResponse(a,b,c) s_rilenv->OnUnsolicitedResponse(a,b,c)
#define RIL_requestTimedCallback(a,b,c) s_rilenv->RequestTimedCallback(a,b,c)

// registration response
#define REGISTRATION_EVDO_REV_A "8"
#define REGISTRATION_CDMA_1XTT "6"
#define REGISTRATION_NETWORK_UNKNOWN "0"
#define DEFAULT_ROAMING_INDICATOR "1"
#define REGISTRATION_DEFAULT_VALUE "0"
#define REGISTRATION_DEFAULT_PRL_VALUE "1"
typedef struct {
        char *register_state;        // 0
        char *lac;                   // 1
        char *cid;                   // 2
        char *radio_technology;      // 3
        char *basestation_id;        // 4
        char *basestation_latitude;  // 5
        char *basestation_longitude; // 6
        char *concurrent_service;    // 7
        char *system_id;             // 8
        char *network_id;            // 9
        char *roaming_indicator;     // 10
        char *prl_state;             // 11
        char *prl_roaming_indicator; // 12
        char *deny_reason;           // 13
        char *pri_scramble_code;     // 14
} RIL_Registration_response;

// preferred network enumeration
typedef enum {
    NETWORK_MODE_WCDMA_PREF     = 0, /* GSM/WCDMA (WCDMA preferred) */
    NETWORK_MODE_GSM_ONLY       = 1, /* GSM only */
    NETWORK_MODE_WCDMA_ONLY     = 2, /* WCDMA only */
    NETWORK_MODE_GSM_UMTS       = 3, /* GSM/WCDMA (auto mode, according to PRL)
                                     AVAILABLE Application Settings menu*/
    NETWORK_MODE_HYBRID           = 4, /* CDMA and EvDo (auto mode, according to PRL)
                                      AVAILABLE Application Settings menu*/
    NETWORK_MODE_CDMA_NO_EVDO   = 5, /* CDMA only */
    NETWORK_MODE_EVDO_NO_CDMA   = 6, /* EvDo only */
    NETWORK_MODE_GLOBAL         = 7, /* GSM/WCDMA, CDMA, and EvDo (auto mode, according to PRL)
                                        AVAILABLE Application Settings menu*/
} Network_mode;

// signal strength related
#define SIGNAL_STRENGTH_DEFAULT 0
#define EVDO_ECIO_DEFAULT 750
#define EVDO_SNR_DEFAULT 8
#define CDMA_ECIO_DEFAULT 90

#define SCREEN_IS_OFF   0
#define SCREEN_IS_ON    1

#define DATA_STATE_DISCONNECTED 0
#define DATA_STATE_CONNECTED    1

// max attempts to auto activate
// clear by system restart
#define MAX_AUTO_ACTIVATE_RETRY	6

// GPS related build options
#define GPS_USE_FIFO	0
#define GPS_USE_TTY	1
#define GPS_TEST_NMEA	1
#define GPS_RATE_1PPS	1
#define GPS_RATE_10PPS	0
#define GPS_PROPERTY_NAME "ril.gps.port"

// fw100 session context
typedef struct
{
  int mainIsStarted;    // asserted when mainloop is running

  const char *s_atctrl_path;
  const char *s_data_path;     // example /dev/ttyUSB0
  const char *s_data_devname;  // node name only for ppp example ttyUSB0
  int         s_device_socket;
 
  pthread_t s_tid_mainloop;
  pthread_mutex_t s_state_mutex;
  pthread_cond_t s_state_cond;

  RIL_RadioState sState;
 
  int s_port;
 
  /* trigger change to this with s_state_cond */
  int s_closed;
 
  // fw100 network mode seems stuck in manual mode
  // auto is 0, manual is 1
  int networkSelectMode;
  
  struct timeval TIMEVAL_SIMPOLL;
  struct timeval TIMEVAL_CALLSTATEPOLL;
  struct timeval TIMEVAL_0;

  int screenState;  

  // data call management
  int inDataCall;
  int dataCallIsAutomatic;
  char dataCallLocalIP[64];
  char dataCallGateway[64];

  // GPS
  int gpsFifoEnable;
  int gpsFifoFD;
  int gpsTtyEnable;
  int gpsTtyFD;

  // carrier and activation status
  char meid[32];
  char mdn[32];
  char carrier[16];
  int  otaMsgCnt;
  char otaMsg[64];

  int  moduleAutoActivate;
  int  moduleIsActivated;
  int  moduleActivateRetry;

} fw100SessionCtx_t;

// path to control and status files
#define RIL_CONTROL_FILEPATH "/opt/fusion/fwril-control.txt"
#define RIL_STATUS_FILEPATH "/opt/fusion/fwril-status.txt"
#define RIL_GPS_FIFOPATH "/opt/fusion/gpsfifo"

// helpers
extern const char *requestToString(int request);
extern fw100SessionCtx_t *fw100GetSessionCtx(void);
extern int isRadioOn(void);

// utility functions
int rilReadControl(fw100SessionCtx_t *ctx, const char *file);
int rilWriteStatus(fw100SessionCtx_t *ctx, const char *file);
int rilWriteGPS(fw100SessionCtx_t *ctx, const char *file, const char *gpsinfo);
char *strToUpper(char *p, int max);

// GPS
int rilWriteGPSFifo(fw100SessionCtx_t *ctx, const char *file, const char *gpsinfo);
int rilWriteGPSTty(fw100SessionCtx_t *ctx, const char *gpsinfo, int options);

// fw100 request handlers
extern void requestBasebandVersion(void *data, size_t datalen, RIL_Token t);
extern void requestGetIMEI(void *data, size_t datalen, RIL_Token t);
extern void requestGetIMSI(void *data, size_t datalen, RIL_Token t);
extern void requestOperator(void *data, size_t datalen, RIL_Token t);
extern void requestRegistrationStateEVDO(int request, void *data, size_t datalen, RIL_Token t);
extern void requestRegistrationStateGPRS(int request, void *data, size_t datalen, RIL_Token t);
extern void requestGetPreferredNetworkType (void *data, size_t datalen, RIL_Token t);
extern void requestSetPreferredNetworkType (void *data, size_t datalen, RIL_Token t);
extern void requestSignalStrengthEVDO(void *data, size_t datalen, RIL_Token t);
extern void requestQueryAvailableNetworks(void *data, size_t datalen, RIL_Token t);
extern void requestScreenState(void *data, size_t datalen, RIL_Token t);
extern void requestSetupDataCallEVDO(void *data, size_t datalen, RIL_Token t);
extern void requestDeactivateDataCallEVDO(void *data, size_t datalen, RIL_Token t);
extern void requestSendSMSEVDO(void *data, size_t datalen, RIL_Token t);
extern void requestGetCurrentCallsEVDO(void *data, size_t datalen, RIL_Token t);
extern void requestDataCallList(void *data, size_t datalen, RIL_Token t);
extern void requestDataCallFailCause(void *data, size_t datalen, RIL_Token t);

// timer processing
extern void fw100ModemTimer(void);
extern int  pppAutomatic(void);

// activation
int activateHelper(int options);

#endif  // _fw100_ril_h_included

