/*
 *  Copyright (C) 2022 Andri Yngvason.  All Rights Reserved.
 *  Copyright (C) 2000-2002 Constantin Kaplinsky.  All Rights Reserved.
 *  Copyright (C) 2000 Tridia Corporation.  All Rights Reserved.
 *  Copyright (C) 1999 AT&T Laboratories Cambridge.  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

#include <rfb/rfbclient.h>
#include <assert.h>

rfbBool vnc_client_set_format_and_encodings(rfbClient* client)
{
	assert(client->appData.encodingsString);

	rfbSetPixelFormatMsg spf;
	union {
		char bytes[sz_rfbSetEncodingsMsg + MAX_ENCODINGS*4];
		rfbSetEncodingsMsg msg;
	} buf;

	rfbSetEncodingsMsg *se = &buf.msg;
	uint32_t *encs = (uint32_t *)(&buf.bytes[sz_rfbSetEncodingsMsg]);
	int len = 0;
	rfbBool requestCompressLevel = FALSE;
	rfbBool requestQualityLevel = FALSE;
	rfbBool requestLastRectEncoding = FALSE;

	if (!SupportsClient2Server(client, rfbSetPixelFormat))
		return TRUE;

	spf.type = rfbSetPixelFormat;
	spf.pad1 = 0;
	spf.pad2 = 0;
	spf.format = client->format;
	spf.format.redMax = rfbClientSwap16IfLE(spf.format.redMax);
	spf.format.greenMax = rfbClientSwap16IfLE(spf.format.greenMax);
	spf.format.blueMax = rfbClientSwap16IfLE(spf.format.blueMax);

	if (!WriteToRFBServer(client, (char *)&spf, sz_rfbSetPixelFormatMsg))
		return FALSE;


	if (!SupportsClient2Server(client, rfbSetEncodings))
		return TRUE;

	se->type = rfbSetEncodings;
	se->pad = 0;
	se->nEncodings = 0;

	const char *encStr = client->appData.encodingsString;
	int encStrLen;
	do {
		const char *nextEncStr = strchr(encStr, ',');
		if (nextEncStr) {
			encStrLen = nextEncStr - encStr;
			nextEncStr++;
		} else {
			encStrLen = strlen(encStr);
		}

		if (strncasecmp(encStr,"raw",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingRaw);
		} else if (strncasecmp(encStr,"copyrect",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingCopyRect);
#ifdef LIBVNCSERVER_HAVE_LIBZ
#ifdef LIBVNCSERVER_HAVE_LIBJPEG
		} else if (strncasecmp(encStr,"tight",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingTight);
			requestLastRectEncoding = TRUE;
			if (client->appData.compressLevel >= 0 && client->appData.compressLevel <= 9)
				requestCompressLevel = TRUE;
			if (client->appData.enableJPEG)
				requestQualityLevel = TRUE;
#endif
#endif
		} else if (strncasecmp(encStr,"hextile",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingHextile);
#ifdef LIBVNCSERVER_HAVE_LIBZ
		} else if (strncasecmp(encStr,"zlib",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingZlib);
			if (client->appData.compressLevel >= 0 && client->appData.compressLevel <= 9)
				requestCompressLevel = TRUE;
		} else if (strncasecmp(encStr,"zlibhex",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingZlibHex);
			if (client->appData.compressLevel >= 0 && client->appData.compressLevel <= 9)
				requestCompressLevel = TRUE;
		} else if (strncasecmp(encStr,"trle",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingTRLE);
		} else if (strncasecmp(encStr,"zrle",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingZRLE);
		} else if (strncasecmp(encStr,"zywrle",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingZYWRLE);
			requestQualityLevel = TRUE;
#endif
		} else if ((strncasecmp(encStr,"ultra",encStrLen) == 0) || (strncasecmp(encStr,"ultrazip",encStrLen) == 0)) {
			/* There are 2 encodings used in 'ultra' */
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingUltra);
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingUltraZip);
		} else if (strncasecmp(encStr,"corre",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingCoRRE);
		} else if (strncasecmp(encStr,"rre",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingRRE);
		} else if (strncasecmp(encStr,"open-h264",encStrLen) == 0) {
			encs[se->nEncodings++] = rfbClientSwap32IfLE(50);
		} else {
			rfbClientLog("Unknown encoding '%.*s'\n",encStrLen,encStr);
		}

		encStr = nextEncStr;
	} while (encStr && se->nEncodings < MAX_ENCODINGS);

	if (se->nEncodings < MAX_ENCODINGS && requestCompressLevel) {
		encs[se->nEncodings++] = rfbClientSwap32IfLE(client->appData.compressLevel +
				rfbEncodingCompressLevel0);
	}

	if (se->nEncodings < MAX_ENCODINGS && requestQualityLevel) {
		if (client->appData.qualityLevel < 0 || client->appData.qualityLevel > 9)
			client->appData.qualityLevel = 5;
		encs[se->nEncodings++] = rfbClientSwap32IfLE(client->appData.qualityLevel +
				rfbEncodingQualityLevel0);
	}

	/* Remote Cursor Support (local to viewer) */
	if (client->appData.useRemoteCursor) {
		if (se->nEncodings < MAX_ENCODINGS)
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingXCursor);
		if (se->nEncodings < MAX_ENCODINGS)
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingRichCursor);
		if (se->nEncodings < MAX_ENCODINGS)
			encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingPointerPos);
	}

	/* Keyboard State Encodings */
	if (se->nEncodings < MAX_ENCODINGS)
		encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingKeyboardLedState);

	/* New Frame Buffer Size */
	if (se->nEncodings < MAX_ENCODINGS && client->canHandleNewFBSize)
		encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingNewFBSize);

	/* Last Rect */
	if (se->nEncodings < MAX_ENCODINGS && requestLastRectEncoding)
		encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingLastRect);

	/* Server Capabilities */
	if (se->nEncodings < MAX_ENCODINGS)
		encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingSupportedMessages);
	if (se->nEncodings < MAX_ENCODINGS)
		encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingSupportedEncodings);
	if (se->nEncodings < MAX_ENCODINGS)
		encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingServerIdentity);

	/* xvp */
	if (se->nEncodings < MAX_ENCODINGS)
		encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingXvp);

	if (se->nEncodings < MAX_ENCODINGS)
		encs[se->nEncodings++] = rfbClientSwap32IfLE(rfbEncodingQemuExtendedKeyEvent);

	len = sz_rfbSetEncodingsMsg + se->nEncodings * 4;

	se->nEncodings = rfbClientSwap16IfLE(se->nEncodings);

	return WriteToRFBServer(client, buf.bytes, len);
}
