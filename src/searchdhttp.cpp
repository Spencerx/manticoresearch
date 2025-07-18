//
// Copyright (c) 2017-2025, Manticore Software LTD (https://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "searchdhttp.h"
#include "jsonqueryfilter.h"
#include "attribute.h"
#include "sphinxpq.h"

#include "http/http_parser.h"
#include "searchdaemon.h"
#include "searchdha.h"
#include "searchdreplication.h"
#include "accumulator.h"
#include "networking_daemon.h"
#include "client_session.h"
#include "tracer.h"
#include "searchdbuddy.h"
#include "compressed_http.h"
#include "daemon/logger.h"
#include "daemon/search_handler.h"
#include "sphinxquery/xqparser.h"

static bool g_bLogBadHttpReq = val_from_env ( "MANTICORE_LOG_HTTP_BAD_REQ", false ); // log content of bad http requests, ruled by this env variable
static int g_iLogHttpData = val_from_env ( "MANTICORE_LOG_HTTP_DATA", 0 ); // verbose logging of http data, ruled by this env variable

static bool LOG_LEVEL_HTTP = val_from_env ( "MANTICORE_LOG_HTTP", false ); // verbose logging http processing events, ruled by this env variable
#define LOG_COMPONENT_HTTP ""
#define HTTPINFO LOGMSG ( VERBOSE_DEBUG, HTTP, HTTP )

extern CSphString g_sStatusVersion;

static const Str_t g_sDataDisabled = FROMS("-");
Str_t Data2Log ( Str_t tMsg ) { return ( g_iLogHttpData ? Str_t ( tMsg.first, Min ( tMsg.second, g_iLogHttpData ) ) : g_sDataDisabled ); }
Str_t Data2Log ( ByteBlob_t tMsg ) { return ( g_iLogHttpData ? Str_t ( (const char *)tMsg.first, Min ( tMsg.second, g_iLogHttpData ) ) : g_sDataDisabled ); }

int HttpGetStatusCodes ( EHTTP_STATUS eStatus ) noexcept
{
	switch ( eStatus )
	{
	case EHTTP_STATUS::_100: return 100;
	case EHTTP_STATUS::_200: return 200;
	case EHTTP_STATUS::_206: return 206;
	case EHTTP_STATUS::_400: return 400;
	case EHTTP_STATUS::_403: return 403;
	case EHTTP_STATUS::_404: return 404;
	case EHTTP_STATUS::_405: return 405;
	case EHTTP_STATUS::_409: return 409;
	case EHTTP_STATUS::_413: return 413;
	case EHTTP_STATUS::_415: return 415;
	case EHTTP_STATUS::_500: return 500;
	case EHTTP_STATUS::_501: return 501;
	case EHTTP_STATUS::_503: return 503;
	case EHTTP_STATUS::_526: return 526;

	default: return 503;
	};
};

EHTTP_STATUS HttpGetStatusCodes ( int iStatus ) noexcept
{
	switch ( iStatus )
	{
	case 100: return EHTTP_STATUS::_100;
	case 200: return EHTTP_STATUS::_200;
	case 206: return EHTTP_STATUS::_206;
	case 400: return EHTTP_STATUS::_400;
	case 403: return EHTTP_STATUS::_403;
	case 404: return EHTTP_STATUS::_404;
	case 405: return EHTTP_STATUS::_405;
	case 409: return EHTTP_STATUS::_409;
	case 413: return EHTTP_STATUS::_413;
	case 415: return EHTTP_STATUS::_415;
	case 500: return EHTTP_STATUS::_500;
	case 501: return EHTTP_STATUS::_501;
	case 503: return EHTTP_STATUS::_503;
	case 526: return EHTTP_STATUS::_526;

	default: return EHTTP_STATUS::_503;
	};
}

inline constexpr const char* HttpGetStatusName ( EHTTP_STATUS eStatus ) noexcept
{
	switch ( eStatus )
	{
	case EHTTP_STATUS::_100: return "100 Continue";
	case EHTTP_STATUS::_200: return "200 OK";
	case EHTTP_STATUS::_206: return "206 Partial Content";
	case EHTTP_STATUS::_400: return "400 Bad Request";
	case EHTTP_STATUS::_403: return "403 Forbidden";
	case EHTTP_STATUS::_404: return "404 Not Found";
	case EHTTP_STATUS::_405: return "405 Method Not Allowed";
	case EHTTP_STATUS::_409: return "409 Conflict";
	case EHTTP_STATUS::_413: return "413 Request Entity Too Large";
	case EHTTP_STATUS::_415: return "415 Unsupported Media Type";
	case EHTTP_STATUS::_500: return "500 Internal Server Error";
	case EHTTP_STATUS::_501: return "501 Not Implemented";
	case EHTTP_STATUS::_503: return "503 Service Unavailable";
	case EHTTP_STATUS::_526: return "526 Invalid SSL Certificate";

	default: return "503 Service Unavailable";
	};
}

extern CSphString g_sMySQLVersion;

static void HttpBuildReply ( CSphVector<BYTE> & dData, EHTTP_STATUS eCode, const char * sBody, int iBodyLen, bool bHtml, bool bHeadReply )
{
	assert ( sBody && iBodyLen );

	const char * sContent = ( bHtml ? "text/html" : "application/json" );
	CSphString sHttp;
	sHttp.SetSprintf ( "HTTP/1.1 %s\r\nServer: %s\r\nContent-Type: %s; charset=UTF-8\r\nContent-Length:%d\r\n\r\n", HttpGetStatusName(eCode), g_sMySQLVersion.cstr(), sContent, iBodyLen );

	int iHeaderLen = sHttp.Length();
	int iBufLen = iHeaderLen;
	if ( !bHeadReply )
		iBufLen += iBodyLen;
	dData.Resize ( iBufLen );
	memcpy ( dData.Begin(), sHttp.cstr(), iHeaderLen );
	if ( !bHeadReply )
		memcpy ( dData.Begin() + iHeaderLen, sBody, iBodyLen );
}

void HttpBuildReply ( CSphVector<BYTE> & dData, EHTTP_STATUS eCode, const char * sBody, int iBodyLen, bool bHtml )
{
	HttpBuildReply ( dData, eCode, sBody, iBodyLen, bHtml, false );
}

void HttpBuildReplyHead ( CSphVector<BYTE> & dData, EHTTP_STATUS eCode, const char * sBody, int iBodyLen, bool bHeadReply )
{
	HttpBuildReply ( dData, eCode, sBody, iBodyLen, false, bHeadReply );
}


void HttpErrorReply ( CSphVector<BYTE> & dData, EHTTP_STATUS eCode, const char * szError )
{
	JsonObj_c tErr;
	tErr.AddStr ( "error", szError );
	CSphString sJsonError = tErr.AsString();
	HttpBuildReply ( dData, eCode, sJsonError.cstr(), sJsonError.Length(), false );
}

struct Endpoint_t
{
	const char* m_szName1;
	const char* m_szName2;
};

static Endpoint_t g_dEndpoints[(size_t)EHTTP_ENDPOINT::TOTAL] =
	{
		{ "index.html", nullptr },
		{ "sql", nullptr },
		{ "search", "json/search" },
		{ "index", "json/index" },
		{ "create", "json/create" },
		{ "insert", "json/insert" },
		{ "replace", "json/replace" },
		{ "update", "json/update" },
		{ "delete", "json/delete" },
		{ "bulk", "json/bulk" },
		{ "pq", "json/pq" },
		{ "cli", nullptr },
		{ "cli_json", nullptr },
		{ "_bulk", nullptr }
};

EHTTP_ENDPOINT StrToHttpEndpoint ( const CSphString& sEndpoint ) noexcept
{
	if ( sEndpoint.Begins ( g_dEndpoints[(int)EHTTP_ENDPOINT::PQ].m_szName1 ) || sEndpoint.Begins ( g_dEndpoints[(int)EHTTP_ENDPOINT::PQ].m_szName2 ) )
		return EHTTP_ENDPOINT::PQ;

	for ( int i = 0; i < (int)EHTTP_ENDPOINT::TOTAL; ++i )
		if ( sEndpoint == g_dEndpoints[i].m_szName1 || ( g_dEndpoints[i].m_szName2 && sEndpoint == g_dEndpoints[i].m_szName2 ) )
			return EHTTP_ENDPOINT ( i );

	return EHTTP_ENDPOINT::TOTAL;
}

///////////////////////////////////////////////////////////////////////
/// Stream reader

bool CharStream_c::GetError() const
{
	return ( ( m_pIn && m_pIn->GetError() ) || !m_sError.IsEmpty() );
}

const CSphString & CharStream_c::GetErrorMessage() const
{
	if ( m_pIn && m_pIn->GetError() )
		return m_pIn->GetErrorMessage();
	else
		return m_sError;
}

/// stub - returns feed string
class BlobStream_c final: public CharStream_c
{
	Str_t m_sData;

public:
	BlobStream_c ( const CSphString & sData )
		: CharStream_c ( nullptr )
		, m_sData { FromStr ( sData ) }
	{}

	Str_t Read() final
	{
		if ( m_bDone )
			return dEmptyStr;

		m_bDone = true;
		return m_sData;
	}

	Str_t ReadAll() final
	{
		auto sData = Read();

		Str_t sDescr = { sData.first, Min ( sData.second, 100 ) };
		myinfo::SetDescription ( sDescr, sDescr.second );
		return sData;
	}
};
/// stream with known content length and no special massage over socket
class RawSocketStream_c final : public CharStream_c
{
	int m_iContentLength;
	bool m_bTerminated = false;
	BYTE m_uOldTerminator = 0;
	CSphVector<BYTE> m_dUnpacked;
	bool m_bCompressed = false;

public:
	RawSocketStream_c ( AsyncNetInputBuffer_c * pIn, int iContentLength, bool bCompressed )
		: CharStream_c ( pIn )
		, m_iContentLength ( iContentLength )
		, m_bCompressed ( bCompressed )
	{
		assert ( pIn );
		m_bDone = !m_iContentLength;
	}

	~RawSocketStream_c() final
	{
		if ( m_bTerminated )
			m_pIn->Terminate ( 0, m_uOldTerminator );
		m_pIn->DiscardProcessed ( 0 );
	}

	Str_t Read() final
	{
		if ( m_bDone )
			return dEmptyStr;

		m_pIn->DiscardProcessed ( 0 );
		if ( !m_pIn->HasBytes() && m_pIn->ReadAny()<0 )
		{
			if ( !m_pIn->GetError() )
				m_sError.SetSprintf ( "failed to receive HTTP request (error='%s')", sphSockError() );
			m_bDone = true;
			return dEmptyStr;
		}

		auto iChunk = Min ( m_iContentLength, m_pIn->HasBytes() );
		m_iContentLength -= iChunk;
		m_bDone = !m_iContentLength;

		// Temporary write \0 at the end, since parser wants z-terminated buf
		if ( m_bDone )
		{
			m_uOldTerminator = m_pIn->Terminate ( iChunk, '\0' );
			m_bTerminated = true;
		}

		return Decompress ( m_pIn->PopTail ( iChunk ) );
	}

	Str_t ReadAll() final
	{
		if ( m_bDone )
			return dEmptyStr;

		// that is oneshot read - we sure, we're done
		m_bDone = true;

		if ( m_iContentLength && !m_pIn->ReadFrom ( m_iContentLength ) )
		{
			if ( !m_pIn->GetError() )
				m_sError.SetSprintf ( "failed to receive HTTP request (error='%s')", sphSockError() );
			return dEmptyStr;
		}

		m_uOldTerminator = m_pIn->Terminate ( m_iContentLength, '\0' );
		return Decompress ( m_pIn->PopTail ( m_iContentLength ) );
	}

	Str_t Decompress ( const ByteBlob_t & tIn )
	{
		if ( !m_bCompressed )
			return B2S ( tIn );

		m_dUnpacked.Resize ( 0 );
		if ( !GzipDecompress ( tIn, m_dUnpacked, m_sError ) )
		{
			m_bDone = true;
			return dEmptyStr;
		}
		return Str_t ( m_dUnpacked );
	}
};

/// chunked stream - i.e. total content length is unknown
class ChunkedSocketStream_c final: public CharStream_c
{
	CSphVector<BYTE>	m_dData;	// used only in ReadAll() call
	int m_iLastParsed;
	bool m_bBodyDone;
	CSphVector<Str_t> m_dBodies;
	http_parser_settings m_tParserSettings;
	http_parser* m_pParser;

private:
	// callbacks
	static int cbParserBody ( http_parser* pParser, const char* sAt, size_t iLen )
	{
		assert ( pParser->data );
		auto pThis = static_cast<ChunkedSocketStream_c*> ( pParser->data );
		return pThis->ParserBody ( { sAt, (int)iLen } );
	}

	static int cbMessageComplete ( http_parser* pParser )
	{
		assert ( pParser->data );
		auto pThis = static_cast<ChunkedSocketStream_c*> ( pParser->data );
		return pThis->MessageComplete();
	}

	inline int MessageComplete()
	{
		HTTPINFO << "ChunkedSocketStream_c::MessageComplete";

		m_bBodyDone = true;
		return 0;
	}

	inline int ParserBody ( Str_t sData )
	{
		HTTPINFO << "ParserBody chunked str with " << sData.second << " bytes '" << Data2Log ( sData ) << "'";;

		if ( !IsEmpty ( sData ) )
			m_dBodies.Add ( sData );
		return 0;
	}

	void ParseBody ( ByteBlob_t sData )
	{
		HTTPINFO << "ParseBody chunked blob with " << sData.second << " bytes '" << Data2Log ( sData ) << "'";;
		m_iLastParsed = (int)http_parser_execute ( m_pParser, &m_tParserSettings, (const char*)sData.first, sData.second );
		if ( m_iLastParsed != sData.second )
		{
			HTTPINFO << "ParseBody error: parsed " << m_iLastParsed << ", chunk " << sData.second;
			if ( !m_pIn->GetError() )
				m_sError = http_errno_description ( (http_errno)m_pParser->http_errno );
		}
	}

public:
	ChunkedSocketStream_c ( AsyncNetInputBuffer_c * pIn, http_parser * pParser, bool bBodyDone, CSphVector<Str_t> dBodies, int iLastParsed )
		: CharStream_c ( pIn )
		, m_iLastParsed ( iLastParsed )
		, m_bBodyDone ( bBodyDone )
		, m_pParser ( pParser )
	{
		assert ( pIn );
		m_dBodies = std::move ( dBodies );
		http_parser_settings_init ( &m_tParserSettings );
		m_tParserSettings.on_body = cbParserBody;
		m_tParserSettings.on_message_complete = cbMessageComplete;
		m_pParser->data = this;
	}

	void DiscardLast()
	{
		m_pIn->PopTail ( std::exchange ( m_iLastParsed, 0 ) );
		m_pIn->DiscardProcessed ( 0 );
	}

	~ChunkedSocketStream_c() final
	{
		DiscardLast();
	}

	Str_t Read() final
	{
		if ( m_bDone )
			return dEmptyStr;

		while ( m_dBodies.IsEmpty() )
		{
			if ( m_bBodyDone )
			{
				m_bDone = true;
				return dEmptyStr;
			}

			DiscardLast();
			if ( !m_pIn->HasBytes() )
			{
				switch ( m_pIn->ReadAny() )
				{
				case -1:
				case 0:
					m_bDone = true;
					return dEmptyStr;
				default:
					break;
				}
			}
			ParseBody ( m_pIn->Tail() );
		}

		auto sResult = m_dBodies.First();
		m_dBodies.Remove ( 0 );

		if ( m_bBodyDone && m_dBodies.IsEmpty() )
		{
			m_bDone = true;
			const_cast<char&> ( sResult.first[sResult.second] ) = '\0';
		}
		return sResult;
	}

	Str_t ReadAll() final
	{
		auto sFirst = Read();

		if ( m_bDone )
			return sFirst;

		m_dData.Append ( sFirst );
		do
			m_dData.Append ( Read() );
		while ( !m_bDone );

		m_dData.Add ( '\0' );
		m_dData.Resize ( m_dData.GetLength() - 1 );
		return m_dData;
	}
};

///////////////////////////////////////////////////////////////////////

CSphString HttpEndpointToStr ( EHTTP_ENDPOINT eEndpoint )
{
	assert ( eEndpoint < EHTTP_ENDPOINT::TOTAL );
	return g_dEndpoints[(int)eEndpoint].m_szName1;
}

void HttpBuildReply ( CSphVector<BYTE> & dData, EHTTP_STATUS eCode, Str_t sReply, bool bHtml )
{
	const char * sContent = ( bHtml ? "text/html" : "application/json" );
	StringBuilder_c sHttp;
	sHttp.Sprintf ( "HTTP/1.1 %s\r\nServer: %s\r\nContent-Type: %s; charset=UTF-8\r\nContent-Length: %d\r\n\r\n", HttpGetStatusName ( eCode ), g_sStatusVersion.cstr(), sContent, sReply.second );

	dData.Reserve ( sHttp.GetLength() + sReply.second );
	dData.Append ( (Str_t)sHttp );
	dData.Append ( sReply );
}


HttpRequestParser_c::HttpRequestParser_c()
{
	http_parser_settings_init ( &m_tParserSettings );

	m_tParserSettings.on_url = cbParserUrl;
	m_tParserSettings.on_header_field = cbParserHeaderField;
	m_tParserSettings.on_header_value = cbParserHeaderValue;
	m_tParserSettings.on_headers_complete = cbParseHeaderCompleted;
	m_tParserSettings.on_body = cbParserBody;

	m_tParserSettings.on_message_begin = cbMessageBegin;
	m_tParserSettings.on_message_complete = cbMessageComplete;
	m_tParserSettings.on_status = cbMessageStatus;

	Reinit();
}

void HttpRequestParser_c::Reinit()
{
	HTTPINFO << "HttpRequestParser_c::Reinit()";

	http_parser_init ( &m_tParser, HTTP_REQUEST );
	m_sEndpoint = "";
	m_sCurField.Clear();
	m_sCurValue.Clear();
	m_hOptions.Reset();
	m_eType = HTTP_GET;
	m_sUrl.Clear();
	m_bHeaderDone = false;
	m_bBodyDone = false;
	m_dParsedBodies.Reset();
	m_iParsedBodyLength = 0;
	m_iLastParsed = 0;
	m_szError = nullptr;
	m_tParser.data = this;
}

bool HttpRequestParser_c::ParseHeader ( ByteBlob_t sData )
{
	HTTPINFO << "ParseChunk with " << sData.second << " bytes '" << Data2Log ( sData ) << "'";

	m_iLastParsed = (int) http_parser_execute ( &m_tParser, &m_tParserSettings, (const char *)sData.first, sData.second );
	if ( m_iLastParsed != sData.second )
	{
		if ( g_bLogBadHttpReq )
		{
			sphWarning ( "ParseChunk error: parsed %d, chunk %d, conn %d, %.*s", m_iLastParsed, sData.second, session::GetConnID(), sData.second, sData.first );
		} else
		{
			HTTPINFO << "ParseChunk error: parsed " << m_iLastParsed << ", chunk " << sData.second;
		}
		m_szError = http_errno_description ( (http_errno)m_tParser.http_errno );
		return true;
	}

	return m_bHeaderDone;
}


int HttpRequestParser_c::ParsedBodyLength() const
{
	return m_iParsedBodyLength;
}

bool HttpRequestParser_c::Expect100() const
{
	return m_hOptions.Exists ( "expect" ) && m_hOptions["expect"] == "100-continue";
}

bool HttpRequestParser_c::KeepAlive() const
{
	return m_bKeepAlive;
}

const char* HttpRequestParser_c::Error() const
{
	return m_szError;
}

bool HttpRequestParser_c::IsBuddyQuery () const
{
	return ::IsBuddyQuery ( m_hOptions );
}

inline int Char2Hex ( BYTE uChar )
{
	switch (uChar)
	{
	case '0': return 0;
	case '1': return 1;
	case '2': return 2;
	case '3': return 3;
	case '4': return 4;
	case '5': return 5;
	case '6': return 6;
	case '7': return 7;
	case '8': return 8;
	case '9': return 9;
	case 'a': case 'A': return 10;
	case 'b': case 'B': return 11;
	case 'c': case 'C': return 12;
	case 'd': case 'D': return 13;
	case 'e': case 'E': return 14;
	case 'f': case 'F': return 15;
	default: break;
	}
	return -1;
}

inline int Chars2Hex ( const char* pSrc )
{
	int iRes = Char2Hex ( *( pSrc + 1 ) );
	return iRes < 0 ? iRes : iRes + Char2Hex ( *pSrc ) * 16;
}

void UriPercentReplace ( Str_t & sEntity, Replace_e ePlus )
{
	if ( IsEmpty ( sEntity ) )
		return;

	const char* pSrc = sEntity.first;
	auto* pDst = const_cast<char*> ( pSrc );
	char cPlus = ( ePlus==Replace_e::NoPlus ? ' ' : '+' );
	auto* pEnd = pSrc + sEntity.second;
	while ( pSrc < pEnd )
	{
		if ( *pSrc=='%' && *(pSrc+1) && *(pSrc+2) )
		{
			auto iCode = Chars2Hex ( pSrc + 1 );
			if ( iCode<0 )
			{
				*pDst++ = *pSrc++;
				continue;
			}
			pSrc += 3;
			*pDst++ = (char) iCode;
		} else
		{
			*pDst++ = ( *pSrc=='+' ? cPlus : *pSrc );
			pSrc++;
		}
	}
	sEntity.second = int ( pDst - sEntity.first );
}

void StoreRawQuery ( OptionsHash_t& hOptions, CSphString sRawBody )
{
	if ( sRawBody.IsEmpty() )
		return;
	hOptions.Add ( std::move ( sRawBody ), "raw_query" );
}

void DecodeAndStoreRawQuery ( OptionsHash_t& hOptions, const Str_t& sWholeData )
{
	if ( IsEmpty ( sWholeData ) )
		return;

	// store raw query
	CSphString sRawBody ( sWholeData );				   // copy raw data, important!
	Str_t sRaw { sRawBody.cstr(), sWholeData.second }; // FromStr implies strlen(), but we don't need it
	UriPercentReplace ( sRaw, Replace_e::NoPlus );				   // avoid +-decoding
	*const_cast<char*> ( sRaw.first + sRaw.second ) = '\0';

	StoreRawQuery ( hOptions, std::move ( sRawBody ));
}

void HttpRequestParser_c::ParseList ( Str_t sData, OptionsHash_t & hOptions )
{
	HTTPINFO << "ParseList with " << sData.second << " bytes '" << Data2Log ( sData ) << "'";

	CSphString sBuf ( sData );
	const char * sCur = sBuf.cstr();
	const char * sLast = sCur;
	const char * sEnd = sCur + sData.second;

	Str_t sName = dEmptyStr;
	for ( ; sCur<sEnd; ++sCur )
	{
		switch (*sCur)
		{
		case '=':
			{
				sName = { sLast, int ( sCur - sLast ) };
				UriPercentReplace ( sName, Replace_e::NoPlus );
				sLast = sCur + 1;
				break;
			}
		case '&':
			{
				Str_t sVal { sLast, int ( sCur - sLast ) };
				UriPercentReplace ( sVal, Replace_e::NoPlus );
				ToLower ( sName );
				hOptions.Add ( sVal, sName );
				sLast = sCur + 1;
				sName = dEmptyStr;
				break;
			}
		default:
			break;
		}
	}

	if ( IsEmpty ( sName ) )
		return;

	Str_t sVal { sLast, int ( sCur - sLast ) };
	UriPercentReplace ( sVal, Replace_e::NoPlus );
	ToLower ( sName );
	hOptions.Add ( sVal, sName );
}

inline int HttpRequestParser_c::ParserUrl ( Str_t sData )
{
	HTTPINFO << "ParseUrl with " << sData.second << " bytes '" << sData << "'";

	m_sUrl << sData;
	return 0;
}

inline void HttpRequestParser_c::FinishParserUrl ()
{
	if ( m_sUrl.IsEmpty() )
		return;

	auto _ = AtScopeExit ( [this] { m_sUrl.Clear(); } );
	auto sData = (Str_t) m_sUrl;
	http_parser_url tUri;
	if ( http_parser_parse_url ( sData.first, sData.second, 0, &tUri ) )
		return;

	DWORD uPath = ( 1UL<<UF_PATH );
	DWORD uQuery = ( 1UL<<UF_QUERY );

	if ( ( tUri.field_set & uPath )!=0 )
	{
		const char * sPath = sData.first + tUri.field_data[UF_PATH].off;
		int iPathLen = tUri.field_data[UF_PATH].len;
		if ( *sPath=='/' )
		{
			++sPath;
			--iPathLen;
		}

		// URL should be split fully to point to proper endpoint 
		m_sEndpoint.SetBinary ( sPath, iPathLen );
		// transfer endpoint for further parse
		m_hOptions.Add ( m_sEndpoint, "endpoint" );
	}

	if ( ( tUri.field_set & uQuery )!=0 )
	{
		Str_t sRawGetQuery { sData.first + tUri.field_data[UF_QUERY].off, tUri.field_data[UF_QUERY].len };
		if ( m_eType==HTTP_GET )
			DecodeAndStoreRawQuery ( m_hOptions, sRawGetQuery );
		ParseList ( sRawGetQuery, m_hOptions );
	}

	CSphString sFullURL;
	if ( ( tUri.field_set & uPath )!=0 && ( tUri.field_set & uQuery )!=0 )
	{
		const char * sStart = sData.first + tUri.field_data[UF_PATH].off;
		const char * sEnd = sData.first + tUri.field_data[UF_QUERY].off + tUri.field_data[UF_QUERY].len;
		sFullURL.SetBinary ( sStart, sEnd-sStart );
		m_hOptions.Add ( sFullURL, "full_url" );
	} else if ( ( tUri.field_set & uPath )!=0 )
	{
		const char * sPath = sData.first + tUri.field_data[UF_PATH].off;
		int iPathLen = tUri.field_data[UF_PATH].len;

		// URL should be split fully to point to proper endpoint 
		sFullURL.SetBinary ( sPath, iPathLen );
		m_hOptions.Add ( sFullURL, "full_url" );
	}
}

inline int HttpRequestParser_c::ParserHeaderField ( Str_t sData )
{
	FinishParserKeyVal();
	m_sCurField << sData;
	return 0;
}

inline int HttpRequestParser_c::ParserHeaderValue ( Str_t sData )
{
	m_sCurValue << sData;
	return 0;
}

inline void HttpRequestParser_c::FinishParserKeyVal()
{
	if ( m_sCurValue.IsEmpty() )
		return;

	CSphString sField = (CSphString)m_sCurField;
	sField.ToLower();
	m_hOptions.Add ( (CSphString)m_sCurValue, sField );
	m_sCurField.Clear();
	m_sCurValue.Clear();
}

inline int HttpRequestParser_c::ParserBody ( Str_t sData )
{
	HTTPINFO << "ParserBody parser with " << sData.second << " bytes '" << Data2Log ( sData ) << "'";

	if ( !m_dParsedBodies.IsEmpty() )
	{
		auto& sLast = m_dParsedBodies.Last();
		if ( sLast.first + sLast.second == sData.first )
			sLast.second += sData.second;
		else
			m_dParsedBodies.Add ( sData );
	} else
		m_dParsedBodies.Add ( sData );
	m_iParsedBodyLength += sData.second;
	return 0;
}

inline int HttpRequestParser_c::MessageComplete ()
{
	HTTPINFO << "MessageComplete";

	m_bBodyDone = true;
	return 0;
}

inline int HttpRequestParser_c::ParseHeaderCompleted ()
{
	HTTPINFO << "ParseHeaderCompleted. Upgrade=" << (unsigned int)m_tParser.upgrade << ", length=" << (int64_t) m_tParser.content_length;

	// we're not support connection upgrade - so just reset upgrade flag, if detected.
	// rfc7540 section-3.2 (for http/2) says, we just should continue as if no 'upgrade' header was found
	m_tParser.upgrade = 0;

	// connection wide http options
	m_bKeepAlive = ( ( m_tParser.flags & F_CONNECTION_KEEP_ALIVE ) != 0 );
	m_eType = (http_method)m_tParser.method;

	FinishParserKeyVal();
	FinishParserUrl();
	m_bHeaderDone = true;
	return 0;
}

int HttpRequestParser_c::cbParserUrl ( http_parser* pParser, const char* sAt, size_t iLen )
{
	assert ( pParser->data );
	auto pThis = static_cast<HttpRequestParser_c*> ( pParser->data );
	return pThis->ParserUrl ( { sAt, (int)iLen } );
}

int HttpRequestParser_c::cbParserHeaderField ( http_parser* pParser, const char* sAt, size_t iLen )
{
	assert ( pParser->data );
	auto pThis = static_cast<HttpRequestParser_c*> ( pParser->data );
	return pThis->ParserHeaderField ( { sAt, (int)iLen } );
}

int HttpRequestParser_c::cbParserHeaderValue ( http_parser* pParser, const char* sAt, size_t iLen )
{
	assert ( pParser->data );
	auto pThis = static_cast<HttpRequestParser_c*> ( pParser->data );
	return pThis->ParserHeaderValue ( { sAt, (int)iLen } );
}

int HttpRequestParser_c::cbParseHeaderCompleted ( http_parser* pParser )
{
	assert ( pParser->data );
	auto pThis = static_cast<HttpRequestParser_c*> ( pParser->data );
	return pThis->ParseHeaderCompleted ();
}

int HttpRequestParser_c::cbMessageBegin ( http_parser* pParser )
{
	HTTPINFO << "cbMessageBegin";
	return 0;
}

int HttpRequestParser_c::cbMessageComplete ( http_parser* pParser )
{
	assert ( pParser->data );
	auto pThis = static_cast<HttpRequestParser_c*> ( pParser->data );
	return pThis->MessageComplete();
}

int HttpRequestParser_c::cbMessageStatus ( http_parser* pParser, const char* sAt, size_t iLen )
{
	HTTPINFO << "cbMessageStatus with '" << Str_t { sAt, (int)iLen } << "'";
	return 0;
}

int HttpRequestParser_c::cbParserBody ( http_parser* pParser, const char* sAt, size_t iLen )
{
	assert ( pParser->data );
	auto pThis = static_cast<HttpRequestParser_c*> ( pParser->data );
	return pThis->ParserBody ( { sAt, (int)iLen } );
}

static const char * g_sIndexPage =
R"index(<!DOCTYPE html>
<html>
<head>
<title>Manticore</title>
</head>
<body>
<h1>Manticore daemon</h1>
<p>%s</p>
</body>
</html>)index";


static void HttpHandlerIndexPage ( CSphVector<BYTE> & dData )
{
	StringBuilder_c sIndexPage;
	sIndexPage.Appendf ( g_sIndexPage, g_sStatusVersion.cstr() );
	HttpBuildReply ( dData, EHTTP_STATUS::_200, (Str_t)sIndexPage, true );
}

//////////////////////////////////////////////////////////////////////////
class JsonRequestBuilder_c : public RequestBuilder_i
{
public:
	JsonRequestBuilder_c ( const char* szQuery, CSphString sEndpoint, const CSphString & sRawQuery, const CSphString & sFullUrl )
		: m_sEndpoint ( std::move ( sEndpoint ) )
		, m_tQuery ( szQuery )
		, m_sRawQuery ( sRawQuery )
		, m_sFullUrl ( sFullUrl )
	{
		// fixme: we can implement replacing indexes in a string (without parsing) if it becomes a performance issue
	}

	void BuildRequest ( const AgentConn_t & tAgent, ISphOutputBuffer & tOut ) const final
	{
		// replace "table" value in the json query
		m_tQuery.DelItem ( "table" );
		m_tQuery.AddStr ( "table", tAgent.m_tDesc.m_sIndexes.cstr() );

		CSphString sRequest = m_tQuery.AsString();

		auto tWr = APIHeader ( tOut, SEARCHD_COMMAND_JSON, VER_COMMAND_JSON ); // API header
		tOut.SendString ( m_sEndpoint.cstr() );
		tOut.SendString ( sRequest.cstr() );
		tOut.SendString ( m_sRawQuery.cstr() );
		tOut.SendString ( m_sFullUrl.cstr() );
	}

private:
	CSphString			m_sEndpoint;
	mutable JsonObj_c	m_tQuery;
	const CSphString & m_sRawQuery;
	const CSphString & m_sFullUrl;
};


class JsonReplyParser_c : public ReplyParser_i
{
public:
	JsonReplyParser_c ( int & iAffected, int & iWarnings, SearchFailuresLog_c & tFails )
		: m_iAffected ( iAffected )
		, m_iWarnings ( iWarnings )
		, m_tFails  ( tFails )
	{}

	bool ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & tAgent ) const final
	{
		CSphString sEndpoint = tReq.GetString();
		EHTTP_ENDPOINT eEndpoint = StrToHttpEndpoint ( sEndpoint );
		if ( eEndpoint!=EHTTP_ENDPOINT::JSON_UPDATE && eEndpoint!=EHTTP_ENDPOINT::JSON_DELETE )
			return false;

		DWORD uLength = tReq.GetDword();
		CSphFixedVector<BYTE> dResult ( uLength+1 );
		tReq.GetBytes ( dResult.Begin(), (int)uLength );
		dResult[uLength] = '\0';

		CSphString sError;
		bool bOk = sphGetResultStats ( (const char *)dResult.Begin(), m_iAffected, m_iWarnings, eEndpoint==EHTTP_ENDPOINT::JSON_UPDATE, sError );
		if ( !sError.IsEmpty() )
			m_tFails.Submit ( tAgent.m_tDesc.m_sIndexes, nullptr, sError.cstr() );

		return bOk;
	}

protected:
	int &	m_iAffected;
	int &	m_iWarnings;
	SearchFailuresLog_c & m_tFails;
};

std::unique_ptr<QueryParser_i> CreateQueryParser ( bool bJson ) noexcept
{
	return bJson ? sphCreateJsonQueryParser() : sphCreatePlainQueryParser();
}

std::unique_ptr<RequestBuilder_i> CreateRequestBuilder ( Str_t sQuery, const SqlStmt_t & tStmt )
{
	if ( tStmt.m_bJson )
	{
		assert ( !tStmt.m_sEndpoint.IsEmpty() );
		return std::make_unique<JsonRequestBuilder_c> ( sQuery.first, tStmt.m_sEndpoint, tStmt.m_sRawQuery, tStmt.m_sFullUrl );
	} else
	{
		return std::make_unique<SphinxqlRequestBuilder_c> ( sQuery, tStmt );
	}
}

std::unique_ptr<ReplyParser_i> CreateReplyParser ( bool bJson, int & iUpdated, int & iWarnings, SearchFailuresLog_c & tFails )
{
	if ( bJson )
		return std::make_unique<JsonReplyParser_c> ( iUpdated, iWarnings, tFails );
	else
		return std::make_unique<SphinxqlReplyParser_c> ( &iUpdated, &iWarnings );
}

//////////////////////////////////////////////////////////////////////////
class HttpErrorReporter_c final : public StmtErrorReporter_i
{
public:
	void			Ok ( int iAffectedRows, const CSphString & /*sWarning*/, int64_t /*iLastInsertId*/ ) final { m_iAffected = iAffectedRows; }
	void			Ok ( int iAffectedRows, int /*nWarnings*/ ) final { m_iAffected = iAffectedRows; }
	void			ErrorEx ( EMYSQL_ERR eErr, const char * sError ) final;
	RowBuffer_i * 	GetBuffer() final { return nullptr; }

	bool			IsError() const { return m_bError; }
	const char *	GetError() const { return m_sError.cstr(); }
	int				GetAffectedRows() const { return m_iAffected; }

private:
	bool			m_bError {false};
	CSphString		m_sError;
	int				m_iAffected {0};
};


void HttpErrorReporter_c::ErrorEx ( EMYSQL_ERR /*iErr*/, const char * sError )
{
	m_bError = true;
	m_sError = sError;
}

StmtErrorReporter_i * CreateHttpErrorReporter()
{
	return new HttpErrorReporter_c();
}

//////////////////////////////////////////////////////////////////////////
// all the handlers for http queries

void ReplyBuf ( Str_t sResult, EHTTP_STATUS eStatus, bool bNeedHttpResponse, CSphVector<BYTE> & dData )
{
	if ( bNeedHttpResponse )
		HttpBuildReply ( dData, eStatus, sResult, false );
	else
	{
		dData.Resize ( 0 );
		dData.Append ( sResult );
	}
}


void HttpHandler_c::SetErrorFormat ( bool bNeedHttpResponse )
{
	m_bNeedHttpResponse = bNeedHttpResponse;
}
	
CSphVector<BYTE> & HttpHandler_c::GetResult()
{
	return m_dData;
}

const CSphString & HttpHandler_c::GetError () const
{
	return m_sError;
}

EHTTP_STATUS HttpHandler_c::GetStatusCode() const
{
	return m_eHttpCode;
}

void HttpHandler_c::ReportError ( const char * szError, EHTTP_STATUS eStatus )
{
	m_sError = szError;
	ReportError ( eStatus );
}

void HttpHandler_c::ReportError ( EHTTP_STATUS eStatus )
{
	m_eHttpCode = eStatus;
	if ( m_bNeedHttpResponse )
		sphHttpErrorReply ( m_dData, eStatus, m_sError.cstr() );
	else
	{
		m_dData.Resize ( m_sError.Length() );
		memcpy ( m_dData.Begin(), m_sError.cstr(), m_dData.GetLength() );
	}
}

void HttpHandler_c::FormatError ( EHTTP_STATUS eStatus, const char * sError, ... )
{
	va_list ap;
	va_start ( ap, sError );
	m_sError.SetSprintfVa ( sError, ap );
	va_end ( ap );

	m_eHttpCode = eStatus;
	if ( m_bNeedHttpResponse )
		sphHttpErrorReply ( m_dData, eStatus, m_sError.cstr() );
	else
	{
		int iLen = m_sError.Length();
		m_dData.Resize ( iLen+1 );
		memcpy ( m_dData.Begin(), m_sError.cstr(), iLen );
		m_dData[iLen] = '\0';
	}
}

void HttpHandler_c::ReportError ( const char * sError, HttpErrorType_e eType, EHTTP_STATUS eStatus, const char * sIndex )
{
	if ( sError )
		m_sError = sError;
	m_eHttpCode = eStatus;

	const char * sErrorType = GetErrorTypeName ( eType );
	int iStatus = HttpGetStatusCodes ( eStatus );
	CSphString sReply = ( sErrorType ? JsonEncodeResultError ( m_sError, sErrorType, iStatus, sIndex ) : JsonEncodeResultError ( m_sError, iStatus ) );
	HttpBuildReplyHead ( GetResult(), eStatus, sReply.cstr(), sReply.Length(), false );
}

void HttpHandler_c::BuildReply ( const CSphString & sResult, EHTTP_STATUS eStatus )
{
	m_eHttpCode = eStatus;
	ReplyBuf ( FromStr ( sResult ), eStatus, m_bNeedHttpResponse, m_dData );
}

void HttpHandler_c::BuildReply ( const char* szResult, EHTTP_STATUS eStatus )
{
	m_eHttpCode = eStatus;
	ReplyBuf ( FromSz( szResult ), eStatus, m_bNeedHttpResponse, m_dData );
}

void HttpHandler_c::BuildReply ( const StringBuilder_c & sResult, EHTTP_STATUS eStatus )
{
	m_eHttpCode = eStatus;
	ReplyBuf ( (Str_t)sResult, eStatus, m_bNeedHttpResponse, m_dData );
}

void HttpHandler_c::BuildReply ( Str_t sResult, EHTTP_STATUS eStatus )
{
	m_eHttpCode = eStatus;
	ReplyBuf ( sResult, eStatus, m_bNeedHttpResponse, m_dData );
}

// check whether given served index is exist and has requested type
bool HttpHandler_c::CheckValid ( const ServedIndex_c* pServed, const CSphString& sIndex, IndexType_e eType )
{
	if ( !pServed )
	{
		FormatError ( EHTTP_STATUS::_500, "no such table '%s'", sIndex.cstr () );
		return false;
	}
	if ( pServed->m_eType!=eType )
	{
		FormatError ( EHTTP_STATUS::_500, "table '%s' is not %s", sIndex.cstr(), GetIndexTypeName ( eType ) );
		return false;
	}
	return true;
}



struct HttpOptionTrait_t
{
	const OptionsHash_t & m_tOptions;
	explicit HttpOptionTrait_t ( const OptionsHash_t & tOptions ) 
		: m_tOptions ( tOptions )
	{}
};

class HttpSearchHandler_c : public HttpHandler_c, public HttpOptionTrait_t
{
public:
	bool Process () final
	{
		TRACE_CONN ( "conn", "HttpSearchHandler_c::Process" );
		CSphString sWarning;
		std::unique_ptr<QueryParser_i> pQueryParser = PreParseQuery();
		if ( !pQueryParser )
			return false;

		if ( IsBuddyQuery ( m_tOptions ) )
			m_tParsed.m_tQuery.m_uDebugFlags |= QUERY_DEBUG_NO_LOG;

		auto tHandler = CreateMsearchHandler ( std::move ( pQueryParser ), m_eQueryType, m_tParsed );
		SetStmt ( tHandler );

		QueryProfile_c tProfile;
		tProfile.m_eNeedPlan = (PLAN_FLAVOUR)m_tParsed.m_iPlan;
		tProfile.m_bNeedProfile = m_tParsed.m_bProfile;
		bool bNeedProfile = m_tParsed.m_bProfile || ( m_tParsed.m_iPlan != 0 );
		if ( bNeedProfile )
			tHandler.SetProfile ( &tProfile );

		// search
		tHandler.RunQueries();

		if ( bNeedProfile )
			tProfile.Stop();

		AggrResult_t & tRes = tHandler.m_dAggrResults.First();
		if ( !tRes.m_sError.IsEmpty() )
		{
			ReportError ( tRes.m_sError.cstr(), EHTTP_STATUS::_500 );
			return false;
		}

		// fixme: handle more than one warning at once?
		if ( tRes.m_sWarning.IsEmpty() && !m_tParsed.m_sWarning.IsEmpty() )
			tRes.m_sWarning = m_tParsed.m_sWarning;

		CSphString sResult = EncodeResult ( tHandler.m_dAggrResults, bNeedProfile ? &tProfile : nullptr );
		BuildReply ( sResult, EHTTP_STATUS::_200 );

		return true;
	}

	explicit HttpSearchHandler_c ( const OptionsHash_t & tOptions )
		: HttpOptionTrait_t ( tOptions )
	{}

protected:
	QueryType_e				m_eQueryType {QUERY_SQL};
	ParsedJsonQuery_t		m_tParsed;

	virtual std::unique_ptr<QueryParser_i> PreParseQuery() = 0;
	CSphString		EncodeResult ( const VecTraits_T<AggrResult_t> & dRes, QueryProfile_c * pProfile )
	{
		return sphEncodeResultJson ( dRes, m_tParsed.m_tQuery, pProfile, ResultSetFormat_e::MntSearch );
	}
	virtual void			SetStmt ( SearchHandler_c & tHandler ) {};
};

static void AddAggs ( const VecTraits_T<SqlStmt_t> & dStmt, JsonQuery_c & tQuery )
{
	assert ( dStmt.GetLength()>1 && dStmt[0].m_tQuery.m_bFacetHead );
	tQuery.m_dAggs.Reserve ( dStmt.GetLength()-1 );

	for ( int i=1; i<dStmt.GetLength(); i++ )
	{
		const CSphQuery & tRef = dStmt[i].m_tQuery;
		assert ( tRef.m_dRefItems.GetLength() );

		JsonAggr_t & tBucket = tQuery.m_dAggs.Add();
		tBucket.m_sBucketName = tRef.m_dRefItems[0].m_sExpr;
		tBucket.m_sCol = tRef.m_dRefItems[0].m_sAlias;
	}
}

class HttpSearchHandler_SQL_c final: public HttpSearchHandler_c
{
public:
	explicit HttpSearchHandler_SQL_c ( const OptionsHash_t & tOptions )
		: HttpSearchHandler_c ( tOptions )
	{}

protected:
	CSphVector<SqlStmt_t> m_dStmt;
	std::unique_ptr<QueryParser_i> PreParseQuery() final
	{
		const CSphString * pRawQl = m_tOptions ( "query" );
		if ( !pRawQl || pRawQl->IsEmpty() )
		{
			ReportError ( "query missing", EHTTP_STATUS::_400 );
			return nullptr;
		}

		if ( !sphParseSqlQuery ( FromStr ( *pRawQl ), m_dStmt, m_sError, SPH_COLLATION_DEFAULT ) )
		{
			ReportError ( EHTTP_STATUS::_400 );
			return nullptr;
		}

		( (CSphQuery &) m_tParsed.m_tQuery ) = m_dStmt[0].m_tQuery;
		bool bFacet = ( m_dStmt.GetLength()>1 );
		for ( const auto & tStmt : m_dStmt )
		{
			// should be all FACET in case of multiple queries
			bFacet &= ( tStmt.m_tQuery.m_bFacet || tStmt.m_tQuery.m_bFacetHead );

			if ( tStmt.m_eStmt!=STMT_SELECT )
			{
				ReportError ( "only SELECT queries are supported", EHTTP_STATUS::_501 );
				return nullptr;
			}
		}

		if ( m_dStmt.GetLength()>1 && !bFacet )
		{
			ReportError ( "only FACET multiple queries supported", EHTTP_STATUS::_501 );
			return nullptr;
		}
		if ( bFacet )
			AddAggs ( m_dStmt, m_tParsed.m_tQuery );

		m_eQueryType = QUERY_SQL;

		return sphCreatePlainQueryParser();
	}

	void SetStmt ( SearchHandler_c & tHandler )
	{
		tHandler.m_pStmt = &m_dStmt[0];
		for  ( int i=1; i<m_dStmt.GetLength(); ++i )
			tHandler.SetQuery ( i, m_dStmt[i].m_tQuery, nullptr );
	}
};

typedef std::pair<CSphString,MysqlColumnType_e> ColumnNameType_t;

static const char * GetMysqlTypeName ( MysqlColumnType_e eType )
{
	switch ( eType )
	{
		case MYSQL_COL_DECIMAL:		return "decimal";
		case MYSQL_COL_LONG:		return "long";
		case MYSQL_COL_FLOAT:		return "float";
		case MYSQL_COL_DOUBLE:		return "double";
		case MYSQL_COL_LONGLONG:	return "long long";
		case MYSQL_COL_STRING:		return "string";
		default: return "unknown";
	};
}

static MysqlColumnType_e GetMysqlTypeByName ( const CSphString& sType )
{
	if ( sType=="decimal")
		return MYSQL_COL_DECIMAL;

	if ( sType == "long" )
		return MYSQL_COL_LONG;

	if ( sType == "float" )
		return MYSQL_COL_FLOAT;

	if ( sType == "double" )
		return MYSQL_COL_DOUBLE;

	if ( sType == "long long" )
		return MYSQL_COL_LONGLONG;

	if ( sType == "string" )
		return MYSQL_COL_STRING;

	assert (false && "Unknown column");
	return MYSQL_COL_STRING;
}

JsonEscapedBuilder& operator<< ( JsonEscapedBuilder& tOut, MysqlColumnType_e eType )
{
	tOut.FixupSpacedAndAppendEscaped ( GetMysqlTypeName ( eType ) );
	return tOut;
}

const StrBlock_t dJsonObjCustom { { ",\n", 2 }, { "[", 1 }, { "]", 1 } }; // json object with custom formatting

class JsonRowBuffer_c : public RowBuffer_i
{
public:
	JsonRowBuffer_c()
	{
		m_dBuf.StartBlock ( dJsonObjCustom );
	}

	void PutFloatAsString ( float fVal, const char * ) override
	{
		AddDataColumn();
		m_dBuf << fVal;
	}

	void PutDoubleAsString ( double fVal, const char * ) override
	{
		AddDataColumn();
		m_dBuf << fVal;
	}

	void PutNumAsString ( int64_t iVal ) override
	{
		AddDataColumn();
		m_dBuf << iVal;
	}

	void PutNumAsString ( uint64_t uVal ) override
	{
		AddDataColumn();
		m_dBuf << uVal;
	}

	void PutNumAsString ( int iVal ) override
	{
		AddDataColumn();
		m_dBuf << iVal;
	}
	
	void PutNumAsString ( DWORD uVal ) override
	{
		AddDataColumn();
		m_dBuf << uVal;
	}

	void PutArray ( const ByteBlob_t& dBlob, bool ) override
	{
		AddDataColumn();
		m_dBuf.FixupSpacedAndAppendEscaped ( (const char*)dBlob.first, dBlob.second );
	}

	void PutString ( Str_t sMsg ) override
	{
		PutArray ( S2B ( sMsg ), false );
	}

	void PutMicrosec ( int64_t iUsec ) override
	{
		PutNumAsString ( iUsec );
	}

	void PutNULL() override
	{
		AddDataColumn();
		m_dBuf << "null";
	}

	bool Commit() override
	{
		m_dBuf.FinishBlock ( false ); // finish previous item
		m_dBuf.ObjectBlock(); // start new item
		++m_iTotalRows;
		m_iCol = 0;
		return true;
	}

	void Eof ( bool bMoreResults, int iWarns, const char* ) override
	{
		m_dBuf.FinishBlock ( true ); // last doc, allow empty
		m_dBuf.FinishBlock ( false ); // docs section
		DataFinish ( m_iTotalRows, nullptr, nullptr );
		m_dBuf.FinishBlock ( false ); // root object
	}

	void Error ( const char * szError, EMYSQL_ERR ) override
	{
		auto _ = m_dBuf.Object ( false );
		DataFinish ( 0, szError, nullptr );
		m_bError = true;
		m_sError = szError;
	}

	void Ok ( int iAffectedRows, int iWarns, const char * sMessage, bool bMoreResults, int64_t iLastInsertId ) override
	{
		auto _ = m_dBuf.Object ( false );
		DataFinish ( iAffectedRows, nullptr, sMessage );
	}

	void HeadBegin () override
	{
		m_iTotalRows = 0;
		m_dBuf.ObjectWBlock();
		m_dBuf.Named ( "columns" );
		m_dBuf.ArrayBlock();
	}

	bool HeadEnd ( bool , int ) override
	{
		m_dBuf.FinishBlock(false);
		m_dBuf.Named ( "data" );
		m_dBuf.ArrayWBlock();
		m_dBuf.ObjectBlock();
		return true;
	}

	void HeadColumn ( const char * szName, MysqlColumnType_e eType ) override
	{
		JsonEscapedBuilder sEscapedName;
		sEscapedName.FixupSpacedAndAppendEscaped ( szName );
		ColumnNameType_t tCol { (CSphString)sEscapedName, eType };
		auto _ = m_dBuf.Object(false);
		m_dBuf.AppendName ( tCol.first.cstr(), false );
		auto tTypeBlock = m_dBuf.Object(false);
		m_dBuf.NamedVal ( "type", eType );
		m_dColumns.Add ( tCol );
	}

	void Add ( BYTE ) override {}

	const JsonEscapedBuilder & Finish()
	{
		m_dBuf.FinishBlocks();
		return m_dBuf;
	}

private:
	JsonEscapedBuilder m_dBuf;
	CSphVector<ColumnNameType_t> m_dColumns;
	int m_iTotalRows = 0;
	int m_iCol = 0;

	void AddDataColumn()
	{
		m_dBuf.AppendName ( m_dColumns[m_iCol].first.cstr(), false );
		++m_iCol;
	}

	void DataFinish ( int iTotal, const char* szError, const char* szWarning )
	{
		m_dBuf.NamedVal ( "total", iTotal );
		m_dBuf.NamedString ( "error", szError );
		m_dBuf.NamedString ( "warning", szWarning );
		m_iCol = 0;
		m_dColumns.Reset();
	}
};

/* Below is typical answer sent back by sql endpoint query mode=raw
[{
"columns":[{"id":{"type":"long long"}},{"proto":{"type":"string"}},{"state":{"type":"string"}},{"host":{"type":"string"}},{"connid":{"type":"long long"}},{"killed":{"type":"string"}},{"last cmd":{"type":"string"}}],
"data":[
{"id":2,"proto":"http","state":"query","host":"127.0.0.1:50787","connid":9,"killed":"0","last cmd":"select"},
{"id":1,"proto":"mysql,ssl","state":"query","host":"127.0.0.1:50514","connid":1,"killed":"0","last cmd":"show queries"}
],
"total":2,
"error":"",
"warning":""
}]
 */


void ConvertJsonDataset ( const JsonObj_c & tRoot, const char * sStmt, RowBuffer_i & tOut )
{
	assert ( tRoot.IsArray() );

	int iItem = 0;
	int iItemsCount = tRoot.Size();
	CSphString sParseError;

	for ( const auto & tItem : tRoot )
	{
		int iTotal = 0;
		CSphString sError, sWarning;

		if ( !tItem.FetchIntItem ( iTotal, "total", sParseError, true ) )
		{
			tOut.Error ( sParseError.cstr() );
			break;
		}
		if ( !tItem.FetchStrItem ( sError, "error", sParseError, true ) )
		{
			tOut.Error ( sParseError.cstr() );
			break;
		}
		if ( !tItem.FetchStrItem ( sWarning, "warning", sParseError, true ) )
		{
			tOut.Error ( sParseError.cstr() );
			break;
		}

		if ( !sError.IsEmpty() )
		{
			LogSphinxqlError ( sStmt, FromStr ( sError ) );
			session::GetClientSession()->m_sError = sError;
			session::GetClientSession()->m_tLastMeta.m_sError = sError;
			tOut.Error ( sError.cstr() );
			break;
		}
		if ( !iItem ) // only zero result set sets meta
		{
			session::GetClientSession()->m_tLastMeta.m_iTotalMatches = iTotal;
			session::GetClientSession()->m_tLastMeta.m_sWarning = sWarning;
		}

		using ColType_t = std::pair<CSphString, MysqlColumnType_e>;
		CSphVector<ColType_t> dSqlColumns;
		assert ( tItem.IsObj() );
		JsonObj_c tColumnsNode = tItem.GetArrayItem ( "columns", sParseError, true );
		for ( const auto & tColumnNode : tColumnsNode )
		{
			assert ( tColumnNode.IsObj() ); // like {"id":{"type":"long long"}}
			for ( const auto & tColumn : tColumnNode )
			{
				CSphString sType;
				if ( !tColumn.FetchStrItem ( sType, "type", sParseError, false ) )
					return;
				auto eType = GetMysqlTypeByName ( sType );
				dSqlColumns.Add ( { tColumn.Name(), eType } );
			}
		}


		// fill headers
		if ( !dSqlColumns.IsEmpty() )
		{
			tOut.HeadBegin ();
			dSqlColumns.for_each ( [&] ( const auto& tColumn ) { tOut.HeadColumn ( tColumn.first.cstr(), tColumn.second ); } );
			tOut.HeadEnd();
		} else
		{
			// just simple OK reply without table
			tOut.Ok ( iTotal, ( sWarning.IsEmpty() ? 0 : 1 ) );
			break;
		}

		JsonObj_c tDataNodes = tItem.GetItem ( "data" );
		for ( const auto & tDataRow : tDataNodes )
		{
			assert ( tDataRow.IsObj() ); // like {"id":2,"proto":"http","state":"query","host":"127.0.0.1:50787","connid":9,"killed":"0","last cmd":"select"}
			for ( const auto & tDataCol : tDataRow )
			{
				if ( tDataCol.IsInt () )
					tOut.PutNumAsString ( tDataCol.IntVal() );
				else if ( tDataCol.IsDbl () )
					tOut.PutDoubleAsString ( tDataCol.DblVal() );
				else
					tOut.PutString ( tDataCol.StrVal() );
			}
			if ( !tOut.Commit() )
				return;
		}

		tOut.Eof ( iItem+1!=iItemsCount, ( sWarning.IsEmpty() ? 0 : 1 ) );
		iItem++;
	}
}

class HttpRawSqlHandler_c final: public HttpHandler_c, public HttpOptionTrait_t
{
	Str_t m_sQuery;

public:
	explicit HttpRawSqlHandler_c ( Str_t sQuery, const OptionsHash_t & tOptions )
		: HttpOptionTrait_t ( tOptions )
		, m_sQuery ( sQuery )
	{}

	bool Process () final
	{
		TRACE_CONN ( "conn", "HttpRawSqlHandler_c::Process" );
		if ( IsEmpty ( m_sQuery ) )
		{
			ReportError ( "query missing", EHTTP_STATUS::_400 );
			return false;
		}

		if ( IsBuddyQuery ( m_tOptions ) )
			session::SetQueryDisableLog();

		JsonRowBuffer_c tOut;
		session::Execute ( m_sQuery, tOut );
		if ( tOut.IsError() )
		{
			ReportError ( tOut.GetError().scstr(), EHTTP_STATUS::_500 );
			return false;
		}
		BuildReply ( tOut.Finish(), EHTTP_STATUS::_200 );
		return true;
	}
};


class HttpHandler_JsonSearch_c : public HttpSearchHandler_c
{
	Str_t m_sQuery;

public:
	explicit HttpHandler_JsonSearch_c ( Str_t sQuery, const OptionsHash_t &	tOptions )
		: HttpSearchHandler_c ( tOptions )
		, m_sQuery ( sQuery )
	{}

	std::unique_ptr<QueryParser_i> PreParseQuery() override
	{
		// TODO!!! add parsing collation from the query
		m_tParsed.m_tQuery.m_eCollation = session::GetCollation();

		if ( !sphParseJsonQuery ( m_sQuery, m_tParsed ) )
		{
			ReportError ( TlsMsg::szError(), EHTTP_STATUS::_400 );
			return nullptr;
		}

		m_eQueryType = QUERY_JSON;
		return sphCreateJsonQueryParser();
	}
};


class HttpJsonTxnTraits_c
{
protected:
	HttpJsonTxnTraits_c() = default;
	explicit HttpJsonTxnTraits_c ( ResultSetFormat_e eFormat )
		: m_eFormat ( eFormat )
	{}

	void ProcessBegin ( const CSphString& sIndex )
	{
		// for now - only local mutable indexes are suitable
		{
			auto pIndex = GetServed ( sIndex );
			if ( !ServedDesc_t::IsMutable ( pIndex ) )
				return;
		}

		HttpErrorReporter_c tReporter;
		sphHandleMysqlBegin ( tReporter, FromStr (sIndex) );
		m_iInserts = 0;
		m_iUpdates = 0;
	}

	bool ProcessCommitRollback ( Str_t sIndex, DocID_t & tDocId, JsonObj_c & tResult, CSphString & sError ) const
	{
		HttpErrorReporter_c tReporter;
		sphHandleMysqlCommitRollback ( tReporter, sIndex, true );

		if ( tReporter.IsError() )
		{
			sError = tReporter.GetError();
			tResult = sphEncodeInsertErrorJson ( sIndex.first, sError.cstr(), m_eFormat );
		} else
		{
			auto iDeletes = tReporter.GetAffectedRows();
			auto dLastIds = session::LastIds();
			if ( !dLastIds.IsEmpty() )
				tDocId = dLastIds[0];
			tResult = sphEncodeTxnResultJson ( sIndex.first, tDocId, m_iInserts, iDeletes, m_iUpdates, m_eFormat );
		}
		return !tReporter.IsError();
	}

	int m_iInserts = 0;
	int m_iUpdates = 0;
	const ResultSetFormat_e m_eFormat = ResultSetFormat_e::MntSearch;
};

static bool ProcessInsert ( SqlStmt_t & tStmt, DocID_t & tDocId, JsonObj_c & tResult, CSphString & sError, ResultSetFormat_e eFormat )
{
	HttpErrorReporter_c tReporter;
	sphHandleMysqlInsert ( tReporter, tStmt );

	if ( tReporter.IsError() )
	{
		sError = tReporter.GetError();
		tResult = sphEncodeInsertErrorJson ( tStmt.m_sIndex.cstr(), sError.cstr(), eFormat );
	} else
	{
		auto dLastIds = session::LastIds();
		if ( !dLastIds.IsEmpty() )
			tDocId = dLastIds[0];
		tResult = sphEncodeInsertResultJson ( tStmt.m_sIndex.cstr(), tStmt.m_eStmt == STMT_REPLACE, tDocId, eFormat );
	}

	return !tReporter.IsError();
}

static bool ProcessDelete ( Str_t sRawRequest, const SqlStmt_t& tStmt, DocID_t tDocId, JsonObj_c & tResult, CSphString & sError, ResultSetFormat_e eFormat )
{
	HttpErrorReporter_c tReporter;
	sphHandleMysqlDelete ( tReporter, tStmt, std::move ( sRawRequest ) );

	if ( tReporter.IsError() )
	{
		sError = tReporter.GetError();
		tResult = sphEncodeInsertErrorJson ( tStmt.m_sIndex.cstr(), sError.cstr(), eFormat );
	} else
	{
		tResult = sphEncodeDeleteResultJson ( tStmt.m_sIndex.cstr(), tDocId, tReporter.GetAffectedRows(), eFormat );
	}

	return !tReporter.IsError();
}

class HttpHandler_JsonInsert_c final : public HttpHandler_c
{
	Str_t m_sQuery;
	bool m_bReplace;

public:
	HttpHandler_JsonInsert_c ( Str_t sQuery, bool bReplace )
		: m_sQuery ( sQuery )
		, m_bReplace ( bReplace )
	{}

	bool Process () final
	{
		TRACE_CONN ( "conn", "HttpHandler_JsonInsert_c::Process" );
		SqlStmt_t tStmt;
		DocID_t tDocId = 0;
		if ( !sphParseJsonInsert ( m_sQuery, tStmt, tDocId, m_bReplace, m_sError ) )
		{
			ReportError ( nullptr, HttpErrorType_e::Parse, EHTTP_STATUS::_400, tStmt.m_sIndex.cstr() );
			return false;
		}

		tStmt.m_sEndpoint = HttpEndpointToStr ( m_bReplace ? EHTTP_ENDPOINT::JSON_REPLACE : EHTTP_ENDPOINT::JSON_INSERT );
		JsonObj_c tResult = JsonNull;
		bool bResult = ProcessInsert ( tStmt, tDocId, tResult, m_sError, ResultSetFormat_e::MntSearch );

		if ( bResult )
			BuildReply ( tResult.AsString(), bResult ? EHTTP_STATUS::_200 : EHTTP_STATUS::_409 );
		else
			ReportError ( nullptr, HttpErrorType_e::ActionRequestValidation, EHTTP_STATUS::_409, tStmt.m_sIndex.cstr() );

		return bResult;
	}
};


class HttpJsonUpdateTraits_c
{
	int m_iLastUpdated = 0;

protected:
	HttpJsonUpdateTraits_c() = default;
	explicit HttpJsonUpdateTraits_c ( ResultSetFormat_e eFormat )
		: m_eFormat ( eFormat )
	{}

	bool ProcessUpdate ( Str_t sRawRequest, const SqlStmt_t & tStmt, DocID_t tDocId, JsonObj_c & tResult, CSphString & sError )
	{
		HttpErrorReporter_c tReporter;
		sphHandleMysqlUpdate ( tReporter, tStmt, sRawRequest );

		if ( tReporter.IsError() )
		{
			sError = tReporter.GetError();
			tResult = sphEncodeInsertErrorJson ( tStmt.m_sIndex.cstr(), sError.cstr(), m_eFormat );
		} else
		{
			tResult = sphEncodeUpdateResultJson ( tStmt.m_sIndex.cstr(), tDocId, tReporter.GetAffectedRows(), m_eFormat );
		}

		m_iLastUpdated = tReporter.GetAffectedRows();

		return !tReporter.IsError();
	}

	int GetLastUpdated() const
	{
		return m_iLastUpdated;
	}

	const ResultSetFormat_e m_eFormat = ResultSetFormat_e::MntSearch;
};

static void SetQueryOptions ( const OptionsHash_t & hOpts, SqlStmt_t & tStmt )
{
	if ( tStmt.m_eStmt==STMT_UPDATE || tStmt.m_eStmt==STMT_DELETE )
	{
		const CSphString * pRawQuery = hOpts ( "raw_query" );
		if ( pRawQuery )
			tStmt.m_sRawQuery = *pRawQuery;
		const CSphString * pFullUrl = hOpts  ( "full_url" );
		if ( pFullUrl )
			tStmt.m_sFullUrl = *pFullUrl;
	}
}

class HttpHandler_JsonUpdate_c : public HttpHandler_c, HttpJsonUpdateTraits_c, HttpOptionTrait_t
{
protected:
	Str_t m_sQuery;

public:
	explicit HttpHandler_JsonUpdate_c ( Str_t sQuery, const OptionsHash_t & tOptions )
		: HttpOptionTrait_t ( tOptions )
		, m_sQuery ( sQuery )
	{}

	bool Process () final
	{
		TRACE_CONN ( "conn", "HttpHandler_JsonUpdate_c::Process" );
		SqlStmt_t tStmt;
		tStmt.m_bJson = true;
		tStmt.m_tQuery.m_eQueryType = QUERY_JSON;
		tStmt.m_sEndpoint = HttpEndpointToStr ( EHTTP_ENDPOINT::JSON_UPDATE );
		tStmt.m_eStmt = STMT_UPDATE;

		DocID_t tDocId = 0;
		if ( !ParseQuery ( tStmt, tDocId ) )
		{
			ReportError ( nullptr, HttpErrorType_e::Parse, EHTTP_STATUS::_400, tStmt.m_sIndex.cstr() );
			return false;
		}

		SetQueryOptions ( m_tOptions, tStmt );

		JsonObj_c tResult = JsonNull;
		bool bResult = ProcessQuery ( tStmt, tDocId, tResult );

		if ( bResult )
			BuildReply ( tResult.AsString(), bResult ? EHTTP_STATUS::_200 : EHTTP_STATUS::_409 );
		else
			ReportError ( nullptr, HttpErrorType_e::ActionRequestValidation, EHTTP_STATUS::_409, tStmt.m_sIndex.cstr() );

		return bResult;
	}

protected:
	virtual bool ParseQuery ( SqlStmt_t & tStmt, DocID_t & tDocId )
	{
		return sphParseJsonUpdate ( m_sQuery, tStmt, tDocId, m_sError );
	}

	virtual bool ProcessQuery ( const SqlStmt_t & tStmt, DocID_t tDocId, JsonObj_c & tResult )
	{
		return ProcessUpdate ( m_sQuery, tStmt, tDocId, tResult, m_sError );
	}
};


class HttpHandler_JsonDelete_c final : public HttpHandler_JsonUpdate_c
{
public:
	explicit HttpHandler_JsonDelete_c ( Str_t sQuery, const OptionsHash_t & tOptions )
		: HttpHandler_JsonUpdate_c ( sQuery, tOptions )
	{}

protected:
	bool ParseQuery ( SqlStmt_t & tStmt, DocID_t & tDocId ) final
	{
		tStmt.m_sEndpoint = HttpEndpointToStr ( EHTTP_ENDPOINT::JSON_DELETE );
		tStmt.m_eStmt = STMT_DELETE;
		return sphParseJsonDelete ( m_sQuery, tStmt, tDocId, m_sError );
	}

	bool ProcessQuery ( const SqlStmt_t & tStmt, DocID_t tDocId, JsonObj_c & tResult ) final
	{
		return ProcessDelete ( m_sQuery, tStmt, tDocId, tResult, m_sError, ResultSetFormat_e::MntSearch );
	}
};

// stream for lines - each 'Read()' returns single line (lines split by \r or \n)
class NDJsonStream_c
{
	CharStream_c & m_tIn;
	CSphVector<char> m_dLastChunk;
	Str_t m_sCurChunk { dEmptyStr };
	bool m_bDone;

	int m_iJsons = 0;

public:
	explicit NDJsonStream_c ( CharStream_c& tIn )
		: m_tIn { tIn }
		, m_bDone { m_tIn.Eof() }
	{}

	inline bool Eof() const { return m_bDone;}

	Str_t ReadLine()
	{
		assert ( !m_bDone );
		while (true)
		{
			if ( IsEmpty ( m_sCurChunk ) )
			{
				if ( m_tIn.Eof() || m_tIn.GetError() )
					break;
				m_sCurChunk = m_tIn.Read();
			}

			const char* szLine = m_sCurChunk.first;
			const char* pEnd = szLine + m_sCurChunk.second;
			const char* p = szLine;

			while ( p<pEnd && *p!='\r' && *p!='\n' )
				++p;

			if ( p==pEnd )
			{
				m_dLastChunk.Append ( szLine, p-szLine );
				m_sCurChunk = dEmptyStr;
				continue;
			}

			*( const_cast<char*> ( p ) ) = '\0';
			++p;
			m_sCurChunk = { p, pEnd - p };

			Str_t sResult;
			if ( m_dLastChunk.IsEmpty () )
			{
				sResult =  { szLine, p - szLine - 1 };
// that is commented out, as we better will deal with empty strings on parser level instead.
//				if ( IsEmpty ( sResult ) )
//					continue;
				++m_iJsons;
				HTTPINFO << "chunk " << m_iJsons << " '" << Data2Log ( sResult ) << "'";;
			} else
			{
				m_dLastChunk.Append ( szLine, p - szLine );
				sResult = m_dLastChunk;
				--sResult.second; // exclude terminating \0
				m_dLastChunk.Resize ( 0 );
				++m_iJsons;
				HTTPINFO << "chunk last " << m_iJsons << " '" << Data2Log ( sResult ) << "'";;
			}
			return sResult;
		}

		m_bDone = true;
		m_dLastChunk.Add ( '\0' );
		m_dLastChunk.Resize ( m_dLastChunk.GetLength() - 1 );
		Str_t sResult = m_dLastChunk;
		++m_iJsons;
		HTTPINFO << "chunk termination " << m_iJsons << " '" << Data2Log ( sResult ) << "'";
		return sResult;
	}

	bool GetError() const { return m_tIn.GetError(); }
	const CSphString & GetErrorMessage() const { return m_tIn.GetErrorMessage(); }
};

static Str_t TrimHeadSpace ( Str_t tLine )
{
	if ( IsEmpty ( tLine ) )
		return tLine;

	const char * sCur = tLine.first;
	const char * sEnd = sCur + tLine.second;
	while ( sCur<sEnd && sphIsSpace ( *sCur ) )
		sCur++;

	return Str_t { sCur, sEnd-sCur };
}

class HttpHandler_JsonBulk_c : public HttpHandler_c, public HttpJsonUpdateTraits_c, public HttpJsonTxnTraits_c
{
protected:
	NDJsonStream_c m_tSource;
	const OptionsHash_t& m_tOptions;

public:
	HttpHandler_JsonBulk_c ( CharStream_c& tSource, const OptionsHash_t & tOptions )
		: m_tSource ( tSource )
		, m_tOptions ( tOptions )
	{}

	bool Process ()
	{
		TRACE_CONN ( "conn", "HttpHandler_JsonBulk_c::Process" );

		if ( !CheckNDJson() )
			return false;

		JsonObj_c tResults ( true );
		bool bResult = false;
		int iCurLine = 0;
		int iLastTxStartLine = 0;

		auto FinishBulk = [&, this] ( EHTTP_STATUS eStatus = EHTTP_STATUS::_200 ) {
			JsonObj_c tRoot;
			tRoot.AddItem ( "items", tResults );
			tRoot.AddInt ( "current_line", iCurLine );
			tRoot.AddInt ( "skipped_lines", iCurLine - iLastTxStartLine );
			tRoot.AddBool ( "errors", !bResult );
			tRoot.AddStr ( "error", m_sError.IsEmpty() ? "" : m_sError );
			if ( eStatus == EHTTP_STATUS::_200 && !bResult )
				eStatus = EHTTP_STATUS::_500;
			BuildReply ( tRoot.AsString(), eStatus );
			HTTPINFO << "inserted  " << iCurLine;
			return bResult;
		};

		auto AddResult = [&tResults] ( const char* szStmt, JsonObj_c& tResult ) {
			JsonObj_c tItem;
			tItem.AddItem ( szStmt, tResult );
			tResults.AddItem ( tItem );
		};

		if ( m_tSource.Eof() )
			return FinishBulk();

		// originally we execute txn for single index
		// if there is combo, we fall back to query-by-query commits

		CSphString sTxnIdx;
		CSphString sStmt;

		while ( !m_tSource.Eof() )
		{
			auto tQuery = m_tSource.ReadLine();
			tQuery = TrimHeadSpace ( tQuery ); // could be a line with only whitespace chars
			++iCurLine;

			DocID_t tDocId = 0;
			JsonObj_c tResult = JsonNull;

			if ( IsEmpty ( tQuery ) )
			{
				if ( session::IsInTrans() )
				{
					assert ( !sTxnIdx.IsEmpty() );
					// empty query finishes current txn
					bResult = ProcessCommitRollback ( FromStr ( sTxnIdx ), tDocId, tResult, m_sError );
					AddResult ( "bulk", tResult );
					if ( !bResult )
						break;
					sTxnIdx = "";
					iLastTxStartLine = iCurLine;
				}
				continue;
			}

			bResult = false;
			auto& tCrashQuery = GlobalCrashQueryGetRef();
			tCrashQuery.m_dQuery = { (const BYTE*) tQuery.first, tQuery.second };
			const char* szStmt = tQuery.first;
			SqlStmt_t tStmt;
			tStmt.m_bJson = true;

			CSphString sQuery;
			if ( !sphParseJsonStatement ( szStmt, tStmt, sStmt, sQuery, tDocId, m_sError ) )
			{
				HTTPINFO << "inserted " << iCurLine << ", error: " << m_sError;
				return FinishBulk ( EHTTP_STATUS::_400 );
			}

			if ( sTxnIdx.IsEmpty() )
			{
				sTxnIdx = tStmt.m_sIndex;
				ProcessBegin ( sTxnIdx );
			}
			else if ( session::IsInTrans() && sTxnIdx!=tStmt.m_sIndex )
			{
				assert ( !sTxnIdx.IsEmpty() );
				// we should finish current txn, as we got another index
				bResult = ProcessCommitRollback ( FromStr ( sTxnIdx ), tDocId, tResult, m_sError );
				AddResult ( "bulk", tResult );
				if ( !bResult )
					break;
				sTxnIdx = tStmt.m_sIndex;
				ProcessBegin ( sTxnIdx );
				iLastTxStartLine = iCurLine;
			}

			SetQueryOptions ( m_tOptions, tStmt );

			switch ( tStmt.m_eStmt )
			{
			case STMT_INSERT:
			case STMT_REPLACE:
				bResult = ProcessInsert ( tStmt, tDocId, tResult, m_sError, ResultSetFormat_e::MntSearch );
				if ( bResult )
					++m_iInserts;
				break;

			case STMT_UPDATE:
				tStmt.m_sEndpoint = HttpEndpointToStr ( EHTTP_ENDPOINT::JSON_UPDATE );
				bResult = ProcessUpdate ( FromStr ( sQuery ), tStmt, tDocId, tResult, m_sError );
				if ( bResult )
					m_iUpdates += GetLastUpdated();
				break;

			case STMT_DELETE:
				tStmt.m_sEndpoint = HttpEndpointToStr ( EHTTP_ENDPOINT::JSON_DELETE );
				bResult = ProcessDelete ( FromStr ( sQuery ), tStmt, tDocId, tResult, m_sError, ResultSetFormat_e::MntSearch );
				break;

			default:
				HTTPINFO << "inserted  " << iCurLine << ", got unknown statement:" << (int)tStmt.m_eStmt;
				return FinishBulk ( EHTTP_STATUS::_400 );
			}

			if ( !bResult || !session::IsInTrans() )
				AddResult ( sStmt.cstr(), tResult );

			// no further than the first error
			if ( !bResult )
				break;

			if ( !session::IsInTrans() )
				iLastTxStartLine = iCurLine;
		}

		if ( bResult && session::IsInTrans() )
		{
			assert ( !sTxnIdx.IsEmpty() );
			// We're in txn - that is, nothing committed, and we should do it right now
			JsonObj_c tResult;
			DocID_t tDocId = 0;
			bResult = ProcessCommitRollback ( FromStr ( sTxnIdx ), tDocId, tResult, m_sError );
			AddResult ( "bulk", tResult );
			if ( bResult )
				iLastTxStartLine = iCurLine;
		}

		session::SetInTrans ( false );
		HTTPINFO << "inserted  " << iCurLine << " result: " << (int)bResult << ", error:" << m_sError;
		return FinishBulk();
	}

private:
	bool CheckNDJson()
	{
		if ( !m_tOptions.Exists ( "content-type" ) )
		{
			ReportError ( "Content-Type must be set", HttpErrorType_e::Parse, EHTTP_STATUS::_400 );
			return false;
		}

		auto sContentType = m_tOptions["content-type"].ToLower();
		auto dParts = sphSplit ( sContentType.cstr(), ";" );
		if ( dParts.IsEmpty() || dParts[0] != "application/x-ndjson" )
		{
			ReportError ( "Content-Type must be application/x-ndjson", HttpErrorType_e::Parse, EHTTP_STATUS::_400 );
			return false;
		}

		return true;
	}
};

class HttpHandlerPQ_c final : public HttpHandler_c, public HttpOptionTrait_t
{
	Str_t m_sQuery;
public:
	HttpHandlerPQ_c ( Str_t sQuery, const OptionsHash_t & tOptions )
		: HttpOptionTrait_t ( tOptions )
		, m_sQuery ( sQuery )
	{}

	bool Process () final;

private:
	// FIXME!!! handle replication for InsertOrReplaceQuery and Delete
	bool DoCallPQ ( const CSphString & sIndex, const JsonObj_c & tPercolate, bool bVerbose );
	bool InsertOrReplaceQuery ( const CSphString& sIndex, const JsonObj_c& tJsonQuery, const JsonObj_c& tRoot, CSphString* pUID, bool bReplace );
	bool ListQueries ( const CSphString & sIndex );
	bool Delete ( const CSphString & sIndex, const JsonObj_c & tRoot );
};

struct BulkDoc_t
{
	CSphString m_sAction;
	CSphString m_sIndex;
	DocID_t m_tDocid { 0 };
	Str_t m_tDocLine;
};

struct BulkTnx_t
{
	int m_iFrom { -1 };
	int m_iCount { 0 };
};


class HttpHandlerEsBulk_c : public HttpCompatBaseHandler_c, public HttpJsonUpdateTraits_c, public HttpJsonTxnTraits_c
{
public:
	HttpHandlerEsBulk_c ( Str_t sBody, int iReqType, const SmallStringHash_T<CSphString> & hOpts )
		: HttpCompatBaseHandler_c ( sBody, iReqType, hOpts )
		, HttpJsonUpdateTraits_c ( ResultSetFormat_e::ES )
		, HttpJsonTxnTraits_c ( ResultSetFormat_e::ES )
	{}

	bool Process () override;

private:
	bool ProcessTnx ( const VecTraits_T<BulkTnx_t> & dTnx, VecTraits_T<BulkDoc_t> & dDocs, JsonObj_c & tItems );
	bool Validate();
	void ReportLogError ( const char * sError, HttpErrorType_e eType , EHTTP_STATUS eStatus, bool bLogOnly );
};

static std::unique_ptr<HttpHandler_c> CreateHttpHandler ( EHTTP_ENDPOINT eEndpoint, CharStream_c & tSource, Str_t & sQuery, OptionsHash_t & tOptions, http_method eRequestType )
{
	const CSphString * pOption = nullptr;
	sQuery = dEmptyStr;

	auto SetQuery = [&sQuery] ( Str_t&& sData ) {
		auto& tCrashQuery = GlobalCrashQueryGetRef();
		tCrashQuery.m_dQuery = { (const BYTE*)sData.first, sData.second };
		sQuery = sData;
	};

	// SPH_HTTP_ENDPOINT_SQL SPH_HTTP_ENDPOINT_CLI SPH_HTTP_ENDPOINT_CLI_JSON these endpoints url-encoded, all others are plain json, and we don't want to waste time pre-parsing them
	if ( eEndpoint== EHTTP_ENDPOINT::SQL || eEndpoint== EHTTP_ENDPOINT::CLI || eEndpoint== EHTTP_ENDPOINT::CLI_JSON )
	{
		auto sWholeData = tSource.ReadAll();
		if ( tSource.GetError() )
			return nullptr;
		if ( eEndpoint == EHTTP_ENDPOINT::SQL )
		{
			const std::array<Str_t, 3> sQueries { FROMS ( "query=" ), FROMS ( "mode=raw&query=" ), FROMS ( "raw_response=true&query=" ) };
			if ( std::any_of ( sQueries.cbegin(), sQueries.cend(), [&sWholeData] ( const Str_t& S ) { return sWholeData.second >= S.second && 0 == memcmp ( sWholeData.first, S.first, S.second ); } ) )
			{
				DecodeAndStoreRawQuery ( tOptions, sWholeData );
				HttpRequestParser_c::ParseList ( sWholeData, tOptions );
			} else
			{
				StoreRawQuery ( tOptions, { sWholeData } );
				tOptions.Add ( sWholeData, "query" );
			}
		} else
			StoreRawQuery ( tOptions, { sWholeData } );
	}

	switch ( eEndpoint )
	{
	case EHTTP_ENDPOINT::SQL:
		{
			bool bRawMode = false;
			pOption = tOptions ( "mode" );
			if ( pOption )
				bRawMode = *pOption == "raw";
			else
			{
				pOption = tOptions ( "raw_response" );
				if ( pOption )
					bRawMode = *pOption == "true";
			}

			if ( bRawMode )
			{
				auto pQuery = tOptions ( "query" );
				if ( pQuery )
					SetQuery ( FromStr ( *pQuery ) );
				return std::make_unique<HttpRawSqlHandler_c> ( sQuery, tOptions ); // non-json
			}
			else
			{
				pOption = tOptions ( "raw_query" );
				if ( pOption )
					SetQuery ( FromStr (*pOption) );

				return std::make_unique<HttpSearchHandler_SQL_c> ( tOptions ); // non-json
			}
		}

	case EHTTP_ENDPOINT::CLI:
	case EHTTP_ENDPOINT::CLI_JSON:
		{
			pOption = tOptions ( "raw_query" );
			auto tQuery = pOption ? FromStr ( *pOption ) : dEmptyStr;
			SetQuery ( std::move ( tQuery ) );
			return std::make_unique<HttpRawSqlHandler_c> ( sQuery, tOptions ); // non-json
		}

	case EHTTP_ENDPOINT::JSON_SEARCH:
		SetQuery ( tSource.ReadAll() );
		return std::make_unique<HttpHandler_JsonSearch_c> ( sQuery, tOptions ); // json

	case EHTTP_ENDPOINT::JSON_INDEX:
	case EHTTP_ENDPOINT::JSON_CREATE:
	case EHTTP_ENDPOINT::JSON_INSERT:
	case EHTTP_ENDPOINT::JSON_REPLACE:
		SetQuery ( tSource.ReadAll() );
		if ( tSource.GetError() )
			return nullptr;
		else
			return std::make_unique<HttpHandler_JsonInsert_c> ( sQuery, eEndpoint==EHTTP_ENDPOINT::JSON_INDEX || eEndpoint==EHTTP_ENDPOINT::JSON_REPLACE ); // json

	case EHTTP_ENDPOINT::JSON_UPDATE:
		SetQuery ( tSource.ReadAll() );
		if ( tSource.GetError() )
			return nullptr;
		else
			return std::make_unique<HttpHandler_JsonUpdate_c> ( sQuery, tOptions ); // json

	case EHTTP_ENDPOINT::JSON_DELETE:
		SetQuery ( tSource.ReadAll() );
		if ( tSource.GetError() )
			return nullptr;
		else
			return std::make_unique<HttpHandler_JsonDelete_c> ( sQuery, tOptions ); // json

	case EHTTP_ENDPOINT::JSON_BULK:
		return std::make_unique<HttpHandler_JsonBulk_c> ( tSource, tOptions ); // json

	case EHTTP_ENDPOINT::PQ:
		SetQuery ( tSource.ReadAll() );
		if ( tSource.GetError() )
			return nullptr;
		else
			return std::make_unique<HttpHandlerPQ_c> ( sQuery, tOptions ); // json

	case EHTTP_ENDPOINT::ES_BULK:
		SetQuery ( tSource.ReadAll() );
		if ( tSource.GetError() )
			return nullptr;
		else
			return std::make_unique<HttpHandlerEsBulk_c> ( sQuery, eRequestType, tOptions );

	case EHTTP_ENDPOINT::TOTAL:
		SetQuery ( tSource.ReadAll() );
		if ( tSource.GetError() )
			return nullptr;
		else
			return CreateCompatHandler ( sQuery, eRequestType, tOptions );

	default:
		break;
	}

	return nullptr;
}

HttpProcessResult_t ProcessHttpQuery ( CharStream_c & tSource, Str_t & sSrcQuery, OptionsHash_t & hOptions, CSphVector<BYTE> & dResult, bool bNeedHttpResponse, http_method eRequestType )
{
	TRACE_CONN ( "conn", "ProcessHttpQuery" );

	HttpProcessResult_t tRes;

	const CSphString & sEndpoint = hOptions["endpoint"];
	tRes.m_eEndpoint = StrToHttpEndpoint ( sEndpoint );

	std::unique_ptr<HttpHandler_c> pHandler = CreateHttpHandler ( tRes.m_eEndpoint, tSource, sSrcQuery, hOptions, eRequestType );
	if ( !pHandler )
	{
		if ( tRes.m_eEndpoint == EHTTP_ENDPOINT::INDEX )
		{
			HttpHandlerIndexPage ( dResult );
		} else
		{
			DumpHttp ( eRequestType, sEndpoint, tSource.ReadAll() );
			tRes.m_eReplyHttpCode = EHTTP_STATUS::_501;
			if ( tSource.GetError() )
			{
				tRes.m_sError = tSource.GetErrorMessage();
				if ( tRes.m_sError.Begins ( "length out of bounds" ) )
					tRes.m_eReplyHttpCode = EHTTP_STATUS::_413;
			} else
			{
				tRes.m_sError.SetSprintf ( "/%s - unsupported endpoint", sEndpoint.cstr() );
			}
			sphHttpErrorReply ( dResult, tRes.m_eReplyHttpCode, tRes.m_sError.cstr() );
		}
		return tRes;
	}
	// will be processed by buddy right after source data got parsed
	if ( tRes.m_eEndpoint == EHTTP_ENDPOINT::CLI )
		return tRes;

	pHandler->SetErrorFormat ( bNeedHttpResponse );
	tRes.m_bOk = pHandler->Process();
	tRes.m_sError = pHandler->GetError();
	tRes.m_eReplyHttpCode = pHandler->GetStatusCode();
	dResult = std::move ( pHandler->GetResult() );

	return tRes;
}

void ProcessHttpJsonQuery ( const CSphString & sQuery, OptionsHash_t & hOptions, CSphVector<BYTE> & dResult )
{
	http_method eReqType = HTTP_POST;

	BlobStream_c tQuery ( sQuery );
	Str_t sSrcQuery;
	HttpProcessResult_t tRes = ProcessHttpQuery ( tQuery, sSrcQuery, hOptions, dResult, false, eReqType );
	ProcessHttpQueryBuddy ( tRes, sSrcQuery, hOptions, dResult, false, eReqType );
}

static bool IsCompressed ( const OptionsHash_t & hOptions )
{
	const CSphString * pEncoding = hOptions ( "content-encoding" );
	if ( !pEncoding )
		return false;

	return ( *pEncoding=="gzip" );
}

bool HttpRequestParser_c::ProcessClientHttp ( AsyncNetInputBuffer_c& tIn, CSphVector<BYTE>& dResult )
{
	assert ( !m_szError );
	std::unique_ptr<CharStream_c> pSource;
	bool bCompressed = IsCompressed ( m_hOptions );

	if ( m_tParser.flags & F_CHUNKED )
	{
		pSource = std::make_unique<ChunkedSocketStream_c> ( &tIn, &m_tParser, m_bBodyDone, std::move ( m_dParsedBodies ), m_iLastParsed );
	} else
	{
		// for non-chunked - need to throw out beginning of the packet (with header). Only body rest in the buffer.
		tIn.PopTail ( m_iLastParsed - ParsedBodyLength() );
		int iFullLength = ParsedBodyLength() + ( (int)m_tParser.content_length > 0 ? (int)m_tParser.content_length : 0 );
		pSource = std::make_unique<RawSocketStream_c> ( &tIn, iFullLength, bCompressed );
	}

	EHTTP_ENDPOINT eEndpoint = StrToHttpEndpoint ( m_sEndpoint );
	if ( IsLogManagementEnabled() && eEndpoint==EHTTP_ENDPOINT::TOTAL && m_sEndpoint.Ends ( "_bulk" ) )
		eEndpoint = EHTTP_ENDPOINT::ES_BULK;

	HttpProcessResult_t tRes;
	Str_t sSrcQuery;

	if ( bCompressed && !HasGzip() )
	{
		// 14.11 Content-Encoding
		// If the content-coding of an entity in a request message is not acceptable to the origin server, the server SHOULD respond with a status code of 415 (Unsupported Media Type)
		tRes.m_eReplyHttpCode = EHTTP_STATUS::_415;
		tRes.m_bOk = false;
		tRes.m_sError = "gzip error: unpack is not supported, rebuild with zlib";
		sphHttpErrorReply ( dResult, tRes.m_eReplyHttpCode, tRes.m_sError.cstr() );

	} else if ( bCompressed && ( m_tParser.flags & F_CHUNKED ) )
	{
		tRes.m_eReplyHttpCode = EHTTP_STATUS::_415;
		tRes.m_bOk = false;
		tRes.m_sError = "can not process chunked transfer-coding along with gzip";
		sphHttpErrorReply ( dResult, tRes.m_eReplyHttpCode, tRes.m_sError.cstr() );

	} else
	{
		tRes = ProcessHttpQuery ( *pSource, sSrcQuery, m_hOptions, dResult, true, m_eType );
	}

	return ProcessHttpQueryBuddy ( tRes, sSrcQuery, m_hOptions, dResult, true, m_eType );
}

void sphHttpErrorReply ( CSphVector<BYTE> & dData, EHTTP_STATUS eCode, const char * szError )
{
	JsonObj_c tErr;
	tErr.AddStr ( "error", szError );
	CSphString sJsonError = tErr.AsString();
	HttpBuildReply ( dData, eCode, FromStr (sJsonError), false );
}

static void EncodePercolateMatchResult ( const PercolateMatchResult_t & tRes, const CSphFixedVector<int64_t> & dDocids,	const CSphString & sIndex, JsonEscapedBuilder & tOut )
{
	ScopedComma_c sRootBlock ( tOut, ",", "{", "}" );

	// column names
	tOut.Sprintf ( R"("took":%d,"timed_out":false)", ( int ) ( tRes.m_tmTotal / 1000 ));

	// hits {
	ScopedComma_c sHitsBlock ( tOut, ",", R"("hits":{)", "}");
	tOut.Sprintf ( R"("total":%d,"max_score":1)", tRes.m_dQueryDesc.GetLength()); // FIXME!!! track and provide weight
	if ( tRes.m_bVerbose )
		tOut.Sprintf ( R"("early_out_queries":%d,"matched_queries":%d,"matched_docs":%d,"only_terms_queries":%d,"total_queries":%d)",
			tRes.m_iEarlyOutQueries, tRes.m_iQueriesMatched, tRes.m_iDocsMatched, tRes.m_iOnlyTerms, tRes.m_iTotalQueries );

	// documents
	tOut.StartBlock ( ",", R"("hits":[)", "]");

	int iDocOff = 0;
	for ( const auto& tDesc : tRes.m_dQueryDesc )
	{
		ScopedComma_c sQueryComma ( tOut, ",","{"," }");
		tOut.Sprintf ( R"("table":"%s","_type":"doc","_id":"%U","_score":"1")", sIndex.cstr(), tDesc.m_iQUID );
		{
			ScopedComma_c sBrackets ( tOut, ",", R"("_source":{)", "}");
			if ( !tDesc.m_bQL )
			{
				tOut.Sprintf ( R"("query":%s)", tDesc.m_sQuery.cstr() );
			} else
			{
				ScopedComma_c sBrackets ( tOut, nullptr, R"("query": {"ql":)", "}");
				tOut.AppendEscapedWithComma ( tDesc.m_sQuery.cstr() );
			}
			if ( !tDesc.m_sTags.IsEmpty() )
				tOut.Sprintf ( R"("tags":"%s")", tDesc.m_sTags.cstr() );
		}

		// document count + document id(s)
		if ( tRes.m_bGetDocs )
		{
			ScopedComma_c sFields ( tOut, ",",R"("fields":{"_percolator_document_slot": [)", "] }");
			int iDocs = tRes.m_dDocs[iDocOff];
			for ( int iDoc = 1; iDoc<=iDocs; ++iDoc )
			{
				auto iRow = tRes.m_dDocs[iDocOff + iDoc];
				tOut.Sprintf ("%l", DocID_t ( dDocids.IsEmpty () ? iRow : dDocids[iRow] ) );
			}
			iDocOff += iDocs + 1;
		}
	}

	tOut.FinishBlock ( false ); // hits[]
	// all the rest blocks (root, hits) will be auto-closed here.
}


bool HttpHandlerPQ_c::DoCallPQ ( const CSphString & sIndex, const JsonObj_c & tPercolate, bool bVerbose )
{
	CSphString sWarning, sTmp;
	BlobVec_t dDocs;

	// single document
	JsonObj_c tJsonDoc = tPercolate.GetObjItem ( "document", sTmp );
	if ( tJsonDoc )
	{
		auto & tDoc = dDocs.Add();
		if ( !bson::JsonObjToBson ( tJsonDoc, tDoc, g_bJsonAutoconvNumbers, g_bJsonKeynamesToLowercase ) )
		{
			ReportError ( "Bad cjson", EHTTP_STATUS::_400 );
			return false;
		}
	}

	// multiple documents
	JsonObj_c tJsonDocs = tPercolate.GetArrayItem ( "documents", m_sError, true );
	if ( !m_sError.IsEmpty() )
	{
		ReportError ( EHTTP_STATUS::_400 );
		return false;
	}

	for ( auto i : tJsonDocs )
	{
		auto & tDoc = dDocs.Add();
		if ( !bson::JsonObjToBson ( i, tDoc, g_bJsonAutoconvNumbers, g_bJsonKeynamesToLowercase ) )
		{
			ReportError ( "Bad cjson", EHTTP_STATUS::_400 );
			return false;
		}
	}

	if ( dDocs.IsEmpty() )
	{
		ReportError ( "no documents found", EHTTP_STATUS::_400 );
		return false;
	}

	PercolateOptions_t tOpts;
	tOpts.m_sIndex = sIndex;
	tOpts.m_bGetDocs = true;
	tOpts.m_bVerbose = bVerbose;
	tOpts.m_bGetQuery = true;
	// fixme! id alias here is 'id' or 'uid'. Process it!

	CSphSessionAccum tAcc;
	CPqResult tResult;
	tResult.m_dResult.m_bGetFilters = false;

	PercolateMatchDocuments ( dDocs, tOpts, tAcc, tResult );

	JsonEscapedBuilder sRes;
	EncodePercolateMatchResult ( tResult.m_dResult, tResult.m_dDocids, sIndex, sRes );
	BuildReply ( sRes, EHTTP_STATUS::_200 );

	return true;
}


static void EncodePercolateQueryResult ( bool bReplace, const CSphString & sIndex, int64_t iID, StringBuilder_c & tOut )
{
	if ( bReplace )
		tOut.Sprintf (R"({"table":"%s","type":"doc","_id":"%U","result":"updated","forced_refresh":true})", sIndex.cstr(), iID);
	else
		tOut.Sprintf ( R"({"table":"%s","type":"doc","_id":"%U","result":"created"})", sIndex.cstr (), iID );
}


bool HttpHandlerPQ_c::InsertOrReplaceQuery ( const CSphString & sIndex, const JsonObj_c & tJsonQuery, const JsonObj_c & tRoot, CSphString * pUID, bool bReplace )
{
	CSphString sTmp, sWarning;

	bool bQueryQL = true;
	CSphQuery tQuery;
	const char * sQuery = nullptr;
	JsonObj_c tQueryQL = tJsonQuery.GetStrItem ( "ql", sTmp );
	if ( tQueryQL )
		sQuery = tQueryQL.SzVal();
	else
	{
		bQueryQL = false;
		if ( !ParseJsonQueryFilters ( tJsonQuery, tQuery, m_sError, sWarning ) )
		{
			ReportError ( EHTTP_STATUS::_400 );
			return false;
		}

		if ( NonEmptyQuery ( tJsonQuery ) )
			sQuery = tQuery.m_sQuery.cstr();
	}

	if ( !sQuery || *sQuery=='\0' )
	{
		ReportError ( "no query found", EHTTP_STATUS::_400 );
		return false;
	}

	int64_t iID = 0;
	if ( pUID && !pUID->IsEmpty() )
		iID = strtoll ( pUID->cstr(), nullptr, 10 );

	JsonObj_c tTagsArray = tRoot.GetArrayItem ( "tags", m_sError, true );
	if ( !m_sError.IsEmpty() )
	{
		ReportError ( EHTTP_STATUS::_400 );
		return false;
	}

	StringBuilder_c sTags (", ");
	for ( const auto & i : tTagsArray )
		sTags << i.SzVal();

	JsonObj_c tFilters = tRoot.GetStrItem ( "filters", m_sError, true );
	if ( !m_sError.IsEmpty() )
	{
		ReportError ( EHTTP_STATUS::_400 );
		return false;
	}

	if ( tFilters && !bQueryQL && tQuery.m_dFilters.GetLength() )
	{
		ReportError ( "invalid combination of SphinxQL and query filter provided", EHTTP_STATUS::_501 );
		return false;
	}

	CSphVector<CSphFilterSettings> dFilters;
	CSphVector<FilterTreeItem_t> dFilterTree;
	if ( tFilters )
	{
		auto pServed = GetServed ( sIndex );
		if ( !CheckValid ( pServed, sIndex, IndexType_e::PERCOLATE ) )
			return false;

		RIdx_T<const PercolateIndex_i*> pIndex { pServed };

		if ( !PercolateParseFilters ( tFilters.SzVal(), SPH_COLLATION_UTF8_GENERAL_CI, pIndex->GetInternalSchema (), dFilters, dFilterTree, m_sError ) )
		{
			ReportError ( EHTTP_STATUS::_400 );
			return false;
		}
	} else
	{
		dFilters.SwapData ( tQuery.m_dFilters );
		dFilterTree.SwapData ( tQuery.m_dFilterTree );
	}

	// scope for index lock
	bool bOk = false;
	{
		auto pServed = GetServed ( sIndex );
		if ( !CheckValid ( pServed, sIndex, IndexType_e::PERCOLATE ))
			return false;

		RIdx_T<PercolateIndex_i*> pIndex { pServed };

		PercolateQueryArgs_t tArgs ( dFilters, dFilterTree );
		tArgs.m_sQuery = sQuery;
		tArgs.m_sTags = sTags.cstr();
		tArgs.m_iQUID = iID;
		tArgs.m_bReplace = bReplace;
		tArgs.m_bQL = bQueryQL;

		// add query
		auto pStored = pIndex->CreateQuery ( tArgs, m_sError );
		if ( pStored )
		{
			auto* pSession = session::GetClientSession();
			auto& tAcc = pSession->m_tAcc;
			auto* pAccum = tAcc.GetAcc( pIndex, m_sError );

			ReplicationCommand_t * pCmd = pAccum->AddCommand ( ReplCmd_e::PQUERY_ADD, sIndex );
			// refresh query's UID for reply as it might be auto-generated
			iID = pStored->m_iQUID;
			pCmd->m_pStored = std::move ( pStored );

			bOk = HandleCmdReplicate ( *pAccum );
			TlsMsg::MoveError ( m_sError );
		}
	}

	if ( !bOk )
	{
		ReportError ( EHTTP_STATUS::_500 );
	} else
	{
		StringBuilder_c sRes;
		EncodePercolateQueryResult ( bReplace, sIndex, iID, sRes );
		BuildReply ( sRes, EHTTP_STATUS::_200 );
	}

	return bOk;
}

// for now - forcibly route query as /json/search POST {"table":"<idx>"}. Later matter of deprecate/delete
bool HttpHandlerPQ_c::ListQueries ( const CSphString & sIndex )
{
	StringBuilder_c sQuery;
	sQuery.Sprintf(R"({"table":"%s"})", sIndex.scstr());
	auto pHandler = std::make_unique<HttpHandler_JsonSearch_c> ( (Str_t)sQuery, m_tOptions ) ;
	if ( !pHandler )
		return false;

	pHandler->SetErrorFormat (m_bNeedHttpResponse);
	pHandler->Process ();
	m_dData = std::move ( pHandler->GetResult ());
	return true;
}

bool HttpHandlerPQ_c::Delete ( const CSphString & sIndex, const JsonObj_c & tRoot )
{
	auto* pSession = session::GetClientSession();
	auto& tAcc = pSession->m_tAcc;
	auto* pAccum = tAcc.GetAcc ();

	ReplicationCommand_t * pCmd = pAccum->AddCommand ( ReplCmd_e::PQUERY_DELETE, sIndex );

	JsonObj_c tTagsArray = tRoot.GetArrayItem ( "tags", m_sError, true );
	if ( !m_sError.IsEmpty() )
	{
		ReportError ( EHTTP_STATUS::_400 );
		return false;
	}

	StringBuilder_c sTags ( ", " );
	for ( const auto & i : tTagsArray )
		sTags << i.SzVal();

	JsonObj_c tUidsArray = tRoot.GetArrayItem ( "id", m_sError, true );
	if ( !m_sError.IsEmpty() )
	{
		ReportError ( EHTTP_STATUS::_400 );
		return false;
	}

	for ( const auto & i : tUidsArray )
		pCmd->m_dDeleteQueries.Add ( i.IntVal() );

	if ( !sTags.GetLength() && !pCmd->m_dDeleteQueries.GetLength() )
	{
		ReportError ( "no tags or id field arrays found", EHTTP_STATUS::_400 );
		return false;
	}

	pCmd->m_sDeleteTags = sTags.cstr();
	uint64_t tmStart = sphMicroTimer();

	int iDeleted = 0;
	bool bOk = HandleCmdReplicateDelete ( *pAccum, iDeleted );
	TlsMsg::MoveError ( m_sError );

	uint64_t tmTotal = sphMicroTimer() - tmStart;

	if ( !bOk )
	{
		FormatError ( EHTTP_STATUS::_400, "%s", m_sError.cstr() );
		return false;
	}

	StringBuilder_c tOut;
	tOut.Sprintf (R"({"took":%d,"timed_out":false,"deleted":%d,"total":%d,"failures":[]})",
		( int ) ( tmTotal / 1000 ), iDeleted, iDeleted );
	BuildReply ( tOut, EHTTP_STATUS::_200 );

	return true;
}

bool HttpHandlerPQ_c::Process()
{
	TRACE_CONN ( "conn", "HttpHandlerPQ_c::Process" );
	CSphString * sEndpoint = m_tOptions ( "endpoint" );
	if ( !sEndpoint || sEndpoint->IsEmpty() )
	{
		FormatError ( EHTTP_STATUS::_400, "invalid empty endpoint, should be pq/index_name/operation");
		return false;
	}

	assert ( sEndpoint->Begins ( "json/pq/" ) || sEndpoint->Begins ( "pq/" ) );
	const char * sEndpointMethod = sEndpoint->cstr() + sizeof("pq/") - 1;
	if ( sEndpoint->Begins ( "json/pq/" ) )
		sEndpointMethod = sEndpoint->cstr() + sizeof("json/pq/") - 1;

	StrVec_t dPoints;
	sphSplit ( dPoints, sEndpointMethod, "/" );
	if ( dPoints.GetLength()<2 )
	{
		FormatError ( EHTTP_STATUS::_400, "invalid endpoint '%s', should be pq/index_name/operation", sEndpoint->scstr() );
		return false;
	}

	const CSphString & sIndex = dPoints[0];
	const CSphString & sOp = dPoints[1];
	CSphString * pUID = nullptr;
	if ( dPoints.GetLength()>2 )
		pUID = dPoints.Begin() + 2;

	enum class PercolateOp_e
	{
		UNKNOWN,
		ADD,
		DEL,
		SEARCH
	} eOp = PercolateOp_e::UNKNOWN;

	if ( sOp=="_delete_by_query" )
		eOp = PercolateOp_e::DEL;
	else if ( sOp=="doc" )
		eOp = PercolateOp_e::ADD;
	else if ( sOp=="search" )
		eOp = PercolateOp_e::SEARCH;

	if ( IsEmpty ( m_sQuery ) )
		return ListQueries ( sIndex );

	const JsonObj_c tRoot ( m_sQuery );
	if ( !tRoot )
	{
		ReportError ( "bad JSON object", EHTTP_STATUS::_400 );
		return false;
	}

	if ( !tRoot.Size() )
		return ListQueries ( sIndex );

	if ( eOp==PercolateOp_e::UNKNOWN )
	{
		m_sError.SetSprintf ( "invalid percolate operation '%s', should be one of 'search' or 'doc' or '_delete_by_query'", sOp.cstr() );
		ReportError ( EHTTP_STATUS::_400 );
		return false;
	}


	JsonObj_c tQuery = tRoot.GetObjItem ( "query", m_sError, ( eOp==PercolateOp_e::DEL ) );
	if ( !tQuery && ( eOp!=PercolateOp_e::DEL ) )
	{
		ReportError ( EHTTP_STATUS::_400 );
		return false;
	}

	JsonObj_c tPerc = ( ( eOp==PercolateOp_e::SEARCH ) ? tQuery.GetObjItem ( "percolate", m_sError ) : JsonNull );
	if ( ( eOp==PercolateOp_e::SEARCH ) && !tPerc )
	{
		ReportError ( EHTTP_STATUS::_400 );
		return false;
	}

	bool bVerbose = false;
	JsonObj_c tVerbose = tRoot.GetItem ( "verbose" );
	if ( tVerbose )
	{
		if ( tVerbose.IsDbl() )
			bVerbose = tVerbose.DblVal()!=0.0;
		else if ( tVerbose.IsInt() )
			bVerbose = tVerbose.IntVal()!=0;
		else if ( tVerbose.IsBool() )
			bVerbose = tVerbose.BoolVal();
	}

	if ( eOp==PercolateOp_e::SEARCH )
		return DoCallPQ ( sIndex, tPerc, bVerbose );
	else if ( eOp==PercolateOp_e::DEL )
		return Delete ( sIndex, tRoot );
	else
	{
		bool bRefresh = false;
		CSphString * pRefresh = m_tOptions ( "refresh" );
		if ( pRefresh && !pRefresh->IsEmpty() )
		{
			if ( *pRefresh=="0" )
				bRefresh = false;
			else if ( *pRefresh=="1" )
				bRefresh = true;
		}

		return InsertOrReplaceQuery ( sIndex, tQuery, tRoot, pUID, bRefresh );
	}
}

static bool ParseMetaLine ( const char * sLine, BulkDoc_t & tDoc, CSphString & sError )
{
	JsonObj_c tLineMeta ( sLine );
	JsonObj_c tAction = tLineMeta[0];
	if ( !tAction )
	{
		sError = "no statement found";
		return false;
	}

	tDoc.m_sAction = tAction.Name();

	if ( !tAction.IsObj() )
	{
		sError.SetSprintf ( "statement %s should be an object", tDoc.m_sAction.cstr() );
		return false;
	}

	JsonObj_c tIndex = tAction.GetStrItem ( "_index", sError );
	if ( !tIndex )
		return false;

	tDoc.m_sIndex = tIndex.StrVal();
	
	JsonObj_c tId = tAction.GetItem ( "_id" );
	if ( tId )
	{
		if ( tId.IsNum() )
			tDoc.m_tDocid = tId.IntVal();
		else if ( tId.IsStr() )
			tDoc.m_tDocid = GetDocID ( tId.SzVal() );
		else if ( tId.IsNull() )
			tDoc.m_tDocid = 0;
		else
		{
			sError.SetSprintf ( "_id should be an int or string" );
			return false;
		}
	}

	return true;
}

static bool AddDocid ( SqlStmt_t & tStmt, DocID_t & tDocId, CSphString & sError )
{
	int iDocidPos = tStmt.m_dInsertSchema.GetFirst ( [&] ( const CSphString & sName ) { return sName=="id"; } );
	if ( iDocidPos!=-1 )
	{
		SqlInsert_t & tVal = tStmt.m_dInsertValues[iDocidPos];

		// check and convert to int
		if ( tVal.m_iType!=SqlInsert_t::CONST_INT )
		{
			tVal.SetValueInt ( GetDocID ( tVal.m_sVal.cstr() ), false );
			tVal.m_iType = SqlInsert_t::CONST_INT;
		}

		DocID_t tSrcDocid = (int64_t)tVal.GetValueUint();
		// can not set id at the same time via es meta and via document id property
		if ( tDocId && tDocId!=tSrcDocid )
		{
			sError = "id has already been specified";
			return false;
		}
		
		tDocId = tSrcDocid;
		return true;
	}

	if ( !tDocId )
		return true;

	tStmt.m_dInsertSchema.Add ( sphGetDocidName() );
	SqlInsert_t & tId = tStmt.m_dInsertValues.Add();
	tId.m_iType = SqlInsert_t::CONST_INT;
	tId.SetValueInt(tDocId);

	tStmt.m_iSchemaSz = tStmt.m_dInsertSchema.GetLength();
	return true;
}

static bool ParseSourceLine ( const char * sLine, const CSphString & sAction, SqlStmt_t & tStmt, DocID_t & tDocId, CSphString & sError )
{
	// FIXME!!! update and delete ES compat endpoints
	if ( sAction=="index" )
	{
		JsonObj_c tRoot ( sLine );
		if ( !ParseJsonInsertSource ( tRoot, tStmt, true, sError ) )
			return false;
		if ( !AddDocid ( tStmt, tDocId, sError ) )
			return false;

	}  else if ( sAction=="create" )
	{
		JsonObj_c tRoot ( sLine );
		if ( !ParseJsonInsertSource ( tRoot, tStmt, false, sError ) )
			return false;
		if ( !AddDocid ( tStmt, tDocId, sError ) )
			return false;

	} else if ( sAction=="update" )
	{
		JsonObj_c tUpd ( FromSz ( sLine ) );
		tUpd.AddStr ( "table", tStmt.m_sIndex );
		tUpd.AddInt ( "id", tDocId );
		if ( !ParseJsonUpdate ( tUpd, tStmt, tDocId, sError ) )
			return false;

	} else if ( sAction=="delete" )
	{
		tStmt.m_eStmt = STMT_DELETE;
		tStmt.m_tQuery.m_sSelect = "id";
		CSphFilterSettings & tFilter = tStmt.m_tQuery.m_dFilters.Add();
		tFilter.m_eType = SPH_FILTER_VALUES;
		tFilter.m_dValues.Add ( tDocId );
		tFilter.m_sAttrName = "id";
	} else
	{
		sError.SetSprintf ( "unknown action: %s", sAction.cstr() );
		return false;
	}

	// _bulk could have cluster:index format
	SqlParser_SplitClusterIndex ( tStmt.m_sIndex, &tStmt.m_sCluster );

	return true;
}

char * SkipSpace ( char * p )
{
	while ( sphIsSpace ( *p ) )
		p++;

	return p;
}

static CSphString sphHttpEndpointToStr ( EHTTP_ENDPOINT eEndpoint )
{
	assert ( eEndpoint < EHTTP_ENDPOINT::TOTAL );
	return g_dEndpoints[(int)eEndpoint].m_szName1;
}

bool Ends ( const Str_t tVal, const char * sSuffix )
{
	if ( IsEmpty ( tVal ) || !sSuffix )
		return false;

	auto iSuffix = (int) strlen ( sSuffix );
	if ( tVal.second<iSuffix )
		return false;

	return strncmp ( tVal.first + tVal.second - iSuffix, sSuffix, iSuffix )==0;
}

void HttpHandlerEsBulk_c::ReportLogError ( const char * sError, HttpErrorType_e eType, EHTTP_STATUS eStatus, bool bLogOnly )
{
	if ( !bLogOnly )
		ReportError ( sError, eType, eStatus );

	for ( char * sCur = (char *)GetBody().first; sCur<GetBody().first+GetBody().second; sCur++ )
	{
		if ( *sCur=='\0' )
			*sCur = '\n';
	}

	const CSphString * pUrl = GetOptions() ( "full_url" );
	HTTPINFO << sError << "\n" << ( pUrl ? pUrl->scstr() : "" ) << "\n" << GetBody().first;
}

bool HttpHandlerEsBulk_c::Validate()
{
	CSphString sError;

	CSphString * pOptContentType = GetOptions() ( "content-type" );
	if ( !pOptContentType )
	{
		ReportLogError ( "Content-Type must be set", HttpErrorType_e::IllegalArgument, EHTTP_STATUS::_400, false );
		return false;
	}

	// HTTP field could have multiple values
	StrVec_t dOptContentType = sphSplit ( pOptContentType->cstr(), ",; " );
	if ( !dOptContentType.Contains ( "application/x-ndjson" ) && !dOptContentType.Contains ( "application/json" ) )
	{
		sError.SetSprintf ( "Content-Type header [%s] is not supported", pOptContentType->cstr() );
		ReportLogError ( sError.cstr(), HttpErrorType_e::IllegalArgument, EHTTP_STATUS::_400, false );
		return false;
	}

	if ( IsEmpty ( GetBody() ) )
	{
		ReportLogError ( "request body is required", HttpErrorType_e::Parse, EHTTP_STATUS::_400, false );
		return false;
	}
	if ( !Ends ( GetBody(), "\n" ) )
	{
		ReportLogError ( "The bulk request must be terminated by a newline [\n]", HttpErrorType_e::IllegalArgument, EHTTP_STATUS::_400, false );
		return false;
	}

	return true;
}

bool HttpHandlerEsBulk_c::Process()
{
	if ( !Validate() )
		return false;

	auto & tCrashQuery = GlobalCrashQueryGetRef();
	tCrashQuery.m_dQuery = S2B ( GetBody() );

	CSphVector<Str_t> dLines;
	SplitNdJson ( GetBody(), [&] ( const char * sLine, int iLen ) { dLines.Add ( Str_t ( sLine, iLen ) ); } );
	
	CSphString sError;
	CSphVector<BulkDoc_t> dDocs;
	dDocs.Reserve ( dLines.GetLength() / 2 );
	bool bNextLineMeta = true;
	for ( const Str_t & tLine : dLines )
	{
		if ( !bNextLineMeta )
		{
			dDocs.Last().m_tDocLine = tLine;
			bNextLineMeta = true;
		} else
		{
			// skip empty lines if they are meta information
			if ( IsEmpty ( tLine ) )
				continue;

			// any bad meta result in general error
			BulkDoc_t & tDoc = dDocs.Add();
			if ( !ParseMetaLine ( tLine.first, tDoc, sError ) )
			{
				ReportLogError ( sError.cstr(), HttpErrorType_e::ActionRequestValidation, EHTTP_STATUS::_400, false );
				return false;
			}
			if ( tDoc.m_sAction=="delete" )
			{
				tDoc.m_tDocLine = tLine;
				bNextLineMeta = true;
			} else
			{
				bNextLineMeta = false;
			}
		}
	}
	CSphVector<BulkTnx_t> dTnx;
	const BulkDoc_t * pLastDoc = dDocs.Begin();
	for ( const BulkDoc_t * pCurDoc = pLastDoc + 1; pCurDoc<dDocs.End(); pCurDoc++ )
	{
		// chain the same statements to the same index but not the updates
		if ( pLastDoc->m_sIndex==pCurDoc->m_sIndex && pLastDoc->m_sAction==pCurDoc->m_sAction && pCurDoc->m_sAction!="update" )
			continue;

		BulkTnx_t & tTnx = dTnx.Add();
		tTnx.m_iFrom = pLastDoc - dDocs.Begin();
		tTnx.m_iCount = pCurDoc - pLastDoc;
		pLastDoc = pCurDoc;
	}
	if ( pLastDoc )
	{
		BulkTnx_t & tTnx = dTnx.Add();
		tTnx.m_iFrom = pLastDoc - dDocs.Begin();
		tTnx.m_iCount = dDocs.GetLength() - tTnx.m_iFrom;
	}

	JsonObj_c tItems ( true );
	bool bOk = ProcessTnx ( dTnx, dDocs, tItems );

	JsonObj_c tRoot;
	tRoot.AddItem ( "items", tItems );
	tRoot.AddBool ( "errors", !bOk );
	tRoot.AddInt ( "took", 1 ); // FIXME!!! add delta
	BuildReply ( tRoot.AsString(), ( bOk ? EHTTP_STATUS::_200 : EHTTP_STATUS::_409 ) );

	if ( !bOk )
		ReportLogError ( "failed to commit", HttpErrorType_e::Unknown, EHTTP_STATUS::_400, true );

	return bOk;
}

static void AddEsReply ( const BulkDoc_t & tDoc, JsonObj_c & tRoot )
{
	const JsonObj_c tRefShards ( "{ \"total\": 1, \"successful\": 1, \"failed\": 0 }" );

	char sBuf[70];
	snprintf ( sBuf, sizeof(sBuf), UINT64_FMT, (uint64_t)tDoc.m_tDocid );
	const char * sActionRes = "created";
	if ( tDoc.m_sAction=="delete" )
		sActionRes = "deleted";
	else if ( tDoc.m_sAction=="update" )
		sActionRes = "updated";
	JsonObj_c tShard ( tRefShards.Clone() );

	JsonObj_c tRes;
	tRes.AddStr ( "_index", tDoc.m_sIndex.cstr() );
	tRes.AddStr ( "_type", "doc" );
	tRes.AddStr ( "_id", sBuf );
	tRes.AddInt ( "_version", 1 );
	tRes.AddStr ( "result", sActionRes );
	tRes.AddItem ( "_shards", tShard );
	tRes.AddInt ( "_seq_no", 0 );
	tRes.AddInt ( "_primary_term", 1 );
	tRes.AddInt ( "status", 201 );

	JsonObj_c tAction;
	tAction.AddItem ( tDoc.m_sAction.cstr(), tRes );
	tRoot.AddItem ( tAction );
}

static void AddEsError ( int iReply, const CSphString & sError, const char * sErrorType, const BulkDoc_t & tDoc, JsonObj_c & tRoot )
{
	char sBuf[70];
	snprintf ( sBuf, sizeof(sBuf), UINT64_FMT, (uint64_t)tDoc.m_tDocid );

	JsonObj_c tErrorObj;
	tErrorObj.AddStr ( "type", sErrorType );
	tErrorObj.AddStr ( "reason", sError.cstr() );

	JsonObj_c tRes;
	tRes.AddStr ( "_index", tDoc.m_sIndex.cstr() );
	tRes.AddStr ( "_type", "doc" );
	tRes.AddStr ( "_id", sBuf );
	tRes.AddInt ( "status", 400 );
	tRes.AddItem ( "error", tErrorObj );

	JsonObj_c tAction;
	tAction.AddItem ( tDoc.m_sAction.cstr(), tRes );

	if ( iReply!=-1 )
		tRoot.ReplaceItem ( iReply, tAction );
	else
		tRoot.AddItem ( tAction );
}

bool HttpHandlerEsBulk_c::ProcessTnx ( const VecTraits_T<BulkTnx_t> & dTnx, VecTraits_T<BulkDoc_t> & dDocs, JsonObj_c & tItems )
{
	bool bOk = true;

	CSphVector<std::pair<int, CSphString>> dErrors;

	for ( const BulkTnx_t & tTnx : dTnx )
	{
		const CSphString & sIdx = dDocs[tTnx.m_iFrom].m_sIndex;
		assert ( !sIdx.IsEmpty() );
		ProcessBegin ( sIdx );

		bool bUpdate = false;
		bOk &= dErrors.IsEmpty();
		dErrors.Resize ( 0 );
		for ( int i = 0; i<tTnx.m_iCount; i++ )
		{
			int iDoc = tTnx.m_iFrom + i;
			BulkDoc_t & tDoc = dDocs[iDoc];

			if ( IsEmpty ( tDoc.m_tDocLine ) )
			{
				dErrors.Add ( { iDoc, "failed to parse, document is empty" } );
				continue;
			}

			SqlStmt_t tStmt;
			tStmt.m_tQuery.m_sIndexes = tDoc.m_sIndex;
			tStmt.m_sIndex = tDoc.m_sIndex;
			tStmt.m_sStmt = tDoc.m_tDocLine.first;

			bool bParsed = ParseSourceLine ( tDoc.m_tDocLine.first, tDoc.m_sAction, tStmt, tDoc.m_tDocid, m_sError );
			if ( !bParsed )
			{
				dErrors.Add ( { iDoc, m_sError } );
				continue;
			}

			bool bAction = false;
			JsonObj_c tResult = JsonNull;

			SetQueryOptions ( m_hOpts, tStmt );

			switch ( tStmt.m_eStmt )
			{
			case STMT_INSERT:
			case STMT_REPLACE:
				bAction = ProcessInsert ( tStmt, tDoc.m_tDocid, tResult, m_sError, ResultSetFormat_e::ES );
				break;

			case STMT_UPDATE:
				tStmt.m_sEndpoint = sphHttpEndpointToStr ( EHTTP_ENDPOINT::JSON_UPDATE );
				bAction = ProcessUpdate ( tDoc.m_tDocLine, tStmt, tDoc.m_tDocid, tResult, m_sError );
				bUpdate = true;
				break;

			case STMT_DELETE:
				tStmt.m_sEndpoint = sphHttpEndpointToStr ( EHTTP_ENDPOINT::JSON_DELETE );
				bAction = ProcessDelete ( tDoc.m_tDocLine, tStmt, tDoc.m_tDocid, tResult, m_sError, ResultSetFormat_e::ES );
				break;

			default:
				sphWarning ( "unknown statement \"%s\":%s", tStmt.m_sStmt, tDoc.m_tDocLine.first );
				break; // ignore statement as ES does
			}

			if ( !bAction )
			{
				JsonObj_c tError = JsonNull;
				JsonObj_c tErrorType = JsonNull;
				if ( !tResult.Empty() && tResult.HasItem( "error" ) )
					tError = tResult.GetItem ( "error" );
				if ( !tError.Empty() && tError.HasItem ( "type" ))
					tErrorType = tError.GetItem ( "type" );

				if ( !tErrorType.Empty() )
					dErrors.Add ( { iDoc, tErrorType.StrVal() } );
				else
				{
					dErrors.Add ( { iDoc, CSphString() } );
					dErrors.Last().second.SetSprintf ( "unknown statement \"%s\":%s", tStmt.m_sStmt, tDoc.m_tDocLine.first );
				}
			}
		}

		// FIXME!!! check commit of empty accum
		JsonObj_c tResult;
		DocID_t tDocId = 0;
		bool bCommited = ProcessCommitRollback ( FromStr ( sIdx ), tDocId, tResult, m_sError );
		if ( bCommited )
		{
			if ( bUpdate && !GetLastUpdated() )
			{
				assert ( tTnx.m_iCount==1 );
				const BulkDoc_t & tUpdDoc = dDocs[tTnx.m_iFrom];
				CSphString sUpdError;
				sUpdError.SetSprintf ( "[_doc][" INT64_FMT "]: document missing", tUpdDoc.m_tDocid );
				AddEsError ( -1, sUpdError, "document_missing_exception", tUpdDoc, tItems );
			} else
			{
				for ( int i=0; i<tTnx.m_iCount; i++ )
					AddEsReply ( dDocs[tTnx.m_iFrom+i], tItems );
			}
		} else
		{
			for ( int i=0; i<tTnx.m_iCount; i++ )
			{
				AddEsError ( -1, tResult.GetStrItem ( "error", m_sError, false ).StrVal(), "mapper_parsing_exception", dDocs[tTnx.m_iFrom+i], tItems );
			}
		}

		for ( const auto & tErr : dErrors )
			AddEsError ( tErr.first, tErr.second, "mapper_parsing_exception", dDocs[tErr.first], tItems );
	}

	bOk &= dErrors.IsEmpty();
	session::SetInTrans ( false );
	return bOk;
}

void SplitNdJson ( Str_t sBody, SplitAction_fn && fnAction )
{
	const char * sBodyEnd = sBody.first + sBody.second;
	while ( sBody.first<sBodyEnd )
	{
		const char * sNext = sBody.first;
		// break on CR or LF
		while ( sNext<sBodyEnd && *sNext != '\r' && *sNext != '\n' )
			sNext++;

		if ( sNext==sBodyEnd )
			break;

		*(const_cast<char*>(sNext)) = '\0';
		fnAction ( sBody.first, sNext-sBody.first );
		sBody.first = sNext + 1;

		// skip new lines
		while ( sBody.first<sBodyEnd && *sBody.first == '\n' )
			sBody.first++;
	}
}

bool HttpSetLogVerbosity ( const CSphString & sVal )
{
	if ( !sVal.Begins( "http_" ) )
		return false;

	bool bOn = ( sVal.Ends ( "_1" ) || sVal.Ends ( "_on" ) );
	
	if ( sVal.Begins ( "http_bad_req" ) )
		g_bLogBadHttpReq = bOn;
	else
		LOG_LEVEL_HTTP = bOn;

	return true;
}

void LogReplyStatus100()
{
	HTTPINFO << "100 Continue sent";
}

const char * GetErrorTypeName ( HttpErrorType_e eType )
{
	switch ( eType )
	{
	case HttpErrorType_e::Parse: return "parse_exception";
	case HttpErrorType_e::IllegalArgument: return "illegal_argument_exception";
	case HttpErrorType_e::ActionRequestValidation: return "action_request_validation_exception";
	case HttpErrorType_e::IndexNotFound: return "index_not_found_exception";
	case HttpErrorType_e::ContentParse: return "x_content_parse_exception";
	case HttpErrorType_e::VersionConflictEngine: return "version_conflict_engine_exception";
	case HttpErrorType_e::DocumentMissing: return "document_missing_exception";
	case HttpErrorType_e::ResourceAlreadyExists: return "resource_already_exists_exception";
	case HttpErrorType_e::AliasesNotFound: return "aliases_not_found_exception";
	default:
		return nullptr;;
	}
}
