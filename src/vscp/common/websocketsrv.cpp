// vscpwebsocketserver.cpp
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version
// 2 of the License, or (at your option) any later version.
//
// This file is part of the VSCP (http://www.vscp.org)
//
// Copyright (C) 2000-2017
// Ake Hedman, Grodans Paradis AB, <akhe@grodansparadis.com>
//
// This file is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this file see the file COPYING.  If not, write to
// the Free Software Foundation, 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.
//

#ifdef __GNUG__
//#pragma implementation
#endif


#ifdef WIN32
#include <winsock2.h>
#endif

// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include "wx/wx.h"
#include "wx/defs.h"
#include "wx/app.h"
#include <wx/xml/xml.h>
#include <wx/txtstrm.h>
#include <wx/platinfo.h>
#include <wx/filename.h>

#include <iostream>
#include <sstream>
#include <fstream>

#include <mongoose.h>

#ifdef WIN32

#include <winsock2.h>
//#include <winsock.h>
#include "canal_win32_ipc.h"

#else   // UNIX

#define _POSIX
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <syslog.h>
#include <sys/msg.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <string.h>
#include <netdb.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if_arp.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "wx/wx.h"
#include "wx/defs.h"
#include "wx/log.h"
#include "wx/socket.h"

#endif

#include <wx/config.h>
#include <wx/wfstream.h>
#include <wx/fileconf.h>
#include <wx/tokenzr.h>
#include <wx/listimpl.cpp>
#include <wx/xml/xml.h>
#include <wx/mimetype.h>
#include <wx/filename.h>

#include "web_css.h"
#include "web_js.h"
#include "web_template.h"

#include <mongoose.h>
#include <vscpweb.h>

#include <canal_macro.h>
#include <vscp.h>
#include <vscphelper.h>
#include <vscpeventhelper.h>
#include <tables.h>
#include <configfile.h>
#include <crc.h>
#include <randpassword.h>
#include <version.h>
#include <variablecodes.h>
#include <actioncodes.h>
#include <devicelist.h>
#include <devicethread.h>
#include <dm.h>
#include <mdf.h>
#include <websrv.h>
#include <websocket.h>
#include <controlobject.h>

using namespace std;

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

///////////////////////////////////////////////////
//                 GLOBALS
///////////////////////////////////////////////////

extern CControlObject *gpobj;

// Weberver
extern struct mg_mgr gmgr;

// Linked list of all active sessions. (webserv.h)
extern struct websrv_Session *gp_websrv_sessions;

// Session structure for REST API
extern struct websrv_rest_session *gp_websrv_rest_sessions;

// Prototypes
int webserv_url_decode( const char *src, int src_len,
                            char *dst, int dst_len,
                            int is_form_url_encoded );

void webserv_util_sendheader( struct mg_connection *nc,
                                        const int returncode,
                                        const char *content );

///////////////////////////////////////////////////
//                 WEBSOCKETS
///////////////////////////////////////////////////


#define MAX_WS_CLIENTS (512)

// TODO remove
struct websocket_client {
	struct web_connection *conn;
	int state;
} static websocket_clients[ MAX_WS_CLIENTS ];


////////////////////////////////////////////////////////////////////////////////
// ws1_ConnectHandler
//

int
ws1_ConnectHandler( const struct web_connection *conn, void *cbdata ) 
{
    struct web_context *ctx = web_get_context(conn);
    int reject = 1;
    int i;

    web_lock_context( ctx );
    for (i = 0; i < MAX_WS_CLIENTS; i++) {
        if (websocket_clients[i].conn == NULL) {
            websocket_clients[i].conn = (struct web_connection *)conn;
            websocket_clients[i].state = 1;
            web_set_user_connection_data( websocket_clients[i].conn,
                                            (void *)( websocket_clients + i ) );
            reject = 0;
            break;
        }
    }
    web_unlock_context( ctx );

    fprintf( stdout,
                "Websocket client %s\r\n\r\n",
                ( reject ? "rejected" : "accepted" ) );
    
    return reject;
}

////////////////////////////////////////////////////////////////////////////////
// ws1_CloseHandler
//

void
ws1_CloseHandler(const struct web_connection *conn, void *cbdata) 
{
    struct web_context *ctx = web_get_context( conn );
    struct websocket_client *client = 
        (struct websocket_client *)web_get_user_connection_data( conn );
    //ASSERT(client->conn == conn);
    //ASSERT(client->state >= 1);

    web_lock_context( ctx );
    client->state = 0;
    client->conn = NULL;
    web_unlock_context( ctx );

    fprintf( stdout,
                "Client droped from the set of webserver connections\r\n\r\n");
}

////////////////////////////////////////////////////////////////////////////////
// ws1_ReadyHandler
//

void
ws1_ReadyHandler( struct web_connection *conn, void *cbdata ) 
{
    const char *text = "Hello from the websocket ready handler";
    struct websocket_client *client = 
        (struct websocket_client *)web_get_user_connection_data( conn );

    web_websocket_write( conn, WEB_WEBSOCKET_OPCODE_TEXT, text, strlen( text ) );
    //ASSERT( client->conn == conn );
    //ASSERT( client->state == 1 );

    client->state = 2;
}

////////////////////////////////////////////////////////////////////////////////
// ws1_DataHandler
//

int
ws1_DataHandler( struct web_connection *conn,
                        int bits,
                        char *data,
                        size_t len,
                        void *cbdata ) 
{
    struct websocket_client *client = 
        (struct websocket_client *)web_get_user_connection_data( conn );
    //ASSERT(client->conn == conn);
    //ASSERT(client->state >= 1);

    fprintf(stdout, "Websocket got %lu bytes of ", (unsigned long) len );
    switch ( ( (unsigned char)bits ) & 0x0F ) {
        
        case WEB_WEBSOCKET_OPCODE_CONTINUATION:
            fprintf(stdout, "continuation");
            break;
            
        case WEB_WEBSOCKET_OPCODE_TEXT:
            fprintf(stdout, "text");
            break;
            
        case WEB_WEBSOCKET_OPCODE_BINARY:
            fprintf(stdout, "binary");
            break;
            
        case WEB_WEBSOCKET_OPCODE_CONNECTION_CLOSE:
            fprintf(stdout, "close");
            break;
            
        case WEB_WEBSOCKET_OPCODE_PING:
            fprintf(stdout, "ping");
            break;
            
        case WEB_WEBSOCKET_OPCODE_PONG:
            fprintf(stdout, "pong");
            break;
            
        default:
            fprintf(stdout, "unknown(%1xh)", ((unsigned char) bits) & 0x0F);
            break;
            
    }
    
    fprintf( stdout, " data:\r\n" );
    fwrite( data, len, 1, stdout );
    fprintf( stdout, "\r\n\r\n" );

    return WEB_OK;
}





// Linked list of websocket sessions
// Protected by the websocketSexxionMutex
//static struct websock_session *gp_websock_sessions;


websock_session::websock_session( void )
{
    memset( m_key, 0, 33 );
    memset( m_sid, 0, 33 );
    m_version = 0;
    m_referenceCount = 0;
    bAuthenticated = false;
    lastActiveTime = 0;
    m_pClientItem = NULL;
    bTrigger = false;
    triggerTimeout = 0;
};

websock_session::~websock_session( void )
{
    
};


///////////////////////////////////////////////////////////////////////////////
//                               WEBSOCKET
///////////////////////////////////////////////////////////////////////////////






///////////////////////////////////////////////////////////////////////////////
// websock_command
//

void
VSCPWebServerThread::websock_command( struct mg_connection *nc,
                                        struct http_message *hm,
                                        struct websocket_message *wm,
                                        struct websock_session *pSession,
                                        wxString& strCmd )
{
    wxString strTok;

    // Check pointer
    if (NULL == nc) return;
    if (NULL == hm) return;
    if (NULL == wm) return;
    if (NULL == pSession) return;

    CControlObject *pCtrlObject = (CControlObject *)nc->mgr->user_data;
    if (NULL == pCtrlObject) return;

    pCtrlObject->logMsg ( _("[Websocket] Command = ") + strCmd + _("\n"),
                                        DAEMON_LOGMSG_DEBUG,
                                        DAEMON_LOGTYPE_GENERAL );

    wxStringTokenizer tkz( strCmd, _(";"), wxTOKEN_RET_EMPTY_ALL );

    // Get command
    if (tkz.HasMoreTokens()) {
        strTok = tkz.GetNextToken();
        strTok.MakeUpper();
    }
    else {
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;%d;%s",
                                    WEBSOCK_ERROR_SYNTAX_ERROR,
                                    WEBSOCK_STR_ERROR_SYNTAX_ERROR );
        return;
    }
    
    // ------------------------------------------------------------------------
    //                                NOOP
    //-------------------------------------------------------------------------
    
    if (0 == strTok.Find(_("NOOP"))) {

        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, "+;NOOP" );

        // Send authentication challenge
        if ( !pSession->bAuthenticated ) {

        }
    }
    else if (0 == strTok.Find(_("CHALLENGE"))) {

        // Send authentication challenge
        if ( !pSession->bAuthenticated ) {
            // Start authentication
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "+;AUTH0;%s",
                                    pSession->m_sid );
        }

    }
    
    // ------------------------------------------------------------------------
    //                                AUTH
    //-------------------------------------------------------------------------
    
    // AUTH;user;hash
    else if (0 == strTok.Find(_("AUTH"))) {

	wxString strUser = tkz.GetNextToken();
        wxString strKey = tkz.GetNextToken();
        if ( 1/*pCtrlObject->getWebServer()->websock_authentication( nc,
                                                                    hm,
                                                                    pSession,
                                                                    strUser,
                                                                    strKey )*/ ) {
            // Get user
            CUserItem *pUser = gpobj->m_userList.getUser( strUser );
            if ( NULL == pUser ) goto autherror;
            wxString userSettings;
            pUser->getAsString( userSettings );
	    mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, 
                                                "+;AUTH1;%s",
                                                (const char *)userSettings.mbc_str() );
            pSession->bAuthenticated = true;    // Authenticated

        }
        else {
autherror:            
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;AUTH;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            pSession->bAuthenticated = false;   // Authenticated
        }
    }
    
    // ------------------------------------------------------------------------
    //                                OPEN
    //-------------------------------------------------------------------------
    
    else if (0 == strTok.Find(_("OPEN"))) {

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;OPEN;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            return;     // We still leave channel open
        }

        pSession->m_pClientItem->m_bOpen = true;
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, "+;OPEN" );
    }

    else if (0 == strTok.Find(_("CLOSE"))) {
        pSession->m_pClientItem->m_bOpen = false;
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, "+;CLOSE" );
    }
    
    // ------------------------------------------------------------------------
    //                             SETFILTER/SF
    //-------------------------------------------------------------------------
    
    else if ( ( 0 == strTok.Find( _("SETFILTER") ) ) || 
            ( 0 == strTok.Find( _("SF") ) ) ) {

        unsigned char ifGUID[ 16 ];
        memset(ifGUID, 0, 16);

        // Must be authorized to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;SF;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to set a filter.\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return;     // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 6 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;SF;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to set a filter.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return; // We still leave channel open
        }

        // Get filter
        if ( tkz.HasMoreTokens() ) {

            strTok = tkz.GetNextToken();

            pSession->m_pClientItem->m_mutexClientInputQueue.Lock();
            if (!vscp_readFilterFromString( &pSession->m_pClientItem->m_filterVSCP, strTok ) ) {

                // Unlock
                pSession->m_pClientItem->m_mutexClientInputQueue.Unlock();
                mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;SF;%d;%s",
                                    WEBSOCK_ERROR_SYNTAX_ERROR,
                                    WEBSOCK_STR_ERROR_SYNTAX_ERROR );
                return;
            }

            pSession->m_pClientItem->m_mutexClientInputQueue.Unlock();
        }
        else {
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;SF;%d;%s",
                                    WEBSOCK_ERROR_SYNTAX_ERROR,
                                    WEBSOCK_STR_ERROR_SYNTAX_ERROR );
            return;
        }

        // Get mask
        if ( tkz.HasMoreTokens() ) {

            strTok = tkz.GetNextToken();

            pSession->m_pClientItem->m_mutexClientInputQueue.Lock();
            if (!vscp_readMaskFromString( &pSession->m_pClientItem->m_filterVSCP,
                                            strTok)) {

                // Unlock
                pSession->m_pClientItem->m_mutexClientInputQueue.Unlock();
                mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;SF;%d;%s",
                                    WEBSOCK_ERROR_SYNTAX_ERROR,
                                    WEBSOCK_STR_ERROR_SYNTAX_ERROR );
                pSession->m_pClientItem->m_mutexClientInputQueue.Unlock();
                return;
            }
            pSession->m_pClientItem->m_mutexClientInputQueue.Unlock();
        }
        else {
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;SF;%d;%s",
                                    WEBSOCK_ERROR_SYNTAX_ERROR,
                                    WEBSOCK_STR_ERROR_SYNTAX_ERROR );
            return;
        }

        // Positive response
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, "+;SF" );

    }
    // ------------------------------------------------------------------------
    //                           CLRQ/CLRQUEUE
    //-------------------------------------------------------------------------
    
    // Clear the event queue
    else if ( ( 0 == strTok.Find(_("CLRQUEUE") ) ) || 
                ( 0 == strTok.Find(_("CLRQ") ) ) ) {

        CLIENTEVENTLIST::iterator iterVSCP;

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;CLRQ;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to clear the queue.\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return;     // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 1 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;CLRQ;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to clear the queue.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return; // We still leave channel open
        }

        pSession->m_pClientItem->m_mutexClientInputQueue.Lock();
        for ( iterVSCP = pSession->m_pClientItem->m_clientInputQueue.begin();
                iterVSCP != pSession->m_pClientItem->m_clientInputQueue.end(); ++iterVSCP ) {
            vscpEvent *pEvent = *iterVSCP;
            vscp_deleteVSCPevent(pEvent);
        }

        pSession->m_pClientItem->m_clientInputQueue.Clear();
        pSession->m_pClientItem->m_mutexClientInputQueue.Unlock();

        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, "+;CLRQ" );
    }
    
    
    
    ////////////////////////////////////////////////////////////////////////////
    //                              VARIABLES
    ////////////////////////////////////////////////////////////////////////////
    
    
    // ------------------------------------------------------------------------
    //                            CVAR/CREATEVAR
    //-------------------------------------------------------------------------
    
    else if ( ( 0 == strTok.Find(_("CREATEVAR") ) ) || 
                ( 0 == strTok.Find(_("CVAR") ) ) ) {

        wxString name;
        wxString value;
        wxString note;
        uint8_t type = VSCP_DAEMON_VARIABLE_CODE_STRING;
        bool bPersistent = false;
        uint32_t accessrights = 744;

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;CVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to create a variable.\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return;     // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 6 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;CVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to create a variable.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return ;    // We still leave channel open
        }
        
        // “C;CVAR;name;type;accessrights;bPersistens;value;note”

        // Get variable name
        if (tkz.HasMoreTokens()) {
            name = tkz.GetNextToken();
        }
        else {
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;CVAR;%d;%s",
                                    WEBSOCK_ERROR_SYNTAX_ERROR,
                                    WEBSOCK_STR_ERROR_SYNTAX_ERROR );
            return;
        }
        
        // Check if the variable exist
        CVSCPVariable variable;
        bool bVariableExist = false;
        if ( 0 != pCtrlObject->m_variables.find( name, variable ) ) {
            bVariableExist = true;
        }
        
        // name can not start with "vscp." that is it is a stock variable
        if ( name.Lower().StartsWith( "vscp.") ) {
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;CVAR;%d;%s",
                                    WEBSOCK_ERROR_VARIABLE_NO_STOCK,
                                    WEBSOCK_STR_ERROR_VARIABLE_NO_STOCK );
            return;
        }

        // Get variable type
        bool bInputType = false;
        if ( tkz.HasMoreTokens() ) {
            wxString wxstr = tkz.GetNextToken();
            wxstr = wxstr.Trim();
            if ( wxstr.Length() ) bInputType = true;
            type = vscp_readStringValue( wxstr );
        }
        
        // Get variable access rights
        bool bInputAccessRights = false;
        if ( tkz.HasMoreTokens() ) {
            wxString wxstr = tkz.GetNextToken();
            wxstr = wxstr.Trim();
            if ( wxstr.Length() ) bInputAccessRights = true;
            accessrights = vscp_readStringValue( wxstr );
        }

        // Get variable Persistence 0/1
        bool bInputPersistent = false;
        if ( tkz.HasMoreTokens() ) {
            wxString wxstr = tkz.GetNextToken();
            wxstr = wxstr.Trim();
            if ( wxstr.Length() ) bInputPersistent = true;
            int val = vscp_readStringValue( wxstr );

            if ( 0 == val ) {
                bPersistent = false;
            }
            else if ( 1 == val ) {
                bPersistent = true;
            }
            else {
                mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                        "-;CVAR;%d;%s",
                                        WEBSOCK_ERROR_SYNTAX_ERROR,
                                        WEBSOCK_STR_ERROR_SYNTAX_ERROR );
                return;
            }
        }

        // Get variable value
        bool bInputValue = false;
        if ( tkz.HasMoreTokens() ) {
            bInputValue = true;
            value = tkz.GetNextToken(); // Keep possible coding
        }
        
        // Get variable note
        bool bInputNote = false;
        if (tkz.HasMoreTokens()) {
            bInputNote = true;
            note = tkz.GetNextToken();  // Keep possible coding
        }

        if ( !bVariableExist ) {
            
            // Add the variable
            if ( !pCtrlObject->m_variables.add( name, 
                                value, 
                                type, 
                                pSession->m_pClientItem->m_pUserItem->getUserID(), 
                                bPersistent, 
                                accessrights, 
                                note ) ) {
                mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                        "-;CVAR;%d;%s",
                                        WEBSOCK_ERROR_SYNTAX_ERROR,
                                        WEBSOCK_STR_ERROR_SYNTAX_ERROR );
                return;
            }
        }
        else {
            
            // Save changes to the variable 
            
            if ( bInputType ) {
                variable.setType( type );
            }
            
            if ( bInputAccessRights ) {
                variable.setAccessRights( accessrights );
            }
            
            // Persistence can't be changed
            
            if ( bInputValue ) {
                variable.setValue( value );
            }
            
            if ( bInputNote ) {
                variable.setNote( note );
            }
            
            // Save the changed variable
            if ( !pCtrlObject->m_variables.update( variable ) ) {
                mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                        "-;CVAR;%d;%s",
                                        WEBSOCK_ERROR_SYNTAX_ERROR,
                                        WEBSOCK_STR_ERROR_SYNTAX_ERROR );
                return;
            }
            
        }

        wxString resultstr = _("+;CVAR;");
        resultstr += name;

        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const char *)resultstr.mbc_str() );

    }
    
    // ------------------------------------------------------------------------
    //                              RVAR/READVAR
    //-------------------------------------------------------------------------
    
    else if ( ( 0 == strTok.Find(_("READVAR") ) ) || 
                ( 0 == strTok.Find(_("RVAR") ) ) ) {

        CVSCPVariable variable;
        uint8_t type;
        wxString strvalue;

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;RVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );

            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to read a variable.\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return;     // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 4 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;RVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to read a variable.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return;     // We still leave channel open
        }

        strTok = tkz.GetNextToken();
        if ( 0 == pCtrlObject->m_variables.find( strTok, variable ) ) {
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;RVAR;%d;%s",
                                    WEBSOCK_ERROR_VARIABLE_UNKNOWN,
                                    WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN );
            return;
        }

        //variable.writeValueToString(strvalue);
        //type = variable.getType();
        // name;type;bPersistent;userid;rights;lastchanged;value;note
        wxString resultstr = _("+;RVAR;");
        resultstr += variable.getAsString( false );
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const char *)resultstr.mbc_str() );
    }
    
    // ------------------------------------------------------------------------
    //                                WVAR/WRITEVAR
    //-------------------------------------------------------------------------
    
    else if ( ( 0 == strTok.Find( _("WRITEVAR") ) ) || 
                ( 0 == strTok.Find( _("WVAR") ) ) ) {

        CVSCPVariable variable;
        wxString strvalue;
        uint8_t type;

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;WVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to write a variable.\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return; // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 6 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;WVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to do write variable.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return;     // We still leave channel open
        }

        // Get variable name
        wxString strVarName;
        if (tkz.HasMoreTokens()) {


            strVarName = tkz.GetNextToken();
            if ( 0 == pCtrlObject->m_variables.find( strVarName.Upper(), variable ) ) {
                mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;WVAR;%d;%s",
                                    WEBSOCK_ERROR_VARIABLE_UNKNOWN,
                                    WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN );
                return;
            }

            // Get variable value
            if ( tkz.HasMoreTokens() ) {
                
                strTok = tkz.GetNextToken();                
                
                if ( !variable.setValueFromString( variable.getType(), 
                                                        strTok,
                                                        false ) ) {     // Preserve coding
                    mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;WVAR;%d;%s",
                                    WEBSOCK_ERROR_SYNTAX_ERROR,
                                    WEBSOCK_STR_ERROR_SYNTAX_ERROR );
                    return;
                }
                
                // Update the variable
                if ( !pCtrlObject->m_variables.update( variable ) ) {
                    mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;WVAR;%d;%s",
                                    WEBSOCK_STR_ERROR_VARIABLE_UPDATE,
                                    WEBSOCK_STR_ERROR_VARIABLE_UPDATE );
                    return;
                }
            }
            else {
                mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;WVAR;%d;%s",
                                    WEBSOCK_ERROR_SYNTAX_ERROR,
                                    WEBSOCK_STR_ERROR_SYNTAX_ERROR );
                return;
            }
        }
        else {
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;WVAR;%d;%s",
                                    WEBSOCK_ERROR_SYNTAX_ERROR,
                                    WEBSOCK_STR_ERROR_SYNTAX_ERROR );
            return;
        }

        //variable.writeValueToString( strvalue );
        //type = variable.getType();

        wxString resultstr = _( "+;WVAR;" );
        resultstr += strVarName;

        // Positive reply
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const char *)resultstr.mbc_str() );

    }
    
    // ------------------------------------------------------------------------
    //                             RSTVAR/RESETVAR
    //-------------------------------------------------------------------------
    
    else if ( ( 0 == strTok.Find(_("RESETVAR") ) ) || 
                    ( 0 == strTok.Find(_("RSTVAR") ) ) ) {

        CVSCPVariable variable;
        wxString strvalue;
        uint8_t type;

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;RSTVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            return;     // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 6 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;RSTVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to reset a variable.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return;     // We still leave channel open
        }

        strTok = tkz.GetNextToken();
        if ( 0 == pCtrlObject->m_variables.find(strTok, variable )) {
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;RSTVAR;%d;%s",
                                    WEBSOCK_ERROR_VARIABLE_UNKNOWN,
                                    WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN );
            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to reset a variable.\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return;
        }

        variable.Reset();

        variable.writeValueToString( strvalue );
        type = variable.getType();

        wxString resultstr = _( "+;RSTVAR;" );
        resultstr += strTok;
        resultstr += _( ";" );
        resultstr += wxString::Format( _( "%d" ), type );
        resultstr += _( ";" );
        resultstr += strvalue;

        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, ( const char * )resultstr.mbc_str() );

    }
    
    // ------------------------------------------------------------------------
    //                                 DELVAR/REMOVEVAR
    //-------------------------------------------------------------------------
    
    else if ( ( 0 == strTok.Find(_("DELVAR") ) ) || 
                ( 0 == strTok.Find(_("REMOVEVAR") ) ) ) {

        CVSCPVariable variable;
        wxString name;

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;DELVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to delete a variable.\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return;     // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 6 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;DELVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to delete a variable.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return;     // We still leave channel open
        }

        name = tkz.GetNextToken();
        if ( 0 == pCtrlObject->m_variables.find( name, variable ) ) {
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;DELVAR;%d;%s",
                                    WEBSOCK_ERROR_VARIABLE_UNKNOWN,
                                    WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN );
            return;
        }

        pCtrlObject->m_variableMutex.Lock();
        pCtrlObject->m_variables.remove( name );
        pCtrlObject->m_variableMutex.Unlock();

        wxString strResult = _("+;DELVAR;");
        strResult += name;
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const char *)strResult.mbc_str() );

    }
    
    // ------------------------------------------------------------------------
    //                             LENVAR/LENGTHVAR
    //-------------------------------------------------------------------------
    
    else if ( ( 0 == strTok.Find(_("LENGTHVAR") ) ) || 
                ( 0 == strTok.Find(_("LENVAR") ) ) ) {

        CVSCPVariable variable;

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;LENVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to get length of variable.\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return;     // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 4 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;LENVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to get length of variable.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return;     // We still leave channel open
        }

        strTok = tkz.GetNextToken();
        if ( 0 == pCtrlObject->m_variables.find(strTok, variable ) ) {
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;LENVAR;%d;%s",
                                    WEBSOCK_ERROR_VARIABLE_UNKNOWN,
                                    WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN );
            return;
        }

        wxString resultstr = _("+;LENVAR;");
        resultstr += strTok;
        resultstr += _(";");
        resultstr += wxString::Format(_("%d"), variable.getValue().Length() );
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const char *)resultstr.mbc_str() );

    }
    
    // ------------------------------------------------------------------------
    //                           LCVAR/LASTCHANGEVAR
    //-------------------------------------------------------------------------
    
    else if ( ( 0 == strTok.Find(_("LASTCHANGEVAR") ) ) || 
                ( 0 == strTok.Find(_("LCVAR") ) ) ) {

        CVSCPVariable variable;
        wxString strvalue;

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;LCVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to get last change date of variable.\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return;     // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 4 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;LCVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to get last change date of variable.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return;     // We still leave channel open
        }

        strTok = tkz.GetNextToken();
        if ( 0 == pCtrlObject->m_variables.find(strTok, variable ) ) {
            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;LCVAR;%d;%s",
                                    WEBSOCK_ERROR_VARIABLE_UNKNOWN,
                                    WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN );
            return;
        }

        variable.writeValueToString( strvalue );

        wxString resultstr = _("+;LCVAR;");
        resultstr += strTok;
        resultstr += _( ";" );
        resultstr += variable.getLastChange().FormatISODate() + _(" ") + variable.getLastChange().FormatISOTime();
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const char *)resultstr.mbc_str() );

    }
    
    // ------------------------------------------------------------------------
    //                               LSTVAR/LISTVAR
    //-------------------------------------------------------------------------
    
    else if ( ( 0 == strTok.Find( _("LISTVAR") ) ) || 
                    ( 0 == strTok.Find( _("LSTVAR") ) ) ) {

        CVSCPVariable variable;
        wxString strvalue;
        wxString strSearch;

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;LSTVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to list variable(s).\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return;     // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 4 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;LSTVAR;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to list variable(s).\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return;     // We still leave channel open
        }

        int i = 0;
        wxString resultstr;
        wxString strWork;
        m_pCtrlObject->m_variableMutex.Lock();
        
        if ( tkz.HasMoreTokens() ) {
            strSearch = tkz.GetNextToken();
            strSearch.Trim();
            if ( strSearch.IsEmpty() ) {
                strSearch = _("(.*)");  // list all
            }
        }
        else {
            strSearch = _("(.*)");      // List all
        }
        
        wxString wxstr;
        wxArrayString arrayVars;
        m_pCtrlObject->m_variables.getVarlistFromRegExp( arrayVars, strSearch );
    
        if ( arrayVars.Count() ) {
            
            // +;LSTVAR;ordinal;name;type;userid;accessrights;persistance;last_change
            for ( int i=0; i<arrayVars.Count(); i++ ) {
                if ( 0 != m_pCtrlObject->m_variables.find( arrayVars[ i ], variable ) ) {
                    
                    wxstr = wxString::Format( _("+;LSTVAR;%d;%zu;"), i, arrayVars.Count() );
                    wxstr += variable.getAsString();
                    
                    mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const char *)wxstr.mbc_str() );
                }
            }
    
        }
        else {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;LSTVAR;%d;%s",
                                    WEBSOCK_ERROR_VARIABLE_UNKNOWN,
                                    WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN );
        }    
        
        
        m_pCtrlObject->m_variableMutex.Unlock();
        
        
/* TODO
        // Send count
        resultstr = wxString::Format( _( "+;LISTVAR;%zu" ), m_pCtrlObject->m_VSCP_Variables.m_listVariable.GetCount() );
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, ( const char * )resultstr.mbc_str() );

        
        listVscpVariable::iterator it;
        for( it = m_pCtrlObject->m_VSCP_Variables.m_listVariable.begin();
                    it != m_pCtrlObject->m_VSCP_Variables.m_listVariable.end();
                    ++it ) {

            if ( NULL == ( pvar = *it ) ) continue;

            resultstr = _("+;LISTVAR;");
            resultstr += wxString::Format( _("%d;"), i++ );
            resultstr += pvar->getName();
            resultstr += _(";");
            strWork.Printf( _("%d"), pvar->getType() ) ;
            resultstr += strWork;
            if ( pvar->isPersistent() ) {
                resultstr += _(";true;");
            }
            else {
                resultstr += _(";false;");
            }

            pvar->writeValueToString( strWork );
            resultstr += strWork;

            mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const char *)resultstr.mbc_str() );
        }

        m_pCtrlObject->m_variableMutex.Unlock();
        */

    }
    
    
    ////////////////////////////////////////////////////////////////////////////
    //                             USER TABLES
    ////////////////////////////////////////////////////////////////////////////
    
    
    // ------------------------------------------------------------------------
    //                             GT/GETTABLE
    //-------------------------------------------------------------------------
    
    else if ( ( 0 == strTok.Find(_("GT") ) ) || 
                ( 0 == strTok.Find(_("GETTABLE") ) ) ) {
    /*
        // Format is:
        //      tablename;from;to

        // Must be authorised to do this
        if ( !pSession->bAuthenticated ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_NOT_AUTHORISED,
                                    WEBSOCK_STR_ERROR_NOT_AUTHORISED );
            pCtrlObject->logMsg ( _("[Websocket] User/host not authorised to get table.\n"),
                                        DAEMON_LOGMSG_NORMAL,
                                        DAEMON_LOGTYPE_SECURITY );

            return; // We still leave channel open
        }

        // Check privilege
        if ( (pSession->m_pClientItem->m_pUserItem->getUserRights( 0 ) & 0xf) < 4 ) {
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                    WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT );
            wxString strErr =
                        wxString::Format( _("[Websocket] User [%s] not allowed to get table.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUser().mbc_str() );

            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return; // We still leave channel open
        }

        wxString tblName;
        wxDateTime timeFrom = wxDateTime::Now();
        wxDateTime timeTo = wxDateTime::Now();
        int nRange = 0;

        // Get name of table
        if ( tkz.HasMoreTokens() ) {
            tblName = tkz.GetNextToken();
            tblName.Trim();
            tblName.Trim(false);
            tblName.MakeUpper();
        }
        else {
            // Must have a table name
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_MUST_HAVE_TABLE_NAME,
                                    WEBSOCK_STR_ERROR_MUST_HAVE_TABLE_NAME );
            wxString strErr =
                      wxString::Format( _("[Websocket] Must have a tablename to read a table.\n") );
            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_GENERAL );
            return;
        }

        // Get from-date for range
        if ( tkz.HasMoreTokens() ) {
            wxString wxstr = tkz.GetNextToken();
            timeFrom.ParseDateTime( wxstr );
            wxstr = timeFrom.FormatISODate() + _(" ") + timeFrom.FormatISOTime();
            nRange++; // Start date entered
        }

        // Get to-date for range
        if ( tkz.HasMoreTokens() ) {
            wxString wxstr = tkz.GetNextToken();
            timeTo.ParseDateTime( wxstr);
            wxstr = timeTo.FormatISODate() + _(" ") + timeTo.FormatISOTime();
            nRange++; // End date entered
        }

        if ( (nRange > 1) && timeTo.IsEarlierThan( timeFrom ) ) {

            // To date must be later then from date
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_END_DATE_IS_WRONG,
                                    WEBSOCK_STR_ERROR_END_DATE_IS_WRONG );
            wxString strErr =
                      wxString::Format( _("[Websocket] The end date must be later than the start date.\n") );
            pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_GENERAL );
            return;

        }

        CVSCPTable *ptblItem = NULL;
        pCtrlObject->m_mutexTableList.Lock();
        listVSCPTables::iterator iter;
        for (iter = pCtrlObject->m_listTables.begin(); iter != pCtrlObject->m_listTables.end(); ++iter) {
            ptblItem = *iter;
            if ( tblName== wxString::FromUTF8( ptblItem->m_vscpFileHead.nameTable ) ) {
                break;
            }
            ptblItem = NULL;
        }
        pCtrlObject->m_mutexTableList.Unlock();

        // Did we find it?
        if ( NULL == ptblItem ) {
            // nope
            mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_TABLE_NOT_FOUND,
                                    WEBSOCK_STR_ERROR_TABLE_NOT_FOUND );
            wxString strErr =
                wxString::Format( _("[Websocket] Table not found. [name=%s]\n"), tblName.wx_str() );
            pCtrlObject->logMsg( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_GENERAL );
            return;
        }

        if ( VSCP_TABLE_DYNAMIC == ptblItem->m_vscpFileHead.type ) {

            uint64_t start,end;
            if ( 0 == nRange ) {
                // Neither 'from' or 'to' given
                // Use value from header
                start = ptblItem->getTimeStampStart();
                end = ptblItem->getTimeStampStart();
            }
            else if ( 1 == nRange ) {
                // From given but no end
                start = timeFrom.GetTicks();
                end = ptblItem->getTimeStampStart();
            }
            else {
                // range given
                start = timeFrom.GetTicks();
                end = timeTo.GetTicks();
            }

            // Fetch number of records in set
            ptblItem->m_mutexThisTable.Lock();
            long nRecords = ptblItem->getRangeOfData(start, end );
            ptblItem->m_mutexThisTable.Unlock();

            if ( nRecords > 0 ) {

                struct _vscpFileRecord *pRecords = new struct _vscpFileRecord[nRecords];

                if ( NULL == pRecords ) {
                    mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_MEMORY_ALLOCATION,
                                    WEBSOCK_STR_ERROR_MEMORY_ALLOCATION );
                    wxString strErr =
                            wxString::Format( _("[Websocket] Having problems to allocate memory. [name=%s]\n"), 
                                tblName.wx_str() );
                    pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_GENERAL );
                    return;
                }

                ptblItem->m_mutexThisTable.Lock();
                long nfetchedRecords = ptblItem->getRangeOfData( start, 
                                                                    end, 
                                                                    (void *)pRecords, 
                                                                    nRecords* 
                                                                    sizeof(struct _vscpFileRecord )  );
                ptblItem->m_mutexThisTable.Unlock();

                if ( 0 == nfetchedRecords ) {
                    mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_TABLE_ERROR_READING,
                                    WEBSOCK_STR_ERROR_TABLE_ERROR_READING );
                    wxString strErr =
                        wxString::Format( _("[Websocket] Problem when reading table. [name=%s]\n"), 
                                            tblName.wx_str() );
                    pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_GENERAL );
                    return;
                }
                else {

                    wxDateTime dtStart;
                    dtStart.Set( (time_t)ptblItem->getTimeStampStart() );
                    wxString strDateTimeStart = dtStart.FormatISODate() + _("T") + dtStart.FormatISOTime();

                    wxDateTime dtEnd;
                    dtEnd.Set( (time_t)ptblItem->getTimeStampEnd() );
                    wxString strDateTimeEnd = dtEnd.FormatISODate() + _("T") + dtEnd.FormatISOTime();

                    // First send start post with number if records
                    wxString wxstr = wxString::Format(_("+;GT;START;%d;%d;%s;%s"),
                                                nfetchedRecords,
                                                ptblItem->getNumberOfRecords(),
                                                strDateTimeStart.wx_str(),
                                                strDateTimeEnd.wx_str() );
                    mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    (const char *)wxstr.mbc_str() );

                    // Then send measurement records
                    for ( long i=0; i<nfetchedRecords; i++ ) {
                        wxDateTime dt;
                        dt.Set( (time_t)pRecords[i].timestamp );
                        wxString strDateTime = dt.FormatISODate() + _(" ") + dt.FormatISOTime();
                        wxString wxstr = wxString::Format(_("+;GT;%d;%s;%f"),
                                                i,
                                                strDateTime.wx_str(),
                                                pRecords[i].measurement );
                        mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    wxstr.mbc_str() );
                    }

                    // Last send end post with number if records
                    mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "+;GT;END;%d",
                                    nfetchedRecords );
                }

                // Deallocate storage
                delete[] pRecords;

            }
            else {
                if ( 0 == nRecords ) {
                    mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_TABLE_NO_DATA,
                                    WEBSOCK_STR_ERROR_TABLE_NO_DATA );
                    wxString strErr =
                         wxString::Format( _("[Websocket] No data in table. [name=%s]\n"), tblName.wx_str() );
                    pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_GENERAL );
                    return;
                }
                else {
                    mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_TABLE_ERROR_READING,
                                    WEBSOCK_STR_ERROR_TABLE_ERROR_READING );
                    wxString strErr =
                         wxString::Format( _("[Websocket] Problem when reading table. [name=%s]\n"), tblName.wx_str() );
                    pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_GENERAL );
                    return;
                }
            }

        }
        // OK STATIC table
        else {

            // Fetch number of records in set
            ptblItem->m_mutexThisTable.Lock();
            long nRecords = ptblItem->getStaticRequiredBuffSize();
            ptblItem->m_mutexThisTable.Unlock();

            if ( nRecords > 0 ) {

                struct _vscpFileRecord *pRecords = new struct _vscpFileRecord[nRecords];

                if ( NULL == pRecords ) {
                        mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_MEMORY_ALLOCATION,
                                    WEBSOCK_STR_ERROR_MEMORY_ALLOCATION );
                        wxString strErr =
                             wxString::Format( _("[Websocket] Having problems to allocate memory. [name=%s]\n"), tblName.wx_str() );
                        pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_GENERAL );
                        return;
                }

                // Fetch data
                long nfetchedRecords = ptblItem->getStaticData( pRecords, sizeof( pRecords ) );

                // First send start post with number if records
                mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "+;GT;START;%d",
                                    nfetchedRecords );

                // Then send measurement records
                for ( long i=0; i<nfetchedRecords; i++ ) {
                    mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "+;GT;%d;%d;%f",
                                    i, pRecords[i].timestamp, pRecords[i].measurement );
                }

                // Last send end post with number if records
                mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "+;GT;END;%d",
                                    nfetchedRecords );

               // Deallocate storage
               delete [] pRecords;

            }
            else {
                if ( 0 == nRecords ) {
                    mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_TABLE_NO_DATA,
                                    WEBSOCK_STR_ERROR_TABLE_NO_DATA );
                    wxString strErr =
                         wxString::Format( _("[Websocket] No data in table. [name=%s]\n"), tblName.wx_str() );
                    pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_GENERAL );
                    return;
                }
                else {
                    mg_printf_websocket_frame( nc,
                                    WEBSOCKET_OP_TEXT,
                                    "-;GT;%d;%s",
                                    WEBSOCK_ERROR_TABLE_ERROR_READING,
                                    WEBSOCK_STR_ERROR_TABLE_ERROR_READING );
                    wxString strErr =
                         wxString::Format( _("[Websocket] Problem when reading table. [name=%s]\n"), tblName.wx_str() );
                    pCtrlObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_GENERAL );
                    return;
                }
            }

        }
    */
    }
    
    // ------------------------------------------------------------------------
    //                              VERSION
    //-------------------------------------------------------------------------
    
    else if (0 == strTok.Find(_("VERSION"))) {
        
        wxString strvalue;

        wxString resultstr = _("+;VERSION;");
        resultstr += VSCPD_DISPLAY_VERSION;
        resultstr += _(";");
        resultstr += wxString::Format(_("%d.%d.%d.%d"), VSCPD_MAJOR_VERSION,
                                                            VSCPD_MINOR_VERSION,
                                                            VSCPD_RELEASE_VERSION,
                                                            VSCPD_BUILD_VERSION );
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const char *)resultstr.mbc_str() );
        
    }
    
    // ------------------------------------------------------------------------
    //                              COPYRIGHT
    //-------------------------------------------------------------------------
    
    else if (0 == strTok.Find(_("COPYRIGHT"))) {
        
        wxString strvalue;

        wxString resultstr = _("+;COPYRIGHT;");
        resultstr += VSCPD_COPYRIGHT;
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const char *)resultstr.mbc_str() );
        
    }

    // ------------------------------------------------------------------------
    //                              TABLE
    //
    //  READ, WRITE, ADD, DEL, LIST, LISTALL
    //-------------------------------------------------------------------------
    else if (0 == strTok.Find(_("TABLE"))) {
        
    }
    else {
        mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT,
                                    "-;;%d;%s",
                                    WEBSOCK_ERROR_UNKNOWN_COMMAND,
                                    WEBSOCK_STR_ERROR_UNKNOWN_COMMAND );
    }


    return;
}




///////////////////////////////////////////////////////////////////////////////
// websock_sendevent
//

bool
VSCPWebServerThread::websock_sendevent( struct mg_connection *nc,
                                            websock_session *pSession,
                                            vscpEvent *pEvent )
{
    bool bSent = false;
    bool rv = true;

    // Check pointer
    if (NULL == nc) return false;
    if (NULL == pSession) return false;


    CControlObject *pObject = (CControlObject *)nc->mgr->user_data;
    if (NULL == pObject) return false;

    // Level II events betwen 512-1023 is recognized by the daemon and
    // sent to the correct interface as Level I events if the interface
    // is addressed by the client.
    if ((pEvent->vscp_class <= 1023) &&
            (pEvent->vscp_class >= 512) &&
            (pEvent->sizeData >= 16)) {

        // This event shold be sent to the correct interface if it is
        // available on this machine. If not it should be sent to
        // the rest of the network as normal

        cguid destguid;
        destguid.getFromArray(pEvent->pdata);
        destguid.setAt(0,0);
        destguid.setAt(1,0);
        //unsigned char destGUID[16];
        //memcpy(destGUID, pEvent->pdata, 16); // get destination GUID
        //destGUID[0] = 0; // Interface GUID's have LSB bytes nilled
        //destGUID[1] = 0;

        pObject->m_wxClientMutex.Lock();

        // Find client
        CClientItem *pDestClientItem = NULL;
        VSCPCLIENTLIST::iterator iter;
        for (iter = pObject->m_clientList.m_clientItemList.begin();
                iter != pObject->m_clientList.m_clientItemList.end();
                ++iter) {

            CClientItem *pItem = *iter;
            if ( pItem->m_guid == destguid ) {
                // Found
                pDestClientItem = pItem;
                break;
            }

        }

        if (NULL != pDestClientItem) {

            // If the client queue is full for this client then the
            // client will not receive the message
            if (pDestClientItem->m_clientInputQueue.GetCount() <=
                        pObject->m_maxItemsInClientReceiveQueue) {

                // Create copy of event
                vscpEvent *pnewEvent = new vscpEvent;

                if (NULL != pnewEvent) {

                    pnewEvent->pdata = NULL;

                    vscp_copyVSCPEvent(pnewEvent, pEvent);

                    // Add the new event to the inputqueue
                    pDestClientItem->m_mutexClientInputQueue.Lock();
                    pDestClientItem->m_clientInputQueue.Append(pnewEvent);
                    pDestClientItem->m_semClientInputQueue.Post();
                    pDestClientItem->m_mutexClientInputQueue.Unlock();

                    bSent = true;
                }
                else {
                    bSent = false;
                }

            }
            else {
                // Overun - No room for event
                //vscp_deleteVSCPevent(pEvent);
                bSent = true;
                rv = false;
            }

        } // Client found

        pObject->m_wxClientMutex.Unlock();

    }

    if (!bSent) {

        // There must be room in the send queue
        if (pObject->m_maxItemsInClientReceiveQueue >
                pObject->m_clientOutputQueue.GetCount()) {

            // Create copy of event
            vscpEvent *pnewEvent = new vscpEvent;

            if (NULL != pnewEvent) {

                pnewEvent->pdata = NULL;

                vscp_copyVSCPEvent(pnewEvent, pEvent);

                pObject->m_mutexClientOutputQueue.Lock();
                pObject->m_clientOutputQueue.Append(pnewEvent);
                pObject->m_semClientOutputQueue.Post();
                pObject->m_mutexClientOutputQueue.Unlock();

            }

        }
        else {
            rv = false;
        }
    }

    return rv;
}


///////////////////////////////////////////////////////////////////////////////
// websrv_websocket_message
//

bool
VSCPWebServerThread::websrv_websocket_message( struct mg_connection *nc,
                                                    struct http_message *hm,
                                                    struct websocket_message *wm )
{
    wxString wxstr;
    wxString strCmd;
    char buf[ 2048 ];
    websock_session *pSession;
    const char *p = buf;

   memset( buf, 0, sizeof( buf ) );

    // Check pointer
    if (NULL == nc) return false;
    if (NULL == hm) return false;
    if (NULL == wm) return false;
   
    // Copy data
    if ( wm->size ) memcpy( buf, wm->data, MIN(wm->size, sizeof( buf ) ) );

    pSession = (websock_session *)nc->user_data;
    if (NULL == pSession) return false;

    CControlObject *pObject = (CControlObject *)nc->mgr->user_data;
    if (NULL == pObject) return false;

    memset(buf, 0, sizeof( buf));
    memcpy(buf, (char *)wm->data, MIN( wm->size, sizeof( buf ) ) );

    switch (*p) {

        // Command - | 'C' | command type (byte) | data |
        case 'C':
            p++;
            p++; // Point beyond initial info "C;"
            strCmd = wxString::FromUTF8( p );
            websock_command( nc, hm, wm, pSession, strCmd );
            break;

        // Event | 'E' ; head(byte) , vscp_class(unsigned short) , vscp_type(unsigned
        //              short) , GUID(16*byte), data(0-487 bytes) |
        case 'E':
        {
            // Must be authorised to do this
            if ( !pSession->bAuthenticated ) {
                mg_printf_websocket_frame( nc,
                                            WEBSOCKET_OP_TEXT,
                                            "-;%d;%s",
                                            WEBSOCK_ERROR_NOT_AUTHORISED,
                                            WEBSOCK_STR_ERROR_NOT_AUTHORISED );
                return true;
            }

            p++;
            p++; // Point beyond initial info "E;"
            vscpEvent vscp_event;
            wxstr = wxString::FromAscii( p );

            if ( vscp_setVscpEventFromString( &vscp_event, wxstr ) ) {
                
                // If GUID is all null give it GUID of interface
                if ( vscp_isGUIDEmpty( vscp_event.GUID ) ) {
                    pSession->m_pClientItem->m_guid.writeGUID( vscp_event.GUID );
                }

                // Check if this user is allowed to send this event
                if ( !pSession->m_pClientItem->m_pUserItem->isUserAllowedToSendEvent( vscp_event.vscp_class, vscp_event.vscp_type ) ) {
                    wxString strErr =
                        wxString::Format( _("websocket] User [%s] not allowed to send event class=%d type=%d.\n"),
                                                pSession->m_pClientItem->m_pUserItem->getUserName().mbc_str(),
                                                vscp_event.vscp_class, vscp_event.vscp_type );

                    pObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
                }

                vscp_event.obid = pSession->m_pClientItem->m_clientID;
                if ( 1/*pObject->getWebServer()->websock_sendevent( nc, pSession, &vscp_event )*/ ) {
                    mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, "+;EVENT" );
                }
                else {
                    mg_printf_websocket_frame( nc,
                                            WEBSOCKET_OP_TEXT,
                                            "-;%d;%s",
                                            WEBSOCK_ERROR_TX_BUFFER_FULL,
                                            WEBSOCK_STR_ERROR_TX_BUFFER_FULL );
                }
            }

        }
        break;

        // Unknow command
        default:
            break;

    }

    return true;
}


///////////////////////////////////////////////////////////////////////////////
// websock_authentication
//
// client sends
//      "AUTH;user;hash"
//  where the hash is based on
//      "user:standard vscp password hash:server generated hash(sid)"
//  "user;hash" is received in strKey

bool
VSCPWebServerThread::websock_authentication( struct mg_connection *nc,
                                                struct http_message *hm,
                                                websock_session *pSession,
                                                wxString& strUser,
                                                wxString& strKey )
{
    bool rv = false;
    char response[33];
    char expected_response[33];
    bool bValidHost = false;

    memset( response, 0, sizeof( response ) );
    memset( expected_response, 0, sizeof( expected_response ) );

    // Check pointer
    if (NULL == nc) return false;
    if (NULL == hm) return false;
    if (NULL == pSession) return false;

    CControlObject *pObject = (CControlObject *)nc->mgr->user_data;
    if (NULL == pObject) return false;

    if ( pObject->m_bAuthWebsockets ) {

        // Check if user is valid
        CUserItem *pUser = pObject->m_userList.getUser( strUser );
        if ( NULL == pUser ) return false;

        // Check if remote ip is valid
        bValidHost = pUser->isAllowedToConnect( wxString::FromAscii( inet_ntoa( nc->sa.sin.sin_addr ) ) );

        if ( !bValidHost ) {
            // Log valid login
            wxString strErr =
            wxString::Format( _("[Websocket Client] Host [%s] NOT allowed to connect.\n"),
                    wxString::FromAscii( (const char *)inet_ntoa( nc->sa.sin.sin_addr ) ).wx_str() );

            pObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
            return false;
        }

        strncpy( response, strKey.mbc_str(), MIN( sizeof(response), strKey.Length() ) );

        /*static const char colon[] = ":";
        static const size_t one = 1;        // !!!!! Length must be size_t  !!!!!
        static const size_t n32 = 32;       // !!!!! Length must be size_t  !!!!!
        cs_md5( expected_response,
                    (const char *)strUser.mbc_str(), strUser.Length(),
                    colon, one,
                    (const char *)pUser->getPasswordDomain().mbc_str(), pUser->getPasswordDomain().Length(),
                    colon, one,
                    pSession->m_sid, n32,
                    NULL );*/
        
        unsigned char hash[16];
        cs_md5_ctx ctx;

        cs_md5_init( &ctx );
        cs_md5_update( &ctx, (const unsigned char *)( (const char *)strUser.mbc_str() ), strUser.Length() );
        cs_md5_update( &ctx, (const unsigned char *)":", 1 );
        cs_md5_update( &ctx, (const unsigned char *)( (const char *)pUser->getPasswordDomain().mbc_str() ), 
                                                 pUser->getPasswordDomain().Length() );
        cs_md5_update( &ctx, (const unsigned char *)":", 1 );
        cs_md5_update( &ctx, (const unsigned char *)pSession->m_sid, 32 );
        cs_md5_final( hash, &ctx );
        cs_to_hex( expected_response, hash, sizeof( hash ) );

        rv = ( vscp_strcasecmp( response, expected_response ) == 0 ) ? true : false;

        if (  rv ) {

            pSession->m_pClientItem->bAuthenticated = true;

            // Add user to client
            pSession->m_pClientItem->m_pUserItem = pUser;

            // Copy in the user filter
            memcpy( &pSession->m_pClientItem->m_filterVSCP,
                        pUser->getFilter(),
                        sizeof( vscpEventFilter ) );

            // Log valid login
            wxString strErr =
                wxString::Format( _("[Websocket Client] Host [%s] User [%s] allowed to connect.\n"),
                                    wxString::FromAscii( (const char *)inet_ntoa( nc->sa.sin.sin_addr ) ).wx_str(),
                                    strUser.wx_str() );

            pObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );

        }
        else {
            // Log valid login
            wxString strErr =
            wxString::Format( _("[Websocket Client] user [%s] NOT allowed to connect.\n"),
                                             strUser.wx_str() );

            pObject->logMsg ( strErr, DAEMON_LOGMSG_NORMAL, DAEMON_LOGTYPE_SECURITY );
        }
    }

    return rv;
}

///////////////////////////////////////////////////////////////////////////////
// websock_new_session
//

websock_session *
VSCPWebServerThread::websock_new_session( struct mg_connection *nc,
                                            struct http_message *hm )
{
    char buf[512];
    char ws_version[10];
    char ws_key[33];
    websock_session *pSession = NULL;

    // Check pointer
    if (NULL == nc) return NULL;

    CControlObject *pObject = (CControlObject *)nc->mgr->user_data;
    if (NULL == pObject) return NULL;

    mg_str *phdr;

    // user
    if ( NULL != ( phdr = mg_get_http_header( hm, "Sec-WebSocket-Version" ) ) ) {
        memset( ws_version, 0, sizeof( ws_version ) );
        strncpy( ws_version, phdr->p, MIN( phdr->len, sizeof( ws_version ) ) );
    }
    if ( NULL != ( phdr = mg_get_http_header( hm, "Sec-WebSocket-Key" ) ) ) {
        memset( ws_key, 0, sizeof( ws_key ) );
        strncpy( ws_key, phdr->p, MIN( phdr->len, sizeof( ws_key ) ) );
    }

    // create fresh session
    pSession = (websock_session *)calloc(1, sizeof( websock_session ) );
    if  (NULL == pSession ) {
#ifndef WIN32
        syslog( LOG_ERR, "calloc error: %s\n", strerror(errno)); // TODO  error report Linux
#endif
        return NULL;
    }

    memset( pSession->m_sid, 0, sizeof( pSession->m_sid ) );
    memset( pSession->m_key, 0, sizeof( pSession->m_key ) );

    // Generate a random session ID
    time_t t;
    t = time( NULL );
    sprintf( buf,
                "__%s_%X%X%X%X_be_hungry_stay_foolish_%X%X",
                ws_key,
                (unsigned int)rand(),
                (unsigned int)rand(),
                (unsigned int)rand(),
                (unsigned int)t,
                (unsigned int)rand(),
                1337 );

    memset( pSession->m_sid, 0, sizeof( pSession->m_sid ) );
    //cs_md5( pSession->m_sid, buf, strlen( buf ), NULL );
    vscp_md5( pSession->m_sid, (const unsigned char *)buf, strlen( buf ) ); 

    // Init.
    strcpy( pSession->m_key, ws_key );           // Save key
    pSession->bAuthenticated = false;            // Not authenticated in yet
    pSession->m_version = atoi( ws_version );    // Store protocol version
    pSession->m_pClientItem = new CClientItem(); // Create client
    vscp_clearVSCPFilter(&pSession->m_pClientItem->m_filterVSCP);    // Clear filter
    pSession->bTrigger = false;
    pSession->triggerTimeout = 0;

    // This is an active client
    pSession->m_pClientItem->m_bOpen = false;
    pSession->m_pClientItem->m_type = CLIENT_ITEM_INTERFACE_TYPE_CLIENT_WEBSOCKET;
    pSession->m_pClientItem->m_strDeviceName = _("Internal daemon websocket client. ");
    wxDateTime now = wxDateTime::Now();
    pSession->m_pClientItem->m_strDeviceName += now.FormatISODate();
    pSession->m_pClientItem->m_strDeviceName += _(" ");
    pSession->m_pClientItem->m_strDeviceName += now.FormatISOTime();
    //pSession->m_pClientItem->m_strDeviceName += _(")");

    // Add the client to the Client List
    pObject->m_wxClientMutex.Lock();
    pObject->addClient( pSession->m_pClientItem );
    pObject->m_wxClientMutex.Unlock();

    // Add to linked list
    pSession->m_referenceCount++;
    pSession->lastActiveTime = time(NULL);

    m_websockSessionMutex.Lock();
    gpobj->m_websocketSessions.Append( pSession );
    m_websockSessionMutex.Unlock();

    return pSession;
}


///////////////////////////////////////////////////////////////////////////////
// websock_expire_sessions
//

/*
void
VSCPWebServerThread::websock_expire_sessions( struct mg_connection *nc,
                                                struct http_message *hm )
{
    websock_session *pos;
    websock_session *prev;
    websock_session *next;
    time_t now;

    //return;

    // Check pointer
    if (NULL == nc) return;

    CControlObject *pObject = (CControlObject *)nc->mgr->user_data;
    if (NULL == pObject) return;

    now = time( NULL );
    prev = NULL;
    pos = gp_websock_sessions;

    m_websockSessionMutex.Lock();

    while ( NULL != pos ) {

        next = pos->m_next;

        if ( ( now - pos->lastActiveTime ) > ( WEBSOCKET_EXPIRE_TIME) ) {

            // expire sessions after WEBSOCKET_EXPIRE_TIME
            if ( NULL == prev ) {
                // First pos in list
                gp_websock_sessions = pos->m_next;
            }
            else {
                // Point to item after this one
                prev->m_next = next;
            }

            // Remove client and session item
            pObject->m_wxClientMutex.Lock();
            pObject->removeClient( pos->m_pClientItem );
            pos->m_pClientItem = NULL;
            pObject->m_wxClientMutex.Unlock();
            
            // Free session data
            free( pos );

        }
        else {
            prev = pos;
        }

        pos = next;
    }

    m_websockSessionMutex.Unlock();
}
*/

///////////////////////////////////////////////////////////////////////////////
// websocket_post_incomingEvent
//

void
VSCPWebServerThread::websock_post_incomingEvents( void )
{
    struct mg_connection *nc;
    


    // Iterate over all connections, and push current time message to websocket ones.
    for ( nc = mg_next( &gmgr, NULL);
               nc != NULL;
               nc = mg_next( &gmgr, nc) ) {

        if ( NULL == nc ) return;   // This should not happen

        CControlObject *pObject = (CControlObject *)nc->mgr->user_data;
        if (NULL == pObject) return;

        if ( nc->flags | MG_F_IS_WEBSOCKET ) {

            websock_session *pSession = (websock_session *)nc->user_data;
            if ( NULL == pSession) continue;
            
            // Should be a client item... hmm.... client disconnected 
            if ( NULL == pSession->m_pClientItem ) {
                continue;
            }

            if ( pSession->m_pClientItem->m_bOpen &&
                    pSession->m_pClientItem->m_clientInputQueue.GetCount() ) {

                CLIENTEVENTLIST::compatibility_iterator nodeClient;
                vscpEvent *pEvent;

                pSession->m_pClientItem->m_mutexClientInputQueue.Lock();    // 1
                nodeClient = pSession->m_pClientItem->m_clientInputQueue.GetFirst();
                if ( NULL == nodeClient )  continue;
                pEvent = nodeClient->GetData();                             // 1
                pSession->m_pClientItem->m_clientInputQueue.DeleteNode(nodeClient);
                pSession->m_pClientItem->m_mutexClientInputQueue.Unlock();

                if ( NULL != pEvent ) {

                    if (vscp_doLevel2Filter(pEvent, &pSession->m_pClientItem->m_filterVSCP)) {

                        wxString str;
                        if (vscp_writeVscpEventToString(pEvent, str)) {

                            // Write it out
                            char buf[ 512 ];
                            memset((char *) buf, 0, sizeof( buf));
                            strcpy((char *) buf, (const char*) "E;");
                            strcat((char *) buf, (const char*) str.mb_str(wxConvUTF8));
                            mg_send_websocket_frame(nc, WEBSOCKET_OP_TEXT, buf, strlen(buf) );

                        }
                    }

                    // Remove the event
                    vscp_deleteVSCPevent(pEvent);

                } // Valid pEvent pointer
                
            } // events available
            
        } // websocket

    } // for

 }


