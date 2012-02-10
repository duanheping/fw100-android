/**
 * 
 * \file rilinfo.h
 * \brief ril request and status profiles include file
 */

#ifndef _rilinfo_h_included
#define _rilinfo_h_included

#define MAX_RRI_NAME	64

// range of product profiles
#define RRI_PROFILE_1	1 // data feature
#define RRI_PROFILE_2	2 // voice+data feature
#define RRI_PROFILE_NS	9 // feature not supported

// this product profile
#define RRI_MY_PROFILE  RRI_PROFILE_1

// highest RIL request in rilinfo.c ril_request_info table
#define RRI_RQST_MAX RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE

typedef struct ril_request_info
{
	int id;
	int profile;
	const char name[MAX_RRI_NAME+1];
} ril_request_info_t;

const char *requestToString(int request);
const ril_request_info_t *requestInfo(int request);

#endif // _rilinfo_h_included

