// vscp_client_socketcan.cpp
//
// CANAL client communication classes.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version
// 2 of the License, or (at your option) any later version.
//
// This file is part of the VSCP (https://www.vscp.org)
//
// Copyright:   © 2007-2021
// Ake Hedman, the VSCP project, <info@vscp.org>
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

// !!! Only Linux  !!!

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <sys/socket.h>

#include <linux/can/raw.h>

#include <expat.h>

#include <vscp.h>
#include <vscp_class.h>
#include <vscp_type.h>
#include <vscphelper.h>
#include <guid.h>
#include "vscp_client_socketcan.h"

#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <string>

#include <json.hpp> // Needs C++11  -std=c++11
#include <mustache.hpp>

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

// https://github.com/nlohmann/json
using json = nlohmann::json;
using namespace kainjow::mustache;

// Forward declaration
static void *workerThread(void *pObj);

// CAN DLC to real data length conversion helpers 
static const unsigned char canal_tbldlc2len[] = {
    0, 1, 2, 3, 4, 5, 6, 7,
	8, 12, 16, 20, 24, 32, 48, 64 };

// get data length from can_dlc with sanitized can_dlc 
unsigned char canal_dlc2len(unsigned char can_dlc)
{
	return canal_tbldlc2len[can_dlc & 0x0F];
}

static const unsigned char canal_tbllen2dlc[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8,		    /* 0 - 8 */
	9, 9, 9, 9,				            /* 9 - 12 */
	10, 10, 10, 10,				        /* 13 - 16 */
	11, 11, 11, 11,				        /* 17 - 20 */
	12, 12, 12, 12,				        /* 21 - 24 */
	13, 13, 13, 13, 13, 13, 13, 13,		/* 25 - 32 */
	14, 14, 14, 14, 14, 14, 14, 14,		/* 33 - 40 */
	14, 14, 14, 14, 14, 14, 14, 14,		/* 41 - 48 */
	15, 15, 15, 15, 15, 15, 15, 15,		/* 49 - 56 */
	15, 15, 15, 15, 15, 15, 15, 15 };	/* 57 - 64 */

// map the sanitized data length to an appropriate data length code 
unsigned char canal_len2dlc(unsigned char len)
{
	if (len > 64) {
		return 0xF;
  }

	return canal_tbllen2dlc[len];
}

///////////////////////////////////////////////////////////////////////////////
// C-tor
//

vscpClientSocketCan::vscpClientSocketCan() 
{
    m_type = CVscpClient::connType::SOCKETCAN;
    m_bDebug     = false;
    m_bConnected = false;  // Not connected
    m_threadWork = 0;
    m_bRun = true;
    m_interface    = "vcan0";
    m_guid.getFromString("00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00");
    m_flags = 0;
    //m_socket = -1;
    m_mode = CAN_MTU;

    setResponseTimeout(3);  // Response timeout 3 ms
    pthread_mutex_init(&m_mutexif,NULL);

    vscp_clearVSCPFilter(&m_filterIn);  // Accept all events
    vscp_clearVSCPFilter(&m_filterOut); // Send all events

    sem_init(&m_semSendQueue, 0, 0);
    sem_init(&m_semReceiveQueue, 0, 0);

    pthread_mutex_init(&m_mutexSendQueue, NULL);
    pthread_mutex_init(&m_mutexReceiveQueue, NULL);

 /*
    // Init pool
    spdlog::init_thread_pool(8192, 1);

    // Flush log every five seconds
    spdlog::flush_every(std::chrono::seconds(5));

    auto console = spdlog::stdout_color_mt("console");
    // Start out with level=info. Config may change this
    console->set_level(spdlog::level::debug);
    console->set_pattern("[vscp-client-socketcan] [%^%l%$] %v");
    spdlog::set_default_logger(console);

    console->debug("Starting the vscp-client-socketcan...");

    m_bConsoleLogEnable = true;
    m_consoleLogLevel   = spdlog::level::info;
    m_consoleLogPattern = "[vscp-client-socketcan %c] [%^%l%$] %v";

    m_bEnableFileLog   = true;
    m_fileLogLevel     = spdlog::level::info;
    m_fileLogPattern   = "[vscp-client-socketcan %c] [%^%l%$] %v";
    m_path_to_log_file = "/var/log/vscp/vscp-client-socketcan.log";
    m_max_log_size     = 5242880;
    m_max_log_files    = 7;
*/
}

///////////////////////////////////////////////////////////////////////////////
// D-tor
//

vscpClientSocketCan::~vscpClientSocketCan() 
{
    disconnect();
    pthread_mutex_destroy(&m_mutexif);

    sem_destroy(&m_semSendQueue);
    sem_destroy(&m_semReceiveQueue);

    pthread_mutex_destroy(&m_mutexSendQueue);
    pthread_mutex_destroy(&m_mutexReceiveQueue);
}

///////////////////////////////////////////////////////////////////////////////
// init
//

int vscpClientSocketCan::init(const std::string &interface, 
                                const std::string &guid,
                                unsigned long flags,
                                uint32_t timeout)
{
    m_interface = interface;
    m_guid.getFromString(guid);
    m_flags = flags;
    setResponseTimeout(DEAULT_RESPONSE_TIMEOUT);  // Response timeout 3 ms
    return VSCP_ERROR_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// getConfigAsJson
//

std::string vscpClientSocketCan::getConfigAsJson(void) 
{
    json j;
    std::string rv;
    
    j["interface"] = m_interface;
    j["flags"] = m_flags;
    j["response-timeout"] = getResponseTimeout();

    return rv;
}


///////////////////////////////////////////////////////////////////////////////
// initFromJson
//

bool vscpClientSocketCan::initFromJson(const std::string& config)
{
  json j;

  try {

    m_j_config = json::parse(config);

    // SocketCAN interface
    if (m_j_config.contains("device")) {
      m_interface = m_j_config["device"].get<std::string>();
      spdlog::debug("json socketcan init: interface set to {}.", m_interface);
    }

    // flags
    if (m_j_config.contains("flags")) {
      m_flags = m_j_config["flags"].get<uint32_t>();
      spdlog::debug("json socket init: host set to {}.", m_flags);
    }

    // Response timeout
    if (m_j_config.contains("response-timeout")) {
      uint32_t val = m_j_config["response-timeout"].get<uint32_t>();
      setResponseTimeout(val);
      spdlog::debug("json socket init: Response Timeout set to {}.", val);

    }

/*
    // Logging
    if (m_j_config.contains("logging") && m_j_config["logging"].is_object()) {

      json j = m_j_config["logging"];

      // * * *  CONSOLE  * * *

      // Logging: console-log-enable
      if (j.contains("console-enable")) {
        try {
          m_bConsoleLogEnable = j["console-enable"].get<bool>();
        }
        catch (const std::exception &ex) {
          spdlog::error("Failed to read 'console-enable' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error("Failed to read 'console-enable' due to unknown error.");
        }
      }
      else {
        spdlog::debug("Failed to read LOGGING 'console-enable' Defaults will be used.");
      }

      // Logging: console-log-level
      if (j.contains("console-level")) {
        std::string str;
        try {
          str = j["console-level"].get<std::string>();
        }
        catch (const std::exception &ex) {
          spdlog::error("Failed to read 'console-level' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error("Failed to read 'console-level' due to unknown error.");
        }
        vscp_makeLower(str);
        if (std::string::npos != str.find("off")) {
          m_consoleLogLevel = spdlog::level::off;
        }
        else if (std::string::npos != str.find("critical")) {
          m_consoleLogLevel = spdlog::level::critical;
        }
        else if (std::string::npos != str.find("err")) {
          m_consoleLogLevel = spdlog::level::err;
        }
        else if (std::string::npos != str.find("warn")) {
          m_consoleLogLevel = spdlog::level::warn;
        }
        else if (std::string::npos != str.find("info")) {
          m_consoleLogLevel = spdlog::level::info;
        }
        else if (std::string::npos != str.find("debug")) {
          m_consoleLogLevel = spdlog::level::debug;
        }
        else if (std::string::npos != str.find("trace")) {
          m_consoleLogLevel = spdlog::level::trace;
        }
        else {
          spdlog::error("Failed to read LOGGING 'console-level' has invalid "
                        "value [{}]. Default value used.",
                        str);
        }
      }
      else {
        spdlog::error("Failed to read LOGGING 'console-level' Defaults will be used.");
      }

      // Logging: console-log-pattern
      if (j.contains("console-pattern")) {
        try {
          m_consoleLogPattern = j["console-pattern"].get<std::string>();
        }
        catch (const std::exception &ex) {
          spdlog::error("Failed to read 'console-pattern' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error("Failed to read 'console-pattern' due to unknown error.");
        }
      }
      else {
        spdlog::debug("Failed to read LOGGING 'console-pattern' Defaults will be used.");
      }

      // * * *  FILE  * * *

      // Logging: file-log-enable
      if (j.contains("file-enable")) {
        try {
          m_bEnableFileLog = j["file-enable"].get<bool>();
        }
        catch (const std::exception &ex) {
          spdlog::error("Failed to read 'file-enable' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error("Failed to read 'file-enable' due to unknown error.");
        }
      }
      else {
        spdlog::debug("Failed to read LOGGING 'file-enable' Defaults will be used.");
      }

      // Logging: file-log-level
      if (j.contains("file-log-level")) {
        std::string str;
        try {
          str = j["file-log-level"].get<std::string>();
        }
        catch (const std::exception &ex) {
          spdlog::error("Failed to read 'file-log-level' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error("Failed to read 'file-log-level' due to unknown error.");
        }
        vscp_makeLower(str);
        if (std::string::npos != str.find("off")) {
          m_fileLogLevel = spdlog::level::off;
        }
        else if (std::string::npos != str.find("critical")) {
          m_fileLogLevel = spdlog::level::critical;
        }
        else if (std::string::npos != str.find("err")) {
          m_fileLogLevel = spdlog::level::err;
        }
        else if (std::string::npos != str.find("warn")) {
          m_fileLogLevel = spdlog::level::warn;
        }
        else if (std::string::npos != str.find("info")) {
          m_fileLogLevel = spdlog::level::info;
        }
        else if (std::string::npos != str.find("debug")) {
          m_fileLogLevel = spdlog::level::debug;
        }
        else if (std::string::npos != str.find("trace")) {
          m_fileLogLevel = spdlog::level::trace;
        }
        else {
          spdlog::error("Failed to read LOGGING 'file-log-level' has invalid value "
                        "[{}]. Default value used.",
                        str);
        }
      }
      else {
        spdlog::error("Failed to read LOGGING 'file-log-level' Defaults will be used.");
      }

      // Logging: file-log-pattern
      if (j.contains("file-log-pattern")) {
        try {
          m_fileLogPattern = j["file-log-pattern"].get<std::string>();
        }
        catch (const std::exception &ex) {
          spdlog::error("Failed to read 'file-log-pattern' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error("Failed to read 'file-log-pattern' due to unknown error.");
        }
      }
      else {
        spdlog::debug("Failed to read LOGGING 'file-log-pattern' Defaults will be used.");
      }

      // Logging: file-log-path
      if (j.contains("file-log-path")) {
        try {
          m_path_to_log_file = j["file-log-path"].get<std::string>();
        }
        catch (const std::exception &ex) {
          spdlog::error("Failed to read 'file-log-path' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error("Failed to read 'file-log-path' due to unknown error.");
        }
      }
      else {
        spdlog::error(" Failed to read LOGGING 'file-log-path' Defaults will be used.");
      }

      // Logging: file-log-max-size
      if (j.contains("file-log-max-size")) {
        try {
          m_max_log_size = j["file-log-max-size"].get<uint32_t>();
        }
        catch (const std::exception &ex) {
          spdlog::error("Failed to read 'file-log-max-size' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error("Failed to read 'file-log-max-size' due to unknown error.");
        }
      }
      else {
        spdlog::error("Failed to read LOGGING 'file-log-max-size' Defaults will be used.");
      }

      // Logging: file-log-max-files
      if (j.contains("file-log-max-files")) {
        try {
          m_max_log_files = j["file-log-max-files"].get<uint16_t>();
        }
        catch (const std::exception &ex) {
          spdlog::error("Failed to read 'file-log-max-files' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error("Failed to read 'file-log-max-files' due to unknown error.");
        }
      }
      else {
        spdlog::error("Failed to read LOGGING 'file-log-max-files' Defaults will be used.");
      }

    } // Logging
    else {
      spdlog::error("No logging has been setup.");
    }

    ///////////////////////////////////////////////////////////////////////////
    //                          Setup logger
    ///////////////////////////////////////////////////////////////////////////

    // Console log
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    if (m_bConsoleLogEnable) {
      console_sink->set_level(m_consoleLogLevel);
      console_sink->set_pattern(m_consoleLogPattern);
    }
    else {
      // If disabled set to off
      console_sink->set_level(spdlog::level::off);
    }

    // auto rotating =
    // std::make_shared<spdlog::sinks::rotating_file_sink_mt>("log_filename",
    // 1024*1024, 5, false);
    auto rotating_file_sink =
      std::make_shared<spdlog::sinks::rotating_file_sink_mt>(m_path_to_log_file.c_str(), m_max_log_size, m_max_log_files);

    if (m_bEnableFileLog) {
      rotating_file_sink->set_level(m_fileLogLevel);
      rotating_file_sink->set_pattern(m_fileLogPattern);
    }
    else {
      // If disabled set to off
      rotating_file_sink->set_level(spdlog::level::off);
    }

    std::vector<spdlog::sink_ptr> sinks{ console_sink, rotating_file_sink };
    auto logger = std::make_shared<spdlog::async_logger>("logger",
                                                        sinks.begin(),
                                                        sinks.end(),
                                                        spdlog::thread_pool(),
                                                        spdlog::async_overflow_policy::block);
    // The separate sub loggers will handle trace levels
    logger->set_level(spdlog::level::trace);
    spdlog::register_logger(logger);
*/

    // Filter
    if (m_j_config.contains("filter") && m_j_config["filter"].is_object()) {

      json j = m_j_config["filter"];

      // IN filter
      if (j.contains("in-filter")) {
        try {
          std::string str = j["in-filter"].get<std::string>();
          vscp_readFilterFromString(&m_filterIn, str.c_str());
        }
        catch (const std::exception &ex) {
          spdlog::error(" Failed to read 'in-filter' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error(" Failed to read 'in-filter' due to unknown error.");
        }
      }
      else {
        spdlog::debug(" Failed to read LOGGING 'in-filter' Defaults will be used.");
      }

      // IN mask
      if (j.contains("in-mask")) {
        try {
          std::string str = j["in-mask"].get<std::string>();
          vscp_readMaskFromString(&m_filterIn, str.c_str());
        }
        catch (const std::exception &ex) {
          spdlog::error(" Failed to read 'in-mask' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error(" Failed to read 'in-mask' due to unknown error.");
        }
      }
      else {
        spdlog::debug(" Failed to read 'in-mask' Defaults will be used.");
      }

      // OUT filter
      if (j.contains("out-filter")) {
        try {
          std::string str = j["in-filter"].get<std::string>();
          vscp_readFilterFromString(&m_filterOut, str.c_str());
        }
        catch (const std::exception &ex) {
          spdlog::error(" Failed to read 'out-filter' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error(" Failed to read 'out-filter' due to unknown error.");
        }
      }
      else {
        spdlog::debug(" Failed to read 'out-filter' Defaults will be used.");
      }

      // OUT mask
      if (j.contains("out-mask")) {
        try {
          std::string str = j["out-mask"].get<std::string>();
          vscp_readMaskFromString(&m_filterOut, str.c_str());
        }
        catch (const std::exception &ex) {
          spdlog::error(" Failed to read 'out-mask' Error='{}'", ex.what());
        }
        catch (...) {
          spdlog::error(" Failed to read 'out-mask' due to unknown error.");
        }
      }
      else {
        spdlog::debug(" Failed to read 'out-mask' Defaults will be used.");
      }
    }

  }
  catch (const std::exception& ex) {
    spdlog::error("json socketcan init: Failed to parse json: {}", ex.what());
    return false;
  }

  return true;
}

  ///////////////////////////////////////////////////////////////////////////////
  // connect
  //

  int vscpClientSocketCan::connect(void)
  {
    int rv = VSCP_ERROR_SUCCESS;

/*
    // open the socket 
    if ( (m_socket = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0 )  {
        return CANAL_ERROR_SOCKET_CREATE;
    }

    int mtu, enable_canfd = 1;
    struct sockaddr_can addr;
    struct ifreq ifr;

    strncpy(ifr.ifr_name, m_interface.c_str(), IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    ifr.ifr_ifindex = if_nametoindex(ifr.ifr_name);
    if (!ifr.ifr_ifindex) {
        spdlog::error("Cant get socketcan index from {0}", m_interface);
        return VSCP_ERROR_ERROR;
    }

    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (CANFD_MTU == m_mode) {
        // check if the frame fits into the CAN netdevice 
        if (ioctl(m_socket, SIOCGIFMTU, &ifr) < 0) {
            spdlog::error("FD MTU does not fit for {0}", m_interface);
            return VSCP_TYPE_ERROR_FIFO_SIZE;
        }

        mtu = ifr.ifr_mtu;

        if (mtu != CANFD_MTU) {
            spdlog::error("CAN FD mode is not supported for {0}", m_interface);
            return VSCP_ERROR_NOT_SUPPORTED;
        }

        // interface is ok - try to switch the socket into CAN FD mode
        if (setsockopt(m_socket, 
                            SOL_CAN_RAW, 
                            CAN_RAW_FD_FRAMES,
                            &enable_canfd, 
                            sizeof(enable_canfd))) 
        {
            spdlog::error("Failed to switch socket to FD mode {0}", m_interface);
            return VSCP_ERROR_NOT_SUPPORTED;
        }

    }
*/
    //const int timestamping_flags = (SOF_TIMESTAMPING_SOFTWARE | \
    //    SOF_TIMESTAMPING_RX_SOFTWARE | \
    //    SOF_TIMESTAMPING_RAW_HARDWARE);

    //if (setsockopt(m_socket, SOL_SOCKET, SO_TIMESTAMPING,
    //    &timestamping_flags, sizeof(timestamping_flags)) < 0) {
    //    perror("setsockopt SO_TIMESTAMPING is not supported by your Linux kernel");
    //}

    // disable default receive filter on this RAW socket 
    // This is obsolete as we do not read from the socket at all, but for 
    // this reason we can remove the receive list in the Kernel to save a 
    // little (really a very little!) CPU usage.                          
    //setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0);

/*
    struct timeval tv;
    tv.tv_sec = 0;  
    tv.tv_usec = getResponseTimeout() * 1000;  // Not init'ing this can cause strange errors
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv,sizeof(struct timeval));

    if (bind(m_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        return CANAL_ERROR_SOCKET_BIND;
    }
*/
    // start the workerthread
    if (pthread_create(&m_threadWork, NULL, workerThread, this)) {
        spdlog::critical("Failed to start workerthread");
        return false;
    }

    return rv;
}

///////////////////////////////////////////////////////////////////////////////
// disconnect
//

int vscpClientSocketCan::disconnect(void)
{
    // Do nothing if already terminated
    if (!m_bRun) {
      return VSCP_ERROR_SUCCESS;
    }

    m_bRun = false; // terminate the thread
    // Wait for workerthread to to terminate
    pthread_join(m_threadWork, NULL);
    
    //::close(m_socket);
    m_bConnected = false;
    return CANAL_ERROR_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// isConnected
//

bool vscpClientSocketCan::isConnected(void)
{
    return m_bConnected;
}

///////////////////////////////////////////////////////////////////////////////
// send
//

int vscpClientSocketCan::send(vscpEvent &ev)
{
    canalMsg canalMsg;
    if ( !vscp_convertEventToCanal(&canalMsg, &ev ) ) {
        return VSCP_ERROR_PARAMETER;    
    }

    struct canfd_frame frame;
    memset(&frame, 0, sizeof(frame)); // init CAN FD frame, e.g. LEN = 0 
    
    //convert CanFrame to canfd_frame
    frame.can_id = canalMsg.id;
    frame.len = canalMsg.sizeData;
    //frame.flags = canalMsg.flags;
    memcpy(frame.data, canalMsg.data, canalMsg.sizeData);

    int socket_mode = 0;
    if (m_flags & FLAG_FD_MODE) {
        // ensure discrete CAN FD length values 0..8, 12, 16, 20, 24, 32, 64 
        frame.len = canal_dlc2len(canal_tbllen2dlc[frame.len]);
        socket_mode = 1;
    }
    /* send frame */
    //if (::write(m_socket, &frame, int(socket_mode)) != int(socket_mode)) {
    //    return VSCP_ERROR_WRITE_ERROR;
    //}

    return VSCP_ERROR_WRITE_ERROR;
}

///////////////////////////////////////////////////////////////////////////////
// send
//

int vscpClientSocketCan::send(vscpEventEx &ex)
{
    canalMsg canalMsg;
    if ( !vscp_convertEventExToCanal(&canalMsg, &ex ) ) {
        return VSCP_ERROR_PARAMETER;    
    }

    return 0; //m_canalif.CanalSend(&canalMsg);
}

///////////////////////////////////////////////////////////////////////////////
// receive
//

int vscpClientSocketCan::receive(vscpEvent &ev)
{
    int rv;
    canalMsg canalMsg;
    uint8_t guid[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    
    //if ( CANAL_ERROR_SUCCESS != (rv = m_canalif.CanalReceive(&canalMsg) ) ) {
    //    return rv;
    //}

    return 0; //vscp_convertCanalToEvent(&ev, &canalMsg, guid);
}

///////////////////////////////////////////////////////////////////////////////
// receive
//

int vscpClientSocketCan::receive(vscpEventEx &ex)
{
    int rv;
    canalMsg canalMsg;
    uint8_t guid[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    
    //if ( CANAL_ERROR_SUCCESS != (rv = m_canalif.CanalReceive(&canalMsg) ) ) {
    //    return rv;
    //}

    return vscp_convertCanalToEventEx(&ex,
                                        &canalMsg,
                                        guid);
}

///////////////////////////////////////////////////////////////////////////////
// setfilter
//

int vscpClientSocketCan::setfilter(vscpEventFilter &filter)
{
    int rv;

    uint32_t _filter = ((unsigned long)filter.filter_priority << 26) |
                    ((unsigned long)filter.filter_class << 16) |
                    ((unsigned long)filter.filter_type << 8) | filter.filter_GUID[0];
    //if ( CANAL_ERROR_SUCCESS == (rv = m_canalif.CanalSetFilter(_filter))) {
    //    return rv;
    //}

    uint32_t _mask = ((unsigned long)filter.mask_priority << 26) |
                    ((unsigned long)filter.mask_class << 16) |
                    ((unsigned long)filter.mask_type << 8) | filter.mask_GUID[0];
    return 0; //m_canalif.CanalSetMask(_mask);
}

///////////////////////////////////////////////////////////////////////////////
// getcount
//

int vscpClientSocketCan::getcount(uint16_t *pcount)
{
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// clear
//

int vscpClientSocketCan::clear()
{
    return VSCP_ERROR_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// getversion
//

int vscpClientSocketCan::getversion(uint8_t *pmajor,
                                    uint8_t *pminor,
                                    uint8_t *prelease,
                                    uint8_t *pbuild)
{
    //uint32_t ver = m_canalif.CanalGetDllVersion();

    return VSCP_ERROR_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// getinterfaces
//

int vscpClientSocketCan::getinterfaces(std::deque<std::string> &iflist)
{
    // No interfaces available
    return VSCP_ERROR_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// getwcyd
//

int vscpClientSocketCan::getwcyd(uint64_t &wcyd)
{
    wcyd = VSCP_SERVER_CAPABILITY_NONE;   // No capabilities
    return VSCP_ERROR_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////////
// setConnectionTimeout
//

void vscpClientSocketCan::setConnectionTimeout(uint32_t timeout)
{
    ;
}

//////////////////////////////////////////////////////////////////////////////
// getConnectionTimeout
//

uint32_t vscpClientSocketCan::getConnectionTimeout(void)
{
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// setResponseTimeout
//

void vscpClientSocketCan::setResponseTimeout(uint32_t timeout)
{
    ;
}

//////////////////////////////////////////////////////////////////////////////
// getResponseTimeout
//

uint32_t vscpClientSocketCan::getResponseTimeout(void)
{
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// sendToCallbacks
//

void vscpClientSocketCan::sendToCallbacks(vscpEvent *pev)
{
    if (nullptr != m_evcallback) {
        m_evcallback(pev, m_callbackObject);
    }

    if (nullptr != m_excallback) {
        vscpEventEx ex;
        vscp_convertEventToEventEx(&ex, pev);
        m_excallback(&ex, m_callbackObject);
    }
}

///////////////////////////////////////////////////////////////////////////////
// setCallback
//

int vscpClientSocketCan::setCallback(LPFNDLL_EV_CALLBACK m_evcallback)
{
    // Can not be called when connected
    if ( m_bConnected ) return VSCP_ERROR_ERROR;
    m_evcallback = m_evcallback;
    return VSCP_ERROR_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// setCallback
//

int vscpClientSocketCan::setCallback(LPFNDLL_EX_CALLBACK m_excallback)
{
    // Can not be called when connected
    if ( m_bConnected ) return VSCP_ERROR_ERROR;
    m_excallback = m_excallback;
    return VSCP_ERROR_SUCCESS;
}


//////////////////////////////////////////////////////////////////////
//                Workerthread - CSocketcanWorkerTread
//////////////////////////////////////////////////////////////////////

void *
workerThread(void *pData)
{
  int sock;
  int mtu, enable_canfd = 1;
  fd_set rdfs;
  struct timeval tv;
  struct sockaddr_can addr;
  struct ifreq ifr;
  struct cmsghdr *cmsg;
  struct canfd_frame frame;
  char ctrlmsg[CMSG_SPACE(sizeof(struct timeval)) + CMSG_SPACE(sizeof(__u32))];
  const int canfd_on = 1;

  vscpClientSocketCan *pObj = (vscpClientSocketCan *)pData;
  if (NULL == pObj) {
    spdlog::error("No object data object supplied for worker thread");
    return NULL;
  }

  while (pObj->m_bRun) {

    sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sock < 0) {

      if (ENETDOWN == errno) {
        sleep(1);
        continue;
      }

      spdlog::error("wrkthread socketcan client: Error while opening socket. Terminating!");
      break;
    }

    strncpy(ifr.ifr_name, pObj->m_interface.c_str(), IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    ifr.ifr_ifindex = if_nametoindex(ifr.ifr_name);
    if (!ifr.ifr_ifindex) {
        spdlog::error("Cant get socketcan index from {0}", pObj->m_interface);
        return NULL;
    }
    //ioctl(sock, SIOCGIFINDEX, &ifr);

    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (pObj->m_bDebug) {
      spdlog::debug("using interface name '{}'.", ifr.ifr_name);
    }

    // try to switch the socket into CAN FD mode
    //setsockopt(sock, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof(canfd_on));

    if (CANFD_MTU == pObj->m_mode) {

      // check if the frame fits into the CAN netdevice
      if (ioctl(sock, SIOCGIFMTU, &ifr) < 0) {
          spdlog::error("FD MTU does not fit for {0}", pObj->m_interface);
          //return VSCP_TYPE_ERROR_FIFO_SIZE;
          return NULL;
      }

      mtu = ifr.ifr_mtu;

      if (mtu != CANFD_MTU) {
          spdlog::error("CAN FD mode is not supported for {0}", pObj->m_interface);
          //return VSCP_ERROR_NOT_SUPPORTED;
          return NULL;
      }

      // interface is ok - try to switch the socket into CAN FD mode
      if (setsockopt(sock,
                          SOL_CAN_RAW,
                          CAN_RAW_FD_FRAMES,
                          &enable_canfd,
                          sizeof(enable_canfd))) {
          spdlog::error("Failed to switch socket to FD mode {0}", pObj->m_interface);
          //return VSCP_ERROR_NOT_SUPPORTED;
          return NULL;
      }

    }

    struct timeval tv;
    tv.tv_sec = 0;  
    tv.tv_usec = pObj->getResponseTimeout() * 1000;  // Not init'ing this can cause strange errors
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv,sizeof(struct timeval));

    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
      spdlog::error("wrkthread socketcan client: Error in socket bind. Terminating!");
      close(sock);
      sleep(2);
      //continue;
      return NULL;
    }

    bool bInnerLoop = true;
    while (pObj->m_bRun && bInnerLoop) {

      FD_ZERO(&rdfs);
      FD_SET(sock, &rdfs);

      tv.tv_sec  = 0;
      tv.tv_usec = 5000; // 5ms timeout

      int ret;
      if ((ret = select(sock + 1, &rdfs, NULL, NULL, &tv)) < 0) {
        // Error
        if (ENETDOWN == errno) {
          // We try to get contact with the net
          // again if it goes down
          bInnerLoop = false;
        }
        else {
          pObj->m_bRun = false;
        }
        continue;
      }

      if (ret) {

        // There is data to read

        ret = read(sock, &frame, sizeof(struct can_frame));
        if (ret < 0) {
          if (ENETDOWN == errno) {
            // We try to get contact with the net
            // again if it goes down
            bInnerLoop = false;
            sleep(2);
          }
          else {
            pObj->m_bRun = true;
          }
          continue;
        }

        // Must be Extended
        if (!(frame.can_id & CAN_EFF_FLAG)) {
          continue;
        }

        // Mask of control bits
        frame.can_id &= CAN_EFF_MASK;

        vscpEvent *pEvent = new vscpEvent();
        if (nullptr != pEvent) {

          // This can lead to level I frames having to
          // much data. Later code will handel this case.
          pEvent->pdata = new uint8_t[frame.len];
          if (nullptr == pEvent->pdata) {
            delete pEvent;
            continue;
          }

          // GUID will be set to GUID of interface
          // by driver interface with LSB set to nickname
          //memcpy(pEvent->GUID, pObj->m_guid.getGUID(), 16);
          pEvent->GUID[VSCP_GUID_LSB] = frame.can_id & 0xff;

          // Set VSCP class
          pEvent->vscp_class = vscp_getVscpClassFromCANALid(frame.can_id);

          // Set VSCP type
          pEvent->vscp_type = vscp_getVscpTypeFromCANALid(frame.can_id);

          // Copy data if any
          pEvent->sizeData = frame.len;
          if (frame.len) {              
            memcpy(pEvent->pdata, frame.data, frame.len);
          }

          if (vscp_doLevel2Filter(pEvent, &pObj->m_filterIn)) {

            if (nullptr != pObj->m_evcallback) {
                pObj->m_evcallback(pEvent, pObj->m_callbackObject);
            }

            if (nullptr != pObj->m_excallback) {
                vscpEventEx ex;
                if (vscp_convertEventToEventEx(&ex, pEvent) ) {
                    pObj->m_excallback(&ex, pObj->m_callbackObject);
                }
            }

            if ((nullptr != pObj->m_evcallback) && (nullptr != pObj->m_excallback)) {
                pthread_mutex_lock(&pObj->m_mutexReceiveQueue);
                pObj->m_receiveList.push_back(pEvent);
                sem_post(&pObj->m_semReceiveQueue);
                pthread_mutex_unlock(&pObj->m_mutexReceiveQueue);
            }
          }
          else {
            vscp_deleteEvent(pEvent);
          }
        }
      }
      else {

        // Check if there is event(s) to send
        if (pObj->m_sendList.size()) {

          // Yes there are data to send
          // So send it out on the CAN bus

          pthread_mutex_lock(&pObj->m_mutexSendQueue);
          vscpEvent *pEvent = pObj->m_sendList.front();
          pObj->m_sendList.pop_front();
          pthread_mutex_unlock(&pObj->m_mutexSendQueue);

          if (NULL == pEvent) {
            continue;
          }

          // Class must be a Level I class or a Level II
          // mirror class
          if (pEvent->vscp_class < 512) {
            frame.can_id = vscp_getCANALidFromEvent(pEvent);
            frame.can_id |= CAN_EFF_FLAG; // Always extended
            if (0 != pEvent->sizeData) {
              frame.len = (pEvent->sizeData > 8 ? 8 : pEvent->sizeData);
              memcpy(frame.data, pEvent->pdata, frame.len);
            }
          }
          else if (pEvent->vscp_class < 1024) {
            pEvent->vscp_class -= 512;
            frame.can_id = vscp_getCANALidFromEvent(pEvent);
            frame.can_id |= CAN_EFF_FLAG; // Always extended
            if (0 != pEvent->sizeData) {
              frame.len = ((pEvent->sizeData - 16) > 8 ? 8 : pEvent->sizeData - 16);
              memcpy(frame.data, pEvent->pdata + 16, frame.len);
            }
          }

          // Remove the event
          pthread_mutex_lock(&pObj->m_mutexSendQueue);
          vscp_deleteEvent(pEvent);
          pthread_mutex_unlock(&pObj->m_mutexSendQueue);

          // Write the data
          int nbytes = write(sock, &frame, sizeof(struct can_frame));

        } // event to send

      } // No data to read

    } // Inner loop

    // Close the socket
    close(sock);

  } // Outer loop

  return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// Workerthread
//
// This thread call the appropriate callback when events are received
//


// static void *workerThread(void *pObj)
// {
//     uint8_t guid[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
//     vscpClientSocketCan *pClient = (vscpClientSocketCan *)pObj;
//     VscpCanalDeviceIf *pif = (VscpCanalDeviceIf *)&(pClient->m_canalif);

//     if (NULL == pif) return NULL;

//     while (pClient->m_bRun) {

//         //pthread_mutex_lock(&pif->m_mutexif);
        
//         // Check if there are events to fetch
//         int cnt;
//         if ((cnt = pClient->m_canalif.CanalDataAvailable())) {

//             while (cnt) {
//                 canalMsg msg;
//                 if ( CANAL_ERROR_SUCCESS == pClient->m_canalif.CanalReceive(&msg) ) {
//                     if ( NULL != pClient->m_evcallback ) {
//                         vscpEvent ev;
//                         if (vscp_convertCanalToEvent(&ev,
//                                                         &msg,
//                                                         guid) ) {
//                             pClient->m_evcallback(&ev);
//                         }
//                     }
//                     if ( NULL != pClient->m_excallback ) {
//                         vscpEventEx ex;
//                         if (vscp_convertCanalToEventEx(&ex,
//                                                         &msg,
//                                                         guid) ) {
//                             pClient->m_excallback(&ex);
//                         }
//                     }
//                 }
//                 cnt--;
//             }

//         }

//         //pthread_mutex_unlock(&pif->m_mutexif);
//         usleep(200);
//     }

//     return NULL;
// }

// void *workerThread(void *pData)
// {
//     int sock;
//     char devname[IFNAMSIZ + 1];
//     fd_set rdfs;
//     struct timeval tv;
//     struct sockaddr_can addr;
//     struct ifreq ifr;
//     struct cmsghdr *cmsg;
//     struct canfd_frame frame;
//     char
//       ctrlmsg[CMSG_SPACE(sizeof(struct timeval)) + CMSG_SPACE(sizeof(__u32))];
//     const int canfd_on = 1;

//     vscpClientSocketCan *pObj = (vscpClientSocketCan *)pData;
//     if (NULL == pObj) {
//         //syslog(LOG_ERR, "No object data supplied for worker thread");
//         return NULL;
//     }

//     strncpy(devname, pObj->m_interface.c_str(), sizeof(devname) - 1);

//     while (pObj->m_bRun) {

//         sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
//         if (sock < 0) {

//             if (ENETDOWN == errno) {
//                 sleep(1);
//                 continue;   // We try again
//             }

//             if (pObj->m_flags & pObj->FLAG_ENABLE_DEBUG) {
//                 syslog(LOG_ERR,
//                        "%s",
//                     (const char *)"ReadSocketCanTread: Error while "
//                                      "opening socket. Terminating!");
//             }
//             break;
//         }

//         strcpy(ifr.ifr_name, devname);
//         ioctl(sock, SIOCGIFINDEX, &ifr);

//         addr.can_family  = AF_CAN;
//         addr.can_ifindex = ifr.ifr_ifindex;

// #ifdef DEBUG
//         printf("using interface name '%s'.\n", ifr.ifr_name);
// #endif

//         // try to switch the socket into CAN FD mode
//         setsockopt(
//           sock, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof(canfd_on));

//         if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
//             syslog(LOG_ERR,
//                    "%s",
//                    (const char *)"wrkthread socketcan client: Error in socket bind. "
//                                  "Terminating!");
//             close(sock);
//             sleep(2);
//             continue;
//         }

//         bool bInnerLoop = true;
//         while (!pObj->m_bRun && bInnerLoop) {

//             FD_ZERO(&rdfs);
//             FD_SET(sock, &rdfs);

//             tv.tv_sec  = 0;
//             tv.tv_usec = 5000; // 5ms timeout

//             int ret;
//             if ((ret = select(sock + 1, &rdfs, NULL, NULL, &tv)) < 0) {
//                 // Error
//                 if (ENETDOWN == errno) {
//                     // We try to get contact with the net
//                     // again if it goes down
//                     bInnerLoop = false;
//                 } else {
//                     pObj->m_bRun = false;
//                 }
//                 continue;
//             }

//             if (ret) {

//                 // There is data to read

//                 ret = read(sock, &frame, sizeof(struct can_frame));
//                 if (ret < 0) {
//                     if (ENETDOWN == errno) {
//                         // We try to get contact with the net
//                         // again if it goes down
//                         bInnerLoop = false;
//                         sleep(2);
//                     } else {
//                         pObj->m_bRun = false;
//                     }
//                     continue;
//                 }

//                 // Must be Extended
//                 if (!(frame.can_id & CAN_EFF_FLAG)) continue;

//                 // Mask of control bits
//                 frame.can_id &= CAN_EFF_MASK;

//                 vscpEvent *pEvent = new vscpEvent();
//                 if (NULL != pEvent) {

//                     pEvent->pdata = new uint8_t[frame.len];
//                     if (NULL == pEvent->pdata) {
//                         delete pEvent;
//                         continue;
//                     }

//                     // GUID will be set to GUID of interface
//                     // by driver interface with LSB set to nickname
//                     memset(pEvent->GUID, 0, 16);
//                     pEvent->GUID[VSCP_GUID_LSB] = frame.can_id & 0xff;

//                     // Set VSCP class
//                     pEvent->vscp_class = vscp_getVscpClassFromCANALid(frame.can_id);

//                     // Set VSCP type
//                     pEvent->vscp_type = vscp_getVscpTypeFromCANALid(frame.can_id);

//                     // Copy data if any
//                     pEvent->sizeData = frame.len;
//                     if (frame.len) {
//                         memcpy(pEvent->pdata, frame.data, frame.len);
//                     }

//                     // if (vscp_doLevel2Filter(pEvent, &pObj->m_vscpfilter)) {
//                     //     pthread_mutex_lock( &pObj->m_mutexReceiveQueue);
//                     //     pObj->m_receiveList.push_back(pEvent);
//                     //     sem_post(&pObj->m_semReceiveQueue);
//                     //     pthread_mutex_unlock( &pObj->m_mutexReceiveQueue);
//                     // } else {
//                     //     vscp_deleteVscpEvent(pEvent);
//                     // }
//                 }

//             } else {

//                 // Check if there is event(s) to send
//                 // if (pObj->m_sendList.size()) {

//                 //     // Yes there are data to send
//                 //     // So send it out on the CAN bus

//                 //     pthread_mutex_lock( &pObj->m_mutexSendQueue);
//                 //     vscpEvent *pEvent = pObj->m_sendList.front();
//                 //     pObj->m_sendList.pop_front();
//                 //     pthread_mutex_unlock( &pObj->m_mutexSendQueue);

//                 //     if (NULL == pEvent) continue;

//                 //     // Class must be a Level I class or a Level II
//                 //     // mirror class
//                 //     if (pEvent->vscp_class < 512) {
//                 //         frame.can_id = vscp_getCANALidFromEvent(pEvent);
//                 //         frame.can_id |= CAN_EFF_FLAG; // Always extended
//                 //         if (0 != pEvent->sizeData) {
//                 //             frame.len =
//                 //               (pEvent->sizeData > 8 ? 8 : pEvent->sizeData);
//                 //             memcpy(frame.data, pEvent->pdata, frame.len);
//                 //         }
//                 //     } else if (pEvent->vscp_class < 1024) {
//                 //         pEvent->vscp_class -= 512;
//                 //         frame.can_id = vscp_getCANALidFromEvent(pEvent);
//                 //         frame.can_id |= CAN_EFF_FLAG; // Always extended
//                 //         if (0 != pEvent->sizeData) {
//                 //             frame.len = ((pEvent->sizeData - 16) > 8
//                 //                            ? 8
//                 //                            : pEvent->sizeData - 16);
//                 //             memcpy(frame.data, pEvent->pdata + 16, frame.len);
//                 //         }
//                 //     }

//                 //     // Remove the event
//                 //     pthread_mutex_lock( &pObj->m_mutexSendQueue);
//                 //     vscp_deleteVSCPevent(pEvent);
//                 //     pthread_mutex_unlock( &pObj->m_mutexSendQueue);

//                 //     // Write the data
//                 //     int nbytes = write(sock, &frame, sizeof(struct can_frame));

//                 //} // event to send

//             } // No data to read

//         } // Inner loop

//         // Close the socket
//         close(sock);

//     } // Outer loop

//     return NULL;
// }
