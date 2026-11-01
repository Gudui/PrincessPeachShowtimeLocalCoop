#pragma once

#include <lib/log/svc_logger.hpp>
#include "program/patches/sd_log.hpp"

#if PSHOW_SD_LOG_ENABLED
inline exl::log::LoggerMgr<exl::log::SvcLogger, pshowcoop::sdlog::SdLogger> Logging;
#else
inline exl::log::LoggerMgr<exl::log::SvcLogger> Logging;
#endif
