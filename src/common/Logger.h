#pragma once
#include <string>
#include <iostream>
#include <sstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <windows.h>

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

class Logger {
public:
    static void Log(LogLevel level, const std::string& tag, const std::string& msg) {
        std::lock_guard<std::mutex> lock(GetMutex());

        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm_now;
        localtime_s(&tm_now, &now_c);

        const char* levelStr = "INFO";
        switch (level) {
            case LogLevel::Debug: levelStr = "DEBUG"; break;
            case LogLevel::Info:  levelStr = "INFO";  break;
            case LogLevel::Warn:  levelStr = "WARN";  break;
            case LogLevel::Error: levelStr = "ERROR"; break;
        }

        std::ostringstream ss;
        ss << "[" << std::put_time(&tm_now, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count()
           << "][" << levelStr << "][" << tag << "] " << msg << "\n";

        std::string str = ss.str();
        OutputDebugStringA(str.c_str());
    }

    static void D(const std::string& tag, const std::string& msg) { Log(LogLevel::Debug, tag, msg); }
    static void I(const std::string& tag, const std::string& msg) { Log(LogLevel::Info, tag, msg); }
    static void W(const std::string& tag, const std::string& msg) { Log(LogLevel::Warn, tag, msg); }
    static void E(const std::string& tag, const std::string& msg) { Log(LogLevel::Error, tag, msg); }

private:
    static std::mutex& GetMutex() {
        static std::mutex s_mutex;
        return s_mutex;
    }
};
