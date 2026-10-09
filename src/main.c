/*
 *  __MODULE__	main.c
 *  __IDENT__	V13-000
 *  __REV__	13.0
 *
 *  Abstract:	Entry point of napi-set-mac, see napi_mac.c for usage.
 *
 *  Modification history:
 *	13.0	09-OCT-2026	C port of napi-set-mac v12.1.
 */

#include "napi_mac.h"

int main(int argc, char **argv)
{
	int rc = napi_main(argc, argv);

	/* udev logs every non-zero RUN exit as "failed"; only real errors should be. */
	if (g_in_udev && rc != EXIT_ERROR)
		rc = EXIT_OK;
	return rc;
}
