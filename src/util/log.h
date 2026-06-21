#ifndef LOG_H
#define LOG_H

#include <stdio.h>

enum LogLevel {
    LOG_LEVEL_TRACE    = 0,
    LOG_LEVEL_DEBUG    = 1,
    LOG_LEVEL_INFO     = 2,
    LOG_LEVEL_WARN     = 3,
    LOG_LEVEL_ERROR    = 4,
    LOG_LEVEL_CRITICAL = 5,
};

#ifndef CURRENT_LOG_LEVEL
#define CURRENT_LOG_LEVEL LOG_LEVEL_TRACE
#endif

extern FILE* g_log_file;

#define LOG_TRACE(fmt, ...) do { if (CURRENT_LOG_LEVEL <= LOG_LEVEL_TRACE) { fprintf(stderr, "[TRACE] " __FILE__ ":%d " fmt "\n", __LINE__, ##__VA_ARGS__); if (g_log_file) { fprintf(g_log_file, "[TRACE] " __FILE__ ":%d " fmt "\n", __LINE__, ##__VA_ARGS__); fflush(g_log_file); } } } while(0)
#define LOG_DEBUG(fmt, ...) do { if (CURRENT_LOG_LEVEL <= LOG_LEVEL_DEBUG) { fprintf(stderr, "[DEBUG] " __FILE__ ":%d " fmt "\n", __LINE__, ##__VA_ARGS__); if (g_log_file) { fprintf(g_log_file, "[DEBUG] " __FILE__ ":%d " fmt "\n", __LINE__, ##__VA_ARGS__); fflush(g_log_file); } } } while(0)
#define LOG_INFO(fmt, ...)  do { if (CURRENT_LOG_LEVEL <= LOG_LEVEL_INFO)  { fprintf(stderr, "[INFO]  " fmt "\n", ##__VA_ARGS__); fflush(stderr); if (g_log_file) { fprintf(g_log_file, "[INFO]  " fmt "\n", ##__VA_ARGS__); fflush(g_log_file); } } } while(0)
#define LOG_WARN(fmt, ...)  do { if (CURRENT_LOG_LEVEL <= LOG_LEVEL_WARN) { fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__); if (g_log_file) { fprintf(g_log_file, "[WARN]  " fmt "\n", ##__VA_ARGS__); fflush(g_log_file); } } } while(0)
#define LOG_ERROR(fmt, ...) do { if (CURRENT_LOG_LEVEL <= LOG_LEVEL_ERROR) { fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); if (g_log_file) { fprintf(g_log_file, "[ERROR] " fmt "\n", ##__VA_ARGS__); fflush(g_log_file); } } } while(0)
#define LOG_CRITICAL(fmt, ...) do { if (CURRENT_LOG_LEVEL <= LOG_LEVEL_CRITICAL) { fprintf(stderr, "[CRIT] " __FILE__ ":%d " fmt "\n", __LINE__, ##__VA_ARGS__); if (g_log_file) { fprintf(g_log_file, "[CRIT] " __FILE__ ":%d " fmt "\n", __LINE__, ##__VA_ARGS__); fflush(g_log_file); } } } while(0)

#endif
