// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#include <unistd.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "novadbplus/commands/release.h"
#include "novadbplus/commands/version.h"
#include "novadbplus/server/server_entry.h"
#include "novadbplus/server/server_params.h"
#include "novadbplus/utils/invariant.h"
#include "novadbplus/utils/portable.h"
#include "novadbplus/utils/time.h"

static void shutdown(int sigNum) {
  LOG(INFO) << "signal:" << sigNum << " caught, begin shutdown server";
  INVARIANT(novadbplus::getGlobalServer() != nullptr);
  novadbplus::getGlobalServer()->handleShutdownCmd();
}

static void waitForExit() {
  INVARIANT(novadbplus::getGlobalServer() != nullptr);
  novadbplus::getGlobalServer()->waitStopComplete();
}

static void setupSignals() {
#ifndef _WIN32
  struct sigaction ignore;
  memset(&ignore, 0, sizeof(ignore));
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);

  INVARIANT(sigaction(SIGHUP, &ignore, nullptr) == 0);
  INVARIANT(sigaction(SIGUSR2, &ignore, nullptr) == 0);
  INVARIANT(sigaction(SIGPIPE, &ignore, nullptr) == 0);

  struct sigaction exits;
  memset(&exits, 0, sizeof(exits));
  exits.sa_handler = shutdown;
  sigemptyset(&ignore.sa_mask);

  INVARIANT(sigaction(SIGTERM, &exits, nullptr) == 0);
  INVARIANT(sigaction(SIGINT, &exits, nullptr) == 0);
#endif  // !_WIN32
}

static void usage() {
  std::cout << "./novadbplus [configfile]" << std::endl;
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    usage();
    return 0;
  }
  if (strcmp(argv[1], "-v") == 0) {
    std::cout << "novadbplus v=" << getnovadbPlusVersion()
              << " sha=" << novadbPLUS_GIT_SHA1
              << " dirty=" << novadbPLUS_GIT_DIRTY
              << " build=" << novadbPLUS_BUILD_ID << std::endl;
    return 0;
  }

  std::srand((uint32_t)novadbplus::msSinceEpoch());

  novadbplus::gParams = std::make_shared<novadbplus::ServerParams>();
  auto params = novadbplus::gParams;
  auto s = params->parseFile(argv[1]);
  if (!s.ok()) {
    std::cout << "parse config failed:" << s.toString();
    // LOG(FATAL) << "parse config failed:" << s.toString();
    return -1;
  } else {
    std::cout << "start server with cfg:\n" << params->showAll() << std::endl;
    // LOG(INFO) << "start server with cfg:" << params->toString();
  }

  INVARIANT(sizeof(double) == 8);
#ifndef WITH_ASAN
#ifndef WITH_TSAN
#ifndef _WIN32
  if (params->daemon) {
    if (daemon(1 /*nochdir*/, 0 /*noclose*/) < 0) {
      // NOTE(deyukong): it should rarely fail.
      // but if code reaches here, cerr may have been redirected to
      // /dev/null and nothing printed.
      LOG(FATAL) << "daemonlize failed:" << errno;
    }
  }
#endif  // !_WIN32
#endif  // !WITH_TSAN
#endif  // !WITH_ASAN

  FLAGS_minloglevel = 0;
  if (params->logLevel == "debug" || params->logLevel == "verbose") {
    FLAGS_v = 1;
  } else {
    FLAGS_v = 0;
  }

  FLAGS_max_log_size = params->logSizeMb;
  if (params->logDir != "") {
    FLAGS_log_dir = params->logDir;
    std::cout << "glog dir:" << FLAGS_log_dir << std::endl;
    if (!novadbplus::filesystem::exists(FLAGS_log_dir)) {
      std::error_code ec;
      if (!novadbplus::filesystem::create_directories(FLAGS_log_dir, ec)) {
        LOG(WARNING) << " create log path failed: " << ec.message();
      }
    }
  }

  FLAGS_logbufsecs = 1;
  ::google::InitGoogleLogging("novadbplus");
#ifndef _WIN32
  ::google::InstallFailureSignalHandler();
  ::google::InstallFailureWriter([](const char* data, int size) {
    LOG(ERROR) << "Failure:" << std::string(data, size);
    google::FlushLogFiles(google::INFO);
    google::FlushLogFiles(google::WARNING);
    google::FlushLogFiles(google::ERROR);
    google::FlushLogFiles(google::FATAL);
  });
#endif

  LOG(INFO) << "startup pid:" << getpid();

  novadbplus::getGlobalServer() =
    std::make_shared<novadbplus::ServerEntry>(params);
  s = novadbplus::getGlobalServer()->startup(params);
  if (!s.ok()) {
    LOG(FATAL) << "server startup failed:" << s.toString();
  }
  setupSignals();

  // pid file
  std::ofstream pidfile(params->pidFile);
  pidfile << getpid();
  pidfile.close();

  waitForExit();
  LOG(INFO) << "server exits";

  remove(params->pidFile.c_str());
  return 0;
}
