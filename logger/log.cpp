/*
 * Descent 3
 * Copyright (C) 2024 Descent Developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <filesystem>
#include <mutex>
#include <plog/Log.h>
#include <plog/Appenders/ColorConsoleAppender.h>
#include <plog/Formatters/MessageOnlyFormatter.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Initializers/RollingFileInitializer.h>

#ifdef WIN32
#include <plog/Appenders/DebugOutputAppender.h>
#include <cstdio>
#include <windows.h>
#include "debug.h"
#endif

#include "log.h"

namespace {
// The console's lines are already formatted text: the file keeps them as stdout shows them, so a log reader takes the
// file and a stdout capture alike.
class FileFormatter {
public:
  static plog::util::nstring header() { return plog::TxtFormatter::header(); }
  static plog::util::nstring format(const plog::Record &record) {
    if (record.getInstanceId() == CONSOLE_LOG_ID)
      return plog::MessageOnlyFormatter::format(record);
    return plog::TxtFormatter::format(record);
  }
};

// Descent3.log takes plog's records and the console's text (the CONSOLE_LOG_ID instance, one record per con_Printf).
// The console prints some lines in pieces, and the level-load progress has no newline and erases itself with
// backspaces. Its text is held until a newline, or until a plog record comes, which stdout prints after the same
// partial line. The file keeps stdout's order, one line per record.
class FileAppender : public plog::IAppender {
public:
  explicit FileAppender(const char *file_name) : m_file(file_name) {}
  ~FileAppender() override {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_console_line.empty())
      writeConsoleLine();
  }

  void write(const plog::Record &record) override {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (record.getInstanceId() != CONSOLE_LOG_ID) {
      if (!m_console_line.empty())
        writeConsoleLine();
      m_file.write(record);
      return;
    }
    for (const plog::util::nchar *p = record.getMessage(); *p; p++) {
      if (*p == PLOG_NSTR('\n')) {
        writeConsoleLine();
      } else if (*p == PLOG_NSTR('\b')) {
        if (!m_console_line.empty())
          m_console_line.pop_back();
      } else if (*p != PLOG_NSTR('\r')) {
        m_console_line += *p;
      }
    }
  }

private:
  void writeConsoleLine() {
    plog::Record line(plog::info, "", 0, "", nullptr, CONSOLE_LOG_ID);
    line << m_console_line;
    m_file.write(line);
    m_console_line.clear();
  }

  std::mutex m_mutex;
  plog::util::nstring m_console_line;
  plog::RollingFileAppender<FileFormatter> m_file;
};
} // namespace

void InitLog(plog::Severity log_level, bool enable_filelog, bool enable_win_console) {
  std::filesystem::path log_file = "Descent3.log";
  static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
  static FileAppender fileAppender((const char *)log_file.u8string().c_str());

#ifdef WIN32
  static plog::DebugOutputAppender<plog::TxtFormatter> debugAppender;

  if (enable_win_console) {
    // Open console window
    AllocConsole();
    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
  }
#endif

  plog::init(log_level, &consoleAppender);
  if (enable_filelog) {
    if (std::filesystem::is_regular_file(log_file)) {
      // Delete old log
      std::error_code ec;
      std::filesystem::remove(log_file, ec);
    }
    plog::get()->addAppender(&fileAppender);
    // The console's own instance writes only here, so its lines do not print twice on stdout
    plog::init<CONSOLE_LOG_ID>(plog::verbose, &fileAppender);
  }
#ifdef WIN32
  if (IsDebuggerPresent()) {
    plog::get()->addAppender(&debugAppender);
  }
#endif
}
