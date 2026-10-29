#include "anco_ime_editor.h"

bool anco_ime_editor_is_password(int32_t type)
{
    switch ((uint32_t)type & 4095u) {
    case 129: case 145: case 225: case 18: case 128: case 144: case 224:
        return true;
    default: return false;
    }
}

int32_t anco_ime_editor_pattern(int32_t type)
{
    const uint32_t bits = (uint32_t)type;
    if (anco_ime_editor_is_password(type)) return 7;
    switch (bits & 15u) {
    case 1:
        if (bits & (131072u | 262144u)) return 1;
        switch (bits & 4080u) {
        case 16: return 6;
        case 32: return 5;
        case 128: case 144: return 7;
        default: return 0;
        }
    case 2: return (bits & 4080u) == 16u ? 8 : 2;
    case 3: return 3;
    case 4: return 2; /* Actual retained code, despite PATTERN_DATETIME=4. */
    default: return -1;
    }
}

int32_t anco_ime_editor_enter_key(int32_t type, int32_t options)
{
    if (anco_ime_editor_pattern(type) == 1 &&
        (options == 1 || options == 5 || options == 6)) return 8;
    switch (options) {
    case 0: case 2: case 3: case 4: case 5: case 6: case 7: return options;
    default: return 1;
    }
}

bool anco_ime_editor_same(const AncoImeEditor *a, const AncoImeEditor *b)
{
    if (!a || !b) return false;
    if (anco_ime_editor_pattern(a->input_type) !=
        anco_ime_editor_pattern(b->input_type)) return false;
    const int32_t ao = (uint32_t)a->ime_options & 255u;
    const int32_t bo = (uint32_t)b->ime_options & 255u;
    return ao == bo && anco_ime_editor_enter_key(a->input_type, ao) ==
                       anco_ime_editor_enter_key(b->input_type, bo);
}

bool anco_ime_editor_update_fields(AncoImeEditorFields *fields,
                                 bool show_keyboard,
                                 const AncoImeEditor *editor)
{
    if (!fields) return false;
    if (!editor) return true;
    fields->show_keyboard = show_keyboard;
    fields->option |= editor->ime_options & 255;
    fields->pattern |= anco_ime_editor_pattern(editor->input_type);
    fields->enter_key_type |= anco_ime_editor_enter_key(editor->input_type,
                                                      fields->option);
    return true;
}
