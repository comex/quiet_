#pragma once
#include "decls.h"
#include "settings.h"
#include "containers.h"

BEGIN_LOCAL_DECLS

enum save_rw_state {
    SAVE_RW_IDLE = 0,
    SAVE_RW_READ_OPENING,
    SAVE_RW_READ_READING,
    SAVE_RW_READ_CLOSING,
    SAVE_RW_WRITE_OPENING,
    SAVE_RW_WRITE_WRITING,
    SAVE_RW_WRITE_CLOSING,
    SAVE_RW_WRITE_FLUSHING,
};

struct state {
    OSMutex mutex;

    int save_cur_slot;
    enum save_rw_state save_rw_state;
    struct fs_client fs_client;
    struct fs_cmd fs_cmd;
    int save_file_handle;
    char settings_txt_buf[1024] __attribute__((aligned(64)));
    size_t settings_txt_buf_len;
    bool settings_need_write;
    bool settings_loaded;
    int setting_vals[NUM_SETTINGS];

    bool initted_ui;
    struct splitter_heap heap;

    struct qmod_ui *ui;
    uint32_t ui_input_cur_capture; // XXX this sucks
    uint32_t ui_input_cur_held, ui_input_cur_trigger;

    struct graphics_state *graphics_state;

    struct rpl_info mariomaker_rpl_info;
};
struct state *get_state(void);

END_LOCAL_DECLS
