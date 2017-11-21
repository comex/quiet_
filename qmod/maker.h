#pragma once
#include "types.h"
#include "misc.h"
#include "dynstruct.h"

union mm_spawn_rect {
    field_at<0x0, int> x;
    field_at<0x4, int> y;
    field_at<0x8, int> width;
    field_at<0xc, int> height;
    field_at<0x10, uint16_t> bits;
};

union mm_list_link {
    field_at<0, mm_list_link *> prev;
    field_at<4, mm_list_link *> next;
    static constexpr size_t SIZE = 8;
};

union mm_list_head {
    field_at<0x0, mm_list_link> link;
    field_at<0x8, uint32_t> count;
    field_at<0xc, uint32_t> link_offset;
    static constexpr size_t SIZE = 0x10;
};

union mm_entity_spec;

union mm_entity_record {
    field_at<4, void *> vtable;
    field_at<8, void *(*)(void *self, mm_entity_spec *)> ctor;
    field_at<0x10, int> cls;
    field_at<0x14, mm_spawn_rect *> spawn_rect;
};

union vt_mm_entity;

union mm_allocator;

union mm_model;

union mm_entity {
    field_at<0x00, mm_allocator *> allocator;
    field_at<0x04, mm_model *> model;
    field_at<0x14, uint32_t> entity_id;
    field_at<0x18, mm_entity_record *> entity_record;
    field_at<0x1f, bool> dead;
    field_at<0x28, mm_list_head> lh28;
    field_at<0xb4, vt_mm_entity *> vtable;
};

union mm_version_manager {
    field_at<0x08, uint32_t> version1;
    field_at<0x0c, uint32_t> version_to_use;
    field_at<0x10, uint32_t> version3;
};

union mm_state_callback {
    // just a C++ method pointer
    static constexpr size_t SIZE = 8;
    field_at<0, uint16_t> offset_to_this;
    field_at<2, uint16_t> callback_or_offset_to_vt;
    field_at<4, uint32_t> func;
};

template <typename T>
union mm_count_ptr {
    field_at<0, uint32_t> count;
    field_at<4, ds_arrayptr<T>> list;
};

union mm_state {
    field_at<0, const char *> name;

    field_at<8, uint32_t> name_cap;

    static constexpr size_t SIZE = 0x24;
};

union mm_statemgr {
    field_at<0x04, void *> vtable;
    field_at<0x08, int> counter;
    field_at<0x0c, int> state;
    field_at<0x10, int> old_state;
    field_at<0x18, uint32_t> state_count;

    field_at<0x20, mm_count_ptr<mm_state>> state_list;
    field_at<0x28, void *> target_obj;

    field_at<0x2c, mm_count_ptr<mm_state_callback>> cb_entry_list;
    field_at<0x34, mm_count_ptr<mm_state_callback>> cb_frame_list;
    field_at<0x3c, mm_count_ptr<mm_state_callback>> cb_exit_list;

};

union mm_ajit {
    field_at<0x00, void *> prev;
    field_at<0x04, void *> next;
    field_at<0x08, void *> pai1;
    field_at<0x0c, float> cur_frame;
    field_at<0x14, void *> vtable;
    field_at<0x24, float> speed;
};

// capital letters indicate (likely) actual names
union mm_WorldPlayKeeper;
union mm_Scene;

union mm_killer {
    field_at<0x6c, int> timer;
};
