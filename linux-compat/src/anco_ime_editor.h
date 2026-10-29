/* Retained AnCo EditorInfoConverter / Utils.isSameEditorInfo semantics.
 * Inputs are original guest-semantic fields, not Wayland purpose enum values.
 * No endpoint, focus selection, host interface or attach lifecycle here.
 */
#ifndef ANCO_IME_EDITOR_H
#define ANCO_IME_EDITOR_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t input_type;
    int32_t ime_options;
} AncoImeEditor;

/* Scalar prefix of original ImfInputAttribute, not its complete Parcel.
 * The original private-command list is separate and must not be discarded by
 * a caller assembling the full attribute. Initialize to zero for a new object.
 */
typedef struct {
    bool show_keyboard;
    int32_t pattern, enter_key_type, option;
} AncoImeEditorFields;

/* Original setAttrByEditorInfo scalar operations, including OR-on-reuse.
 * NULL editor leaves existing fields unchanged. NULL fields returns false.
 * Does not send, attach, choose editor identity or process private commands.
 */
bool anco_ime_editor_update_fields(AncoImeEditorFields *fields,
                                 bool show_keyboard,
                                 const AncoImeEditor *editor);

bool anco_ime_editor_is_password(int32_t input_type);
int32_t anco_ime_editor_pattern(int32_t input_type);
/* Original converter uses its argument as-is; only callers mask options. */
int32_t anco_ime_editor_enter_key(int32_t input_type, int32_t ime_options);
/* Original attribute comparison, NOT editor/resource identity or a decision
 * to reuse attach. Actual restart and current connection gates remain required.
 * NULL on either side is unequal, including two NULL values.
 */
bool anco_ime_editor_same(const AncoImeEditor *current, const AncoImeEditor *previous);
#endif
