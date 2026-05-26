#pragma once

/*
 * Minimal libmpv C API declarations for runtime dynamic linking.
 * Only type definitions and function-pointer typedefs -- no library linkage.
 * Based on libmpv client.h (mpv 0.36+).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle */
typedef struct mpv_handle mpv_handle;

/* ===== Enumerations ===== */

typedef enum mpv_error {
    MPV_ERROR_SUCCESS           =  0,
    MPV_ERROR_EVENT_QUEUE_FULL  = -1,
    MPV_ERROR_NOMEM             = -2,
    MPV_ERROR_UNINITIALIZED     = -3,
    MPV_ERROR_INVALID_PARAMETER = -4,
    MPV_ERROR_OPTION_NOT_FOUND  = -5,
    MPV_ERROR_OPTION_FORMAT     = -6,
    MPV_ERROR_OPTION_ERROR      = -7,
    MPV_ERROR_PROPERTY_NOT_FOUND = -8,
    MPV_ERROR_PROPERTY_FORMAT   = -9,
    MPV_ERROR_PROPERTY_UNAVAILABLE = -10,
    MPV_ERROR_PROPERTY_ERROR    = -11,
    MPV_ERROR_COMMAND           = -12,
    MPV_ERROR_LOADING_FAILED    = -13,
    MPV_ERROR_AO_INIT_FAILED    = -14,
    MPV_ERROR_VO_INIT_FAILED    = -15,
    MPV_ERROR_NOTHING_TO_PLAY   = -16,
    MPV_ERROR_UNKNOWN_FORMAT    = -17,
    MPV_ERROR_UNSUPPORTED       = -18,
    MPV_ERROR_NOT_IMPLEMENTED   = -19,
    MPV_ERROR_GENERIC           = -20
} mpv_error;

typedef enum mpv_format {
    MPV_FORMAT_NONE       = 0,
    MPV_FORMAT_STRING     = 1,
    MPV_FORMAT_OSD_STRING = 2,
    MPV_FORMAT_FLAG       = 3,
    MPV_FORMAT_INT64      = 4,
    MPV_FORMAT_DOUBLE     = 5,
    MPV_FORMAT_NODE       = 6,
    MPV_FORMAT_NODE_ARRAY = 7,
    MPV_FORMAT_NODE_MAP   = 8,
    MPV_FORMAT_BYTE_ARRAY = 9
} mpv_format;

typedef enum mpv_event_id {
    MPV_EVENT_NONE              = 0,
    MPV_EVENT_SHUTDOWN          = 1,
    MPV_EVENT_LOG_MESSAGE       = 2,
    MPV_EVENT_GET_PROPERTY_REPLY = 3,
    MPV_EVENT_SET_PROPERTY_REPLY = 4,
    MPV_EVENT_COMMAND_REPLY     = 5,
    MPV_EVENT_START_FILE        = 6,
    MPV_EVENT_END_FILE          = 7,
    MPV_EVENT_FILE_LOADED       = 8,
    /* 9-15 deprecated/unused */
    MPV_EVENT_CLIENT_MESSAGE    = 16,
    MPV_EVENT_VIDEO_RECONFIG    = 17,
    MPV_EVENT_AUDIO_RECONFIG    = 18,
    MPV_EVENT_SEEK              = 20,
    MPV_EVENT_PLAYBACK_RESTART  = 21,
    MPV_EVENT_PROPERTY_CHANGE   = 22,
    MPV_EVENT_QUEUE_OVERFLOW    = 24,
    MPV_EVENT_HOOK              = 25
} mpv_event_id;

typedef enum mpv_end_file_reason {
    MPV_END_FILE_REASON_EOF      = 0,
    MPV_END_FILE_REASON_STOP     = 2,
    MPV_END_FILE_REASON_QUIT     = 3,
    MPV_END_FILE_REASON_ERROR    = 4,
    MPV_END_FILE_REASON_REDIRECT = 5
} mpv_end_file_reason;

/* ===== Structures ===== */

typedef struct mpv_node mpv_node;
typedef struct mpv_node_list mpv_node_list;
typedef struct mpv_byte_array mpv_byte_array;

struct mpv_byte_array {
    void*  data;
    size_t size;
};

struct mpv_node_list {
    int        num;
    mpv_node*  values;
    char**     keys;   /* NULL for arrays, valid for maps */
};

struct mpv_node {
    union {
        char*             string;   /* MPV_FORMAT_STRING */
        int               flag;     /* MPV_FORMAT_FLAG */
        __int64           int64;    /* MPV_FORMAT_INT64 */
        double            double_;  /* MPV_FORMAT_DOUBLE */
        mpv_node_list*    list;     /* MPV_FORMAT_NODE_ARRAY / NODE_MAP */
        mpv_byte_array*   ba;       /* MPV_FORMAT_BYTE_ARRAY */
    } u;
    mpv_format format;
};

typedef struct mpv_event_property {
    const char* name;
    mpv_format  format;
    void*       data;
} mpv_event_property;

typedef struct mpv_event_end_file {
    int reason;     /* mpv_end_file_reason */
    int error;      /* mpv_error, valid when reason == ERROR */
} mpv_event_end_file;

typedef struct mpv_event {
    mpv_event_id    event_id;
    int             error;
    unsigned __int64 reply_userdata;
    void*           data;
} mpv_event;

/* ===== Function pointer typedefs for runtime linking ===== */

typedef mpv_handle*    (*pfn_mpv_create)(void);
typedef int            (*pfn_mpv_initialize)(mpv_handle* ctx);
typedef void           (*pfn_mpv_destroy)(mpv_handle* ctx);
typedef void           (*pfn_mpv_terminate_destroy)(mpv_handle* ctx);

typedef int            (*pfn_mpv_set_option)(mpv_handle* ctx, const char* name,
                                             mpv_format format, void* data);
typedef int            (*pfn_mpv_set_option_string)(mpv_handle* ctx,
                                                     const char* name,
                                                     const char* data);

typedef int            (*pfn_mpv_command)(mpv_handle* ctx, const char** args);
typedef int            (*pfn_mpv_command_async)(mpv_handle* ctx,
                                                unsigned __int64 reply_userdata,
                                                const char** args);
typedef int            (*pfn_mpv_command_string)(mpv_handle* ctx, const char* args);

typedef int            (*pfn_mpv_set_property)(mpv_handle* ctx, const char* name,
                                               mpv_format format, void* data);
typedef int            (*pfn_mpv_set_property_string)(mpv_handle* ctx,
                                                       const char* name,
                                                       const char* data);
typedef int            (*pfn_mpv_get_property)(mpv_handle* ctx, const char* name,
                                               mpv_format format, void* data);
typedef char*          (*pfn_mpv_get_property_string)(mpv_handle* ctx,
                                                       const char* name);

typedef int            (*pfn_mpv_observe_property)(mpv_handle* ctx,
                                                    unsigned __int64 reply_userdata,
                                                    const char* name,
                                                    mpv_format format);

typedef mpv_event*     (*pfn_mpv_wait_event)(mpv_handle* ctx, double timeout);

typedef void           (*pfn_mpv_free)(void* data);
typedef void           (*pfn_mpv_free_node_contents)(mpv_node* node);
typedef const char*    (*pfn_mpv_error_string)(int error);

#ifdef __cplusplus
}
#endif
